#include "workflow.hpp"
#include <algorithm>
#include "util.hpp"

namespace {
bool local(const std::string& who) { return who == "local1" || who == "local2"; }
bool contains(const Json& list, const std::string& value) {
    for (const auto& x : list.items()) if (x.str() == value) return true;
    return false;
}
void transfer(WorkflowRun& run, const std::string& from, const std::string& to,
              const std::string& label, long long now) {
    if (from.empty() || from == to) return;
    run.transfers.push_back(J({{"id", util::newId()}, {"from", from}, {"to", to}, {"label", label}, {"ts", now}}));
    if (run.transfers.size() > 8) run.transfers.erase(run.transfers.begin());
}
std::string phaseLabel(const std::string& phase) {
    if (phase == "review") return "審查";
    if (phase == "recheck") return "複審";
    if (phase == "selfcheck") return "自查";
    if (phase == "advise") return "唯讀審查";
    if (phase == "verify") return "驗證";
    return "撰寫";
}
}

void workflowTurn(WorkflowRun& run, const std::string& agent, const std::string& turnId,
                  const std::string& phase, long long now) {
    if (run.id.empty()) run.id = util::newId();
    if (local(agent)) {
        // Each local reviewer receives the writer's changes, not the other local's output.
        transfer(run, run.lastMain, agent, "交付唯讀審查", now);
    } else {
        transfer(run, run.lastMain, agent, phase == "write" ? "交接工作" : "送交" + phaseLabel(phase), now);
        for (const auto& reviewer : run.localReplies) transfer(run, reviewer, agent, "交付審查建議", now);
        run.localReplies.clear();
        run.lastMain = agent;
    }
    run.owner = agent;
    run.phase = phase;
    run.turns[agent] = turnId;
    run.outcome.clear();
}

void workflowLocalReply(WorkflowRun& run, const std::string& agent) {
    if (std::find(run.localReplies.begin(), run.localReplies.end(), agent) == run.localReplies.end()) run.localReplies.push_back(agent);
}

Json workflowView(const Json& context, const Json& state, const WorkflowRun& run, const std::vector<Json>& messages) {
    const Json& g = state["group"];
    Json nodes = Json::array();
    std::vector<std::string> members;
    const bool pipeline = !context["pipeline"]["nodes"].empty();
    std::map<std::string, Json> specs;
    if (pipeline) {
        int index = 0;
        for (auto node : context["pipeline"]["nodes"].items()) {
            const auto id = "pipeline:" + node["id"].str();
            node["index"] = index++;
            specs[id] = node;
            members.push_back(id);
        }
    } else if (context["kind"].str() == "group") {
        members = {"claude", "codex"};
        if (contains(context["members"], "gemini")) members.push_back("gemini");
        for (const char* id : {"local1", "local2"}) {
            if (contains(context["members"], id) || contains(state["typing"], id)) members.push_back(id);
        }
    } else {
        // Only participants in this run count. A Claude DM may already be led by GPT
        // after an earlier takeover; its original kind does not mean Claude joined again.
        for (const char* id : {"claude", "codex"}) {
            if (run.turns.count(id) || contains(state["typing"], id)) members.push_back(id);
        }
        if (members.size() < 2) members.clear();
    }
    std::string runStatus = state["running"].boolean() ? "running" : "idle";
    std::string runLabel = runStatus == "running" ? phaseLabel(run.phase) + "中" : "等待工作";
    for (const auto& who : members) {
        const auto& spec = specs[who];
        const auto agent = pipeline ? spec["backend"].str() : who;
        const bool current = !pipeline || spec["index"].i64() == state["task"]["cursor"].i64();
        const Json* msg = nullptr;
        auto turn = run.turns.find(who);
        if (turn != run.turns.end()) for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if ((*it)["id"].str() == turn->second) { msg = &*it; break; }
        }
        bool active = current && contains(state["typing"], agent);
        std::string status = "waiting", label = "待命", detail;
        if (msg && !(*msg)["streaming"].boolean()) {
            std::string result = (*msg)["status"].str();
            status = result == "failed" ? "failed" : result == "interrupted" ? "stopped" : "done";
            label = status == "failed" ? "沒有完成" : status == "stopped" ? "已停止" : "已完成";
        }
        if (local(agent) && context["models"][agent].str().empty() && !msg && !active) {
            status = "unavailable"; label = "未設定模型";
        }
        if (current && (state["preparing"].str() == agent || (pipeline && state["preparing"].str() == "checkpoint"))) {
            status = "preparing";
            label = "建立檢查點";
        }
        if (current && g["localActive"].str() == agent && !active) {
            status = "loading";
            label = "準備本地模型";
        }
        if (active) {
            status = state["thinking"][agent].boolean() ? "thinking" : local(agent) ? "reviewing" : "writing";
            label = status == "thinking" ? "思考中"
                    : local(agent)       ? "唯讀審查中"
                                         : phaseLabel(msg ? (*msg)["phase"].str("write") : run.phase) + "中";
            if (msg) {
                for (const auto& block : (*msg)["blocks"].items()) {
                    if (block["type"].str() == "tool" && block["status"].str() == "running") {
                        status = "tool"; label = "使用工具"; detail = block["title"].str();
                    }
                }
                if ((*msg)["retry"].isObject()) { status = "retry"; label = "重試連線"; }
                for (const auto& block : (*msg)["blocks"].items()) {
                    std::string type = block["type"].str();
                    if ((type == "approval" || type == "question") && block["state"].str() == "pending") {
                        status = "waitingUser"; label = type == "approval" ? "等你核准" : "等你回答"; detail = block["title"].str();
                    }
                }
            }
        }
        if (g["paused"].boolean() && (who == run.owner || (current && agent == g["current"].str()))) {
            status = "paused"; label = "協作暫停"; detail = g["pauseReason"].str();
        }
        if (g["stopRequested"].boolean() && active) { status = "stopping"; label = "正在停止"; }
        std::string role = local(agent)                           ? "唯讀審查"
                           : context["kind"].str() != "group"     ? "對話"
                           : context["lead"].str("claude") == who ? "主刀"
                                                                  : "審查";
        if (pipeline) role = phaseLabel(spec["role"].str());
        nodes.push(J({{"id", who},
                      {"agent", agent},
                      {"name", pipeline ? spec["name"] : Json()},
                      {"profile", context["names"][agent]},
                      {"status", status},
                      {"label", label},
                      {"detail", detail},
                      {"role", role},
                      {"model", msg ? (*msg)["resolvedModel"].str((*msg)["model"].str())
                                    : (pipeline ? spec["model"].str() : context["models"][agent].str())}}));
        if (state["running"].boolean() && (active || status == "preparing" || status == "loading")) runLabel = label;
        if (who == run.owner && !state["running"].boolean()) { runStatus = status; runLabel = label; }
        if (status == "waitingUser") { runStatus = "waitingUser"; runLabel = label; }
    }
    if (g["paused"].boolean()) { runStatus = "paused"; runLabel = "協作暫停"; }
    if (g["stopRequested"].boolean()) { runStatus = "stopping"; runLabel = "正在停止"; }
    if (!run.outcome.empty()) {
        runStatus = run.outcome == "consensus" || run.outcome == "completed" ? "done" : run.outcome == "stopped" ? "stopped" : "failed";
        runLabel = runStatus == "done" ? "協作完成" : runStatus == "stopped" ? "已停止協作" : "協作結束";
    }
    long long added = 0, removed = 0;
    for (const auto& f : g["files"].fields()) { added += f.second["added"].i64(); removed += f.second["removed"].i64(); }
    long long round = g["round"].i64(), files = (long long)g["files"].fields().size();
    if (!run.outcome.empty()) { added = run.added; removed = run.removed; files = run.files; round = run.round; }
    Json transfers = Json::array();
    for (const auto& event : run.transfers) transfers.push(event);
    return J({{"layout", pipeline ? "pipeline" : "classic"},
              {"runId", run.id},
              {"owner", run.owner},
              {"status", runStatus},
              {"label", runLabel},
              {"phase", run.phase},
              {"nodes", nodes},
              {"transfers", transfers},
              {"round", round},
              {"files", files},
              {"added", added},
              {"removed", removed}});
}
