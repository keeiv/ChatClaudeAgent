// Rolling a conversation back to before one of its messages: project files from
// the checkpoint taken before that turn, and/or the chat itself (both agents forget
// everything from that point on).
#include "app.hpp"
#include "util.hpp"

void App::handleRollback(const Json& m) {
    Conversation* cp = find(m["convId"].str());
    if (!cp) return;
    Conversation& c = *cp;
    if (anyBusy(c) || c.group.active) {
        toast("error", "AI 正在工作，請先停止再回滾");
        return;
    }
    std::string msgId = m["msgId"].str();
    bool files = m["files"].boolean();
    bool chat = m["chat"].boolean();
    const Json& savedTask = c.task;
    bool pipeline = savedTask["pipeline"]["nodes"].size() > 0;
    size_t idx = c.messages.size();
    for (size_t i = 0; i < c.messages.size(); ++i) {
        if (c.messages[i]["id"].str() == msgId) { idx = i; break; }
    }
    if (idx == c.messages.size()) return;

    Json summary = J({{"author", "system"}, {"kind", "rollback"}});
    std::string text;
    if (files) {
        // The checkpoint of the first agent turn at or after the chosen message is the
        // state of the project just before that point.
        std::string snap;
        for (size_t i = idx; i < c.messages.size() && snap.empty(); ++i) snap = c.messages[i]["snapshot"].str();
        if (snap.empty()) {
            toast("error", "這則訊息之後沒有檢查點，無法還原檔案");
            return;
        }
        RestoreResult r = snapshots_.restore(c.id, c.cwd, snap);
        if (!r.ok) {
            toast("error", r.error);
            return;
        }
        Json restored = Json::array(), removed = Json::array();
        for (const auto& p : r.restored) restored.push(p);
        for (const auto& p : r.removed) removed.push(p);
        summary["backup"] = r.backup;
        summary["restored"] = restored;
        summary["removed"] = removed;
        invalidatePipelineTask(c, snap, "使用者已回滾檔案；以目前 Git 版本為準，必要測試須重跑");
        text = "已把專案檔案還原到這則訊息之前（還原 " + std::to_string(r.restored.size()) + " 個、刪除 " +
               std::to_string(r.removed.size()) + " 個之後新增的檔案）";
    }
    if (chat) {
        if (pipeline) {
            clearPipelineSessions(c);
            c.task = Json();
            deleteTask(c.id);
        } else {
            // Claude resumes cut after its last reply before the point; Codex drops the first
            // turn at or after it. With nothing earlier, the agent starts a fresh session.
            std::string uuid;
            bool claudeBefore = false;
            for (size_t i = 0; i < idx; ++i) {
                if (c.messages[i]["author"].str() != "claude") continue;
                claudeBefore = true;
                if (!c.messages[i]["claudeUuid"].str().empty()) uuid = c.messages[i]["claudeUuid"].str();
            }
            if (claudeBefore && !uuid.empty()) {
                claudeFor(c).rewindTo(uuid);  // creates the session object after a restart
            } else {
                c.claudeSession.clear();
                if (c.claude) c.claude->rewindTo("");
            }
            std::string turn;
            bool codexBefore = false;
            for (size_t i = 0; i < c.messages.size(); ++i) {
                if (c.messages[i]["author"].str() != "codex") continue;
                if (i < idx) codexBefore = true;
                else if (turn.empty()) turn = c.messages[i]["codexTurn"].str();
            }
            if (!codexBefore) {
                if (c.codex) {
                    c.codex->shutdown();
                    c.codex.reset();
                }
                c.codexThread.clear();
            } else if (!turn.empty()) {
                std::string err;
                if (CodexThread* t = codexFor(c, &err)) {
                    std::string id = c.id;
                    t->revert(turn, [this, id](bool ok, const std::string& e) {
                        if (!ok) toast("error", "GPT 的對話紀錄沒有回滾成功：" + e);
                    });
                }
            }
        }
        c.messages.erase(c.messages.begin() + idx, c.messages.end());
        c.group = GroupRun();
        c.workflow = WorkflowRun();
        text += text.empty() ? "已回滾聊天：這則訊息和之後的內容都刪除了，AI 也不記得" : "；這則訊息之後的聊天也刪除了";
    }
    if (text.empty()) return;
    summary["text"] = text;
    pushMessage(c, summary);
    metaDirty_ = true;
    saveMessages(c);
    Json arr = Json::array();
    for (const auto& msg : c.messages) arr.push(msg);
    send(J({{"type", "history"}, {"id", c.id}, {"messages", arr}}));
    c.stateDirty = true;
    toast("ok", "回滾完成");
}

void App::handleRollbackUndo(const Json& m) {
    Conversation* cp = find(m["convId"].str());
    if (!cp) return;
    Conversation& c = *cp;
    if (anyBusy(c) || c.group.active) {
        toast("error", "AI 正在工作，請先停止");
        return;
    }
    RestoreResult r = snapshots_.restore(c.id, c.cwd, m["backup"].str());
    if (!r.ok) {
        toast("error", r.error);
        return;
    }
    pushMessage(c, J({{"author", "system"}, {"kind", "info"},
                      {"text", "已復原檔案到回滾之前的狀態（聊天紀錄不會恢復）"}}));
    invalidatePipelineTask(c, m["backup"].str(), "使用者復原了檔案；以目前 Git 版本為準，必要測試須重跑");
    toast("ok", "已復原檔案");
}
