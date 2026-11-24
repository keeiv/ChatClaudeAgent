// Scheduled requests and the worktree merge queue. Both run only while CCA is open, never skip a confirmation, and
// leave pushing or opening PRs to the user; a schedule is the user's own request sent at a chosen time.
#include "app.hpp"

#include <windows.h>

#include <thread>

#include "util.hpp"
#include "workspaces.hpp"

namespace {

std::string localDay(long long ms) {
    ULARGE_INTEGER value;
    value.QuadPart = (unsigned long long)(ms / 1000 + 11644473600LL) * 10000000ULL;
    FILETIME ft{value.LowPart, value.HighPart}, local;
    SYSTEMTIME t;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return buf;
}

}  // namespace

Json sanitizeSchedule(const Json& in) {
    Json days = Json::array();
    for (const auto& d : in["days"].items())
        if (d.isNumber() && d.i64() >= 0 && d.i64() <= 6) days.push(d.i64());
    const std::string time = in["time"].str();
    const bool clock = time.size() == 5 && time[2] == ':' && isdigit((unsigned char)time[0]) &&
                       isdigit((unsigned char)time[1]) && isdigit((unsigned char)time[3]) &&
                       isdigit((unsigned char)time[4]) && std::stoi(time.substr(0, 2)) < 24 &&
                       std::stoi(time.substr(3)) < 60;
    const std::string text = util::trim(in["text"].str());
    if (!clock || days.empty() || text.empty() || text.size() > 4000 || in["convId"].str().empty()) return Json();
    return J({{"id", in["id"].str().empty() ? util::newId() : in["id"].str()},
              {"convId", in["convId"].str()},
              {"text", text},
              {"time", time},
              {"days", days},
              {"enabled", in["enabled"].boolean(true)},
              {"lastRun", in["lastRun"].str()}});
}

void App::tickSchedules(long long now) {
    Json& list = settings_["schedules"];
    if (!list.isArray() || list.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char clock[8];
    std::snprintf(clock, sizeof clock, "%02d:%02d", t.wHour, t.wMinute);
    const std::string today = localDay(now);
    for (auto& s : list.items()) {
        if (!s["enabled"].boolean(true) || s["lastRun"].str() == today || s["time"].str() > clock) continue;
        bool day = false;
        for (const auto& d : s["days"].items()) day |= d.i64() == t.wDayOfWeek;
        if (!day) continue;
        // Late by more than an hour (CCA was closed): skipped for today rather than run at an unexpected time.
        const int due = std::stoi(s["time"].str().substr(0, 2)) * 60 + std::stoi(s["time"].str().substr(3));
        s["lastRun"] = today;
        metaDirty_ = true;
        Conversation* c = find(s["convId"].str());
        if (!c) continue;
        if (t.wHour * 60 + t.wMinute - due > 60) continue;
        if (anyBusy(*c) || c->group.active) {
            pushMessage(*c, J({{"author", "system"},
                               {"kind", "info"},
                               {"text", "排程任務在 " + s["time"].str() + " 時對話正在工作，這次沒有送出：" +
                                            util::truncateUtf8(s["text"].str(), 120)}}));
            continue;
        }
        pushMessage(*c,
                    J({{"author", "system"}, {"kind", "info"}, {"text", "排程任務（" + s["time"].str() + "）開始。"}}));
        const std::string text = s["text"].str();
        if (c->kind == "group") {
            startGroupChecked(*c, text);
        } else {
            const AgentKind k = c->kind == "gemini"                         ? AgentKind::Gemini
                                : c->kind == "cursor"                       ? AgentKind::Cursor
                                : c->kind == "claude" && c->lead != "codex" ? AgentKind::Claude
                                                                            : AgentKind::Codex;
            pushMessage(*c, J({{"author", "user"}, {"text", text}}));
            std::string error;
            if (!sendToAgent(*c, k, text, &error))
                pushMessage(*c, J({{"author", "system"}, {"kind", "error"}, {"text", error}}));
        }
        notify(*c, "排程任務開始", util::truncateUtf8(text, 200));
    }
}

void App::tickMergeQueue(long long now) {
    if (mergeRunning_ || now - lastMergeTick_ < 5000) return;
    lastMergeTick_ = now;
    // The oldest queued task whose agents are idle; one merge at a time, each against the target's current head.
    Conversation* next = nullptr;
    for (auto& c : convs_) {
        const Json& w = c->collaboration["workspace"];
        if (!c->collaboration["mergeQueued"].isNumber() || !w["ok"].boolean() || anyBusy(*c) || c->group.active)
            continue;
        if (!next || c->collaboration["mergeQueued"].i64() < next->collaboration["mergeQueued"].i64()) next = c.get();
    }
    if (!next) return;
    mergeRunning_ = true;
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, id = next->id, workspace = next->collaboration["workspace"]] {
        Json preview = checkTaskMerge(workspace, false, "", "");
        Json result = preview["ok"].boolean() && preview["clean"].boolean()
                          ? checkTaskMerge(workspace, true, preview["target"].str(), preview["source"].str())
                          : preview;
        postToUi([this, life, id, preview, result] {
            if (life.expired()) return;
            mergeRunning_ = false;
            Conversation* c = find(id);
            if (!c) return;
            Json next = c->collaboration;
            next.erase("mergeQueued");
            next["workspace"]["check"] = result["ok"].boolean() ? result : preview;
            persistCollaboration(*c, next);
            std::string text;
            if (result["merged"].boolean())
                text = "合併佇列：已把這個任務的已提交變更合併回 " +
                       c->collaboration["workspace"]["targetBranch"].str() + "。";
            else if (preview["ok"].boolean() && !preview["clean"].boolean())
                text = "合併佇列：和目前的目標有衝突，已移出佇列；可以請 AI 在 worktree 解衝突後重新排隊。";
            else
                text =
                    "合併佇列：沒有合併（" + result["error"].str(preview["error"].str("未知原因")) + "），已移出佇列。";
            pushMessage(
                *c, J({{"author", "system"}, {"kind", result["merged"].boolean() ? "info" : "error"}, {"text", text}}));
            notify(*c, result["merged"].boolean() ? "合併完成" : "合併沒有完成", text);
            c->stateDirty = true;
        });
    }).detach();
}

bool App::handleSchedules(const Json& m) {
    const std::string type = m["type"].str();
    if (type == "schedule.save") {
        Json clean = sanitizeSchedule(m["schedule"]);
        if (clean.isNull()) {
            toast("error", "排程需要對話、HH:MM 時間、至少一天，以及 4000 字以內的需求");
            return true;
        }
        if (!find(clean["convId"].str())) {
            toast("error", "找不到這個對話");
            return true;
        }
        Json& list = settings_["schedules"];
        if (!list.isArray()) list = Json::array();
        bool replaced = false;
        for (auto& s : list.items())
            if (s["id"].str() == clean["id"].str()) {
                clean["lastRun"] = s["lastRun"];
                s = clean;
                replaced = true;
            }
        if (!replaced) {
            if (list.size() >= 30) {
                toast("error", "排程最多 30 個");
                return true;
            }
            list.push(clean);
        }
        metaDirty_ = true;
        sendSettings();
        return true;
    }
    if (type == "schedule.remove") {
        Json kept = Json::array();
        for (const auto& s : settings_["schedules"].items())
            if (s["id"].str() != m["scheduleId"].str()) kept.push(s);
        settings_["schedules"] = kept;
        metaDirty_ = true;
        sendSettings();
        return true;
    }
    if (type == "worktree.queue") {
        Conversation* c = find(m["id"].str());
        if (!c || !c->collaboration["workspace"]["ok"].boolean()) {
            toast("error", "尚未建立 worktree");
            return true;
        }
        Json next = c->collaboration;
        if (m["enabled"].boolean(true)) next["mergeQueued"] = util::nowMs();
        else next.erase("mergeQueued");
        persistCollaboration(*c, next);
        c->stateDirty = true;
        toast("info",
              m["enabled"].boolean(true) ? "已加入合併佇列；輪到時先檢查衝突，沒有衝突才合併" : "已移出合併佇列");
        return true;
    }
    return false;
}
