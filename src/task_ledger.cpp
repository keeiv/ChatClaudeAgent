#include "task_ledger.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "util.hpp"

namespace {

const size_t kMaxHandoffs = 40;

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 100) return false;
    for (unsigned char c : id)
        if (!isalnum(c) && c != '-' && c != '_') return false;
    return true;
}

std::wstring historyDir(const std::string& taskId) {
    std::wstring dir = util::joinPath(util::joinPath(util::appDataDir(), L"task-history"), util::widen(taskId));
    util::ensureDir(dir);
    return dir;
}

std::wstring taskFile(const std::string& convId) {
    std::wstring dir = util::joinPath(util::appDataDir(), L"tasks");
    util::ensureDir(dir);
    return util::joinPath(dir, util::widen(convId + ".json"));
}

bool isTestCommand(const std::string& cmd) {
    std::string s = util::toLower(cmd);
    for (const char* k :
         {"test", "pytest", "jest", "vitest", "ctest", "check", "lint", "build.bat", "tsc", "cargo build"}) {
        if (s.find(k) != std::string::npos) return true;
    }
    return false;
}

std::string finalText(const Json& msg) {
    std::string out;
    for (const auto& b : msg["blocks"].items()) {
        if (b["type"].str() == "text") out += (out.empty() ? "" : "\n\n") + b["text"].str();
    }
    return util::trim(out);
}

}  // namespace

Json newTask(const std::string& convId, const std::string& request, long long now) {
    return J({{"task_id", util::newId()},
              {"conv_id", convId},
              {"request", request},
              {"status", "running"},
              {"created", now},
              {"updated", now},
              {"handoffs", Json::array()},
              {"earlier", 0}});
}

Json handoffFromTurn(const Json& task, const Json& msg, const std::string& agent, const std::string& role,
                     const Json& files, const std::string& verdict, long long now) {
    Json changed = Json::array();
    for (const auto& f : files.items()) {
        changed.push(
            J({{"path", f["path"]}, {"change", f["change"]}, {"added", f["added"]}, {"removed", f["removed"]}}));
    }
    // Commands that ran, with exit codes: test runs are what reviewers check first.
    Json tests = Json::array();
    for (const auto& b : msg["blocks"].items()) {
        if (b["type"].str() != "tool" || b["command"].str().empty()) continue;
        std::string cmd = util::truncateUtf8(
            util::splitLines(b["command"].str()).empty() ? "" : util::splitLines(b["command"].str())[0], 300);
        Json t = J({{"command", cmd}, {"status", b["status"]}, {"test", isTestCommand(cmd)}});
        t["output_reference"] = "message:" + msg["id"].str() + "/block:" + b["id"].str();
        t["output_excerpt"] = util::tailUtf8(b["output"].str(), b["status"].str() == "ok" ? 800 : 2000);
        if (b.has("exitCode")) t["exit_code"] = b["exitCode"];
        tests.push(t);
    }
    Json errors = Json::array();
    for (const auto& block : msg["blocks"].items())
        if (block["type"].str() == "error" && errors.size() < 8)
            errors.push(J({{"text", util::truncateUtf8(block["text"].str(), 2000)},
                           {"reference", "message:" + msg["id"].str() + "/block:" + block["id"].str()}}));
    std::string summary = finalText(msg);
    if (summary.empty() && errors.size()) summary = errors.at(0)["text"].str();
    Json issues = Json::array();
    if (verdict == "issues") issues.push(util::truncateUtf8(summary, 800));
    std::string base = msg["snapshot"].str();
    return J(
        {{"task_id", task["task_id"]},
         {"agent_id", agent},
         {"role", role},
         {"message_id", msg["id"]},
         {"base_revision", base},
         {"head_revision", ""},
         {"changed_files", changed},
         // The full diff stays in the shadow checkpoint repository and the message itself.
         {"diff_reference", base.empty() ? "message:" + msg["id"].str() : "snapshot:" + base + "..(next checkpoint)"},
         {"summary", util::truncateUtf8(summary, 1500)},
         {"issues", issues},
         {"test_results", tests},
         {"errors", errors},
         {"verdict", verdict},
         {"next_action", ""},
         {"ts", now}});
}

void addHandoff(Json& task, Json handoff) {
    Json& list = task["handoffs"];
    if (!list.isArray()) list = Json::array();
    handoff["seq"] = (long long)(task["earlier"].i64() + list.size() + 1);
    list.push(std::move(handoff));
    auto& items = list.items();
    while (items.size() > kMaxHandoffs) {
        // Keep unresolved findings and decisions in the brief; the full event remains indexed on disk.
        Json& memory = task["memory"];
        if (!memory.isArray()) memory = Json::array();
        if (items.front()["issues"].size())
            memory.push(J({{"seq", items.front()["seq"]}, {"issues", items.front()["issues"]}}));
        items.erase(items.begin());
        task["earlier"] = task["earlier"].i64() + 1;
    }
    task["updated"] = util::nowMs();
}

void setNextAction(Json& task, const std::string& action) {
    auto& items = task["handoffs"].items();
    if (!items.empty() && items.back()["next_action"].str().empty()) items.back()["next_action"] = action;
    task["updated"] = util::nowMs();
}

void setHeadRevision(Json& task, const std::string& revision) {
    auto& items = task["handoffs"].items();
    if (items.empty() || revision.empty() || !items.back()["head_revision"].str().empty()) return;
    items.back()["head_revision"] = revision;
    std::string base = items.back()["base_revision"].str();
    if (!base.empty()) items.back()["diff_reference"] = "snapshot:" + base + ".." + revision;
}

Json taskView(const Json& task) {
    Json list = Json::array();
    const auto& items = task["handoffs"].items();
    size_t from = items.size() > 10 ? items.size() - 10 : 0;
    for (size_t i = from; i < items.size(); ++i) {
        const Json& h = items[i];
        long long ran = 0, failed = 0;
        for (const auto& t : h["test_results"].items()) {
            if (!t["test"].boolean()) continue;
            ++ran;
            if (t["exit_code"].i64(0) != 0 || (t["status"].str() != "ok" && !t["status"].str().empty())) ++failed;
        }
        list.push(J({{"seq", h["seq"]},
                     {"agent", h["agent_id"]},
                     {"role", h["role"]},
                     {"verdict", h["verdict"]},
                     {"files", (long long)h["changed_files"].size()},
                     {"tests", ran},
                     {"failed", failed},
                     {"base", h["base_revision"]},
                     {"head", h["head_revision"]},
                     {"next", h["next_action"]},
                     {"issues", (long long)h["issues"].size()},
                     {"ts", h["ts"]}}));
    }
    return J({{"task_id", task["task_id"]},
              {"status", task["status"]},
              {"quotaStop", task["quota_stop"]},
              {"skipped", task["skipped"]},
              {"earlier", task["earlier"]},
              {"count", (long long)(task["earlier"].i64() + items.size())},
              {"handoffs", list},
              {"pipeline", task["pipeline"]},
              {"cursor", task["cursor"]},
              {"cycle", task["cycle"]},
              {"gate", task["gate"]},
              {"artifacts", task["artifacts"]},
              {"head_revision", task["head_revision"]},
              {"blocked_reason", task["blocked_reason"]},
              {"conflict_reference", task["conflict_reference"]},
              {"tests_passed", task["tests_passed"]},
              {"costs", task["costs"]}});
}

std::string taskBrief(const Json& task, size_t maxBytes) {
    if (!task.isObject()) return "";
    std::string head = "【任務紀錄｜" + task["task_id"].str() + "】狀態：" + task["status"].str() + "\n";
    if (task["latest_update"].isObject())
        head += "最新使用者補充（優先套用；完整內容見交接 #" + std::to_string(task["latest_update"]["seq"].i64()) +
                "）：" + util::truncateUtf8(task["latest_update"]["text"].str(), std::min((size_t)2000, maxBytes / 4)) +
                "\n";
    head += "原始需求：" + util::truncateUtf8(task["request"].str(), std::min((size_t)2000, maxBytes / 4)) + "\n";
    if (!task["head_revision"].str().empty()) head += "目前交接版本：" + task["head_revision"].str() + "\n";
    if (!task["conflict_reference"].str().empty()) head += "工作區裁決差異：" + task["conflict_reference"].str() + "\n";
    if (task["memory"].size())
        head += "較早的未解問題索引（完整內容按需查交接紀錄）：" +
                util::truncateUtf8(task["memory"].dump(), std::min((size_t)1000, maxBytes / 4)) + "\n";
    if (!task["blocked_reason"].str().empty())
        head += "待處理：" + util::truncateUtf8(task["blocked_reason"].str(), 500) + "\n";
    const auto& artifacts = task["artifacts"].items();
    for (size_t i = artifacts.size() > 5 ? artifacts.size() - 5 : 0; i < artifacts.size(); ++i)
        head += "圖片證據 id=" + artifacts[i]["id"].str() + " file=" + artifacts[i]["file"].str() + " " +
                artifacts[i]["result"].str() + " " + util::truncateUtf8(artifacts[i]["operation"].str(), 100) + "\n";
    // Newest first until the budget runs out, then printed oldest to newest.
    std::vector<std::string> parts;
    size_t used = head.size();
    const auto& items = task["handoffs"].items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const Json& h = *it;
        std::string s =
            "#" + std::to_string(h["seq"].i64()) + " " + h["agent_id"].str() + "（" + h["role"].str() + "）";
        if (!h["verdict"].str().empty()) s += " 結論 " + h["verdict"].str();
        if (!h["base_revision"].str().empty())
            s += " 版本 " + h["base_revision"].str().substr(0, 8) +
                 (h["head_revision"].str().empty() ? "" : "→" + h["head_revision"].str().substr(0, 8));
        s += "\n";
        if (h["changed_files"].size()) {
            s += "  修改：";
            for (const auto& f : h["changed_files"].items()) {
                s += f["path"].str() + "(+" + std::to_string(f["added"].i64()) + "−" +
                     std::to_string(f["removed"].i64()) + ") ";
            }
            s += "\n";
        }
        int ran = 0, failed = 0;
        std::string which;
        for (const auto& t : h["test_results"].items()) {
            if (!t["test"].boolean()) continue;
            ++ran;
            bool bad = t["exit_code"].i64(0) != 0 || (t["status"].str() != "ok" && !t["status"].str().empty());
            if (bad) ++failed;
            if (ran <= 3) {
                which += (which.empty() ? "" : "、") + util::truncateUtf8(t["command"].str(), 80) + "（" +
                         (t.has("exit_code") ? "結束碼 " + std::to_string(t["exit_code"].i64()) : t["status"].str()) +
                         "）";
            }
        }
        if (ran) s += "  測試：" + std::to_string(ran) + " 次，失敗 " + std::to_string(failed) + "：" + which + "\n";
        if (h["issues"].size()) s += "  問題：" + util::truncateUtf8(h["issues"].at(0).str(), 400) + "\n";
        s += "  摘要：" +
             util::truncateUtf8(util::replaceAll(h["summary"].str(), "\n", " "), it == items.rbegin() ? 700 : 260) +
             "\n";
        if (!h["next_action"].str().empty()) s += "  下一步：" + h["next_action"].str() + "\n";
        if (used + s.size() > maxBytes) break;
        used += s.size();
        parts.push_back(s);
    }
    std::string out = head;
    size_t skipped = task["earlier"].i64() + items.size() - parts.size();
    if (skipped) out += "（較早的 " + std::to_string(skipped) + " 次交接只留在紀錄檔，需要時再查）\n";
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += *it;
    return util::truncateUtf8(out, maxBytes);
}

bool saveTask(const Json& task) {
    if (!task.isObject() || !validId(task["conv_id"].str()) || !validId(task["task_id"].str())) return false;
    std::wstring dir = historyDir(task["task_id"].str());
    if (!util::fileExists(util::joinPath(dir, L"request.json")) &&
        !util::writeFileAtomic(
            util::joinPath(dir, L"request.json"),
            J({{"request", task["request"]}, {"acceptance", task["acceptance"]}, {"created", task["created"]}}).dump()))
        return false;
    for (const auto& h : task["handoffs"].items()) {
        std::wstring file = util::joinPath(dir, util::widen(std::to_string(h["seq"].i64()) + ".json"));
        std::string previous;
        if (!util::readFile(file, previous) || previous != h.dump()) {
            // Preserve the event version before updating the indexed latest copy.
            if (!previous.empty() && !util::writeFileAtomic(file + L"." + util::widen(util::newId()), previous))
                return false;
            if (!util::writeFileAtomic(file, h.dump())) return false;
        }
    }
    return util::writeFileAtomic(taskFile(task["conv_id"].str()), task.dump());
}

Json loadTask(const std::string& convId) {
    if (!validId(convId)) return Json();
    std::string text;
    if (!util::readFile(taskFile(convId), text)) return Json();
    Json t = Json::parse(text);
    return t.isObject() ? t : Json();
}

void deleteTask(const std::string& convId) {
    if (!validId(convId)) return;
    DeleteFileW(taskFile(convId).c_str());
}

Json taskHistory(const std::string& taskId, long long after, size_t limit) {
    Json out = Json::array();
    if (!validId(taskId)) return out;
    std::wstring dir = historyDir(taskId);
    // Sequence numbers are assigned by the UI-thread task owner, never supplied by a tool.
    for (long long n = std::max(0LL, after) + 1; out.size() < std::min(limit, (size_t)50); ++n) {
        std::string text;
        if (!util::readFile(util::joinPath(dir, util::widen(std::to_string(n) + ".json")), text)) break;
        Json h = Json::parse(text);
        if (h.isObject()) out.push(h);
    }
    return out;
}
