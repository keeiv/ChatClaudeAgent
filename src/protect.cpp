// Protected paths: files an agent may not change on its own. After each turn the host compares the turn's checkpoints,
// puts matching files back and keeps the agent's version in a checkpoint; model claims about edits are not consulted.
#include "app.hpp"

#include <cctype>
#include <thread>

#include "gitflow.hpp"
#include "util.hpp"

namespace {

std::string lowerSlashes(std::string s) {
    for (auto& ch : s) {
        if (ch == '\\') ch = '/';
        ch = (char)tolower((unsigned char)ch);
    }
    return s;
}

}  // namespace

std::string sanitizeProtectedPattern(const std::string& raw) {
    std::string p = lowerSlashes(util::trim(raw));
    while (util::startsWith(p, "./")) p = p.substr(2);
    if (p.empty() || p.size() > 200 || p[0] == '/' || p.find(':') != std::string::npos) return "";
    for (unsigned char ch : p)
        if (ch < 0x20 || ch == 0x7f) return "";
    for (const auto& part : {std::string("/../"), std::string("../")})
        if (p.find(part) != std::string::npos || p == "..") return "";
    if (util::endsWith(p, "/..")) return "";
    return p;
}

bool protectedMatch(const std::string& pattern, const std::string& path) {
    const std::string p = sanitizeProtectedPattern(pattern), f = lowerSlashes(path);
    if (p.empty() || f.empty()) return false;
    // The same .gitignore-style rules as the never-commit list, so one pattern means the same thing in both places.
    return gitflow::globMatch(p, f);
}

bool App::protectTurn(Conversation& c, AgentKind k, const AgentEvent& done) {
    if (isLocal(k) || done.data["protectChecked"].boolean() || c.protectedPaths.empty() || !snapshots_.available() ||
        c.cwd.empty())
        return false;
    const Json* msg = findMsg(c, c.activeMsg[(int)k]);
    const std::string base = msg ? (*msg)["snapshot"].str() : "";
    if (base.empty()) return false;
    c.protecting = true;
    c.stateDirty = true;
    std::weak_ptr<int> life = lifetime_;
    Snapshots snaps = snapshots_;
    std::thread([this, life, snaps, convId = c.id, folder = c.cwd, patterns = c.protectedPaths, base, k,
                 done]() mutable {
        Json reverted = Json::array();
        std::string result, failure;
        SnapshotResult after = snaps.take(convId, folder, std::string("protected paths: ") + agentName(k) + " result");
        if (!after.ok) failure = after.error;
        Json delta = after.ok ? snaps.compare(convId, folder, base, after.commit) : Json();
        for (const auto& f : delta["files"].items()) {
            const std::string path = f["path"].str();
            bool hit = false;
            for (const auto& p : patterns) hit |= protectedMatch(p, path);
            if (!hit) continue;
            std::string error;
            if (Snapshots::safeRelativePath(path) && snaps.restorePath(convId, folder, base, path, &error))
                reverted.push(path);
            else failure = path + "：" + error;
        }
        result = after.commit;
        postToUi([this, life, convId, k, done, reverted, result, failure]() mutable {
            if (life.expired()) return;
            Conversation* c = find(convId);
            if (!c) return;
            c->protecting = false;
            c->stateDirty = true;
            if (reverted.size()) {
                std::string list;
                for (const auto& p : reverted.items()) list += (list.empty() ? "" : "、") + p.str();
                pushMessage(
                    *c, J({{"author", "system"},
                           {"kind", "protected"},
                           {"agent", agentName(k)},
                           {"files", reverted},
                           {"checkpoint", result},
                           {"text", "已還原 " + nameOf(k) + " 對受保護路徑的修改：" + list + "。它的版本保存在檢查點 " +
                                        result.substr(0, 8) + "，需要時可以從回滾或審查窗格找回。"}}));
                // Reviewers see only what is still changed; the ledger keeps the refusal.
                Json kept = Json::array();
                for (const auto& f : c->group.turnFiles.items()) {
                    bool gone = false;
                    for (const auto& p : reverted.items()) gone |= f["path"].str() == p.str();
                    if (!gone) kept.push(f);
                }
                c->group.turnFiles = kept;
                if (c->task.isObject()) {
                    if (!c->task["protected_reverts"].isArray()) c->task["protected_reverts"] = Json::array();
                    c->task["protected_reverts"].push(J(
                        {{"agent", agentName(k)}, {"files", reverted}, {"checkpoint", result}, {"ts", util::nowMs()}}));
                    saveTask(c->task);
                }
            }
            if (!failure.empty())
                pushMessage(*c, J({{"author", "system"},
                                   {"kind", "error"},
                                   {"text", "受保護路徑沒有檢查完成（沒有驗證）：" + failure}}));
            AgentEvent next = done;
            next.data["protectChecked"] = true;
            onAgentEvent(convId, k, next);
        });
    }).detach();
    return true;
}
