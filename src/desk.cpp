// Durable user queues and a bounded record of inputs observable at the CCA boundary.
#include "app.hpp"

#include <algorithm>

#include "util.hpp"

namespace {
std::wstring deskFile(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }))
        return L"";
    auto dir = util::joinPath(util::appDataDir(), L"desk");
    util::ensureDir(dir);
    return util::joinPath(dir, util::widen(id + ".json"));
}
Json defaults() {
    return J({{"queue", Json::array()}, {"contexts", Json::array()}});
}
Json excerpt(const std::string& text) {
    return J({{"text", util::truncateUtf8(text, 65536)},
              {"bytes", (long long)text.size()},
              {"truncated", text.size() > 65536}});
}
}  // namespace

void App::loadDesk(Conversation& c) {
    std::string raw;
    c.desk = defaults();
    if (!util::readFile(deskFile(c.id), raw)) return;
    Json saved = raw.size() <= 8 * 1024 * 1024 ? Json::parse(raw) : Json();
    bool valid = saved["queue"].isArray() && saved["contexts"].isArray() && saved["queue"].size() <= 32 &&
                 saved["contexts"].size() <= 20;
    std::set<std::string> ids;
    for (const auto& item : saved["queue"].items()) {
        const auto status = item["status"].str(), id = item["id"].str();
        valid = valid && !id.empty() && ids.insert(id).second && item["text"].isString() &&
                !util::trim(item["text"].str()).empty() && item["text"].str().size() <= 65536 &&
                (status == "pending" || status == "dispatching" || status == "interrupted" || status == "delivered");
    }
    if (!valid) {
        c.desk["problem"] = "佇列資料格式損壞；原檔已保留，請先修復或備份";
        return;
    }
    c.desk = saved;
    for (auto& receipt : c.desk["contexts"].items()) {
        const auto status = receipt["status"].str();
        if (status == "submitted" || status == "running") receipt["status"] = "interrupted";
    }
    // No automatic execution after a crash or restart. An in-flight item needs explicit retry.
    for (auto& item : c.desk["queue"].items())
        if (item["status"].str() == "dispatching") item["status"] = "interrupted";
}

bool App::saveDesk(Conversation& c, const Json& next) {
    if (!c.desk["problem"].str().empty() || !util::writeFileAtomic(deskFile(c.id), next.dump())) return false;
    c.desk = next;
    c.stateDirty = true;
    return true;
}

bool App::handleDesk(const Json& m) {
    const std::string type = m["type"].str();
    if (type == "layout.get" || type == "layout.set") {
        Json current = settings_["layout"];
        if (type == "layout.set") {
            const Json& in = m["layout"];
            auto preset = in["preset"].str();
            bool valid =
                (preset == "reading" || preset == "collaboration" || preset == "review" || preset == "custom") &&
                in["side"].i64() >= 180 && in["side"].i64() <= 380 && in["panel"].i64() >= 260 &&
                in["panel"].i64() <= 520 && in["font"].i64() >= 13 && in["font"].i64() <= 19 &&
                (in["density"].str() == "compact" || in["density"].str() == "comfortable");
            Json previous = settings_;
            if (valid) settings_["layout"] = in;
            bool ok =
                valid && util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump());
            if (!ok) settings_ = previous;
            current = settings_["layout"];
            send(J({{"type", "layout.result"},
                    {"ok", ok},
                    {"requestToken", m["requestToken"]},
                    {"error", ok ? "" : "版面設定未能保存"},
                    {"layout", current}}));
        } else send(J({{"type", "layout.result"}, {"ok", true}, {"layout", current}}));
        return true;
    }
    if (type != "context.get" && !util::startsWith(type, "queue.")) return false;
    Conversation* c = find(m["id"].str());
    if (!c) return true;
    if (c->desk.isNull() || !c->desk["queue"].isArray()) loadDesk(*c);
    if (!c->desk["problem"].str().empty()) {
        send(J({{"type", "queue.result"},
                {"id", c->id},
                {"ok", false},
                {"requestToken", m["requestToken"]},
                {"queue", c->desk["queue"]},
                {"armed", false},
                {"error", c->desk["problem"]}}));
        return true;
    }
    if (type == "context.get") {
        send(J({{"type", "context"}, {"id", c->id}, {"items", c->desk["contexts"]}}));
        return true;
    }
    Json next = c->desk;
    auto& queue = next["queue"].items();
    const std::string id = m["itemId"].str(), text = util::trim(m["text"].str());
    bool valid = true;
    bool arm = c->queueArmed;
    auto item = std::find_if(queue.begin(), queue.end(), [&](const Json& q) { return q["id"].str() == id; });
    if (type == "queue.add") {
        queue.erase(
            std::remove_if(queue.begin(), queue.end(), [](const Json& q) { return q["status"].str() == "delivered"; }),
            queue.end());
        valid = !text.empty() && text.size() <= 65536 && queue.size() < 32;
        if (valid)
            queue.push_back(J({{"id", util::newId()}, {"text", text}, {"ts", util::nowMs()}, {"status", "pending"}}));
        // Adding to a running session opts into dispatch after successful completion.
        if (valid && (anyBusy(*c) || c->group.active) && !c->group.paused) arm = true;
    } else if (type == "queue.resume") {
        arm = true;
    } else if (type == "queue.pause") {
        arm = false;
    } else if (type == "queue.get") {
        // No mutation.
    } else {
        valid = item != queue.end() && (*item)["status"].str() != "dispatching";
        if (valid && type == "queue.edit") {
            valid = !text.empty() && text.size() <= 65536;
            if (valid) (*item)["text"] = text;
        } else if (valid && type == "queue.remove") queue.erase(item);
        else if (valid && type == "queue.retry") (*item)["status"] = "pending";
        else if (valid && type == "queue.move") {
            auto to = m["to"].i64(-1);
            valid = to >= 0 && to < (long long)queue.size();
            if (valid) {
                Json value = *item;
                queue.erase(item);
                queue.insert(queue.begin() + to, value);
            }
        } else valid = false;
    }
    // Pausing is runtime-only and must remain available even when storage is full or locked.
    bool ok = valid && (type == "queue.get" || type == "queue.pause" || saveDesk(*c, next));
    if (ok) {
        c->queueArmed = arm;
        if (type == "queue.pause") c->queueManual = false;
        if (type == "queue.resume") {
            c->queueManual = true;
            c->queueOutcome.clear();
            c->userStopped = false;
        }
        c->stateDirty = true;
    }
    send(J({{"type", "queue.result"},
            {"id", c->id},
            {"requestToken", m["requestToken"]},
            {"ok", ok},
            {"queue", c->desk["queue"]},
            {"armed", c->queueArmed},
            {"error", ok ? "" : "佇列未能保存，原有內容仍保留；請確認項目、文字與資料目錄"}}));
    return true;
}

void App::tickQueue(Conversation& c) {
    if (!c.desk["queue"].isArray() || shuttingDown_) return;
    if (c.queueArmed && (c.group.paused || c.userStopped)) {
        c.queueArmed = false;
        c.stateDirty = true;
    }
    if (anyBusy(c) || c.group.active || !c.preparing.empty()) return;
    if (!c.queueActive.empty()) {
        Json next = c.desk;
        bool success = c.queueOutcome == "completed" && !c.userStopped;
        if (c.kind == "group")
            success = success && (c.workflow.outcome == "completed" || c.workflow.outcome == "consensus");
        for (auto& item : next["queue"].items())
            if (item["id"].str() == c.queueActive) item["status"] = success ? "delivered" : "interrupted";
        if (!success) c.queueArmed = false;
        if (!saveDesk(c, next)) {
            c.queueArmed = false;
            return;
        }
        c.queueActive.clear();
    }
    if (!c.queueManual && (c.userStopped || c.group.paused || c.task["status"].str() == "interrupted" ||
                           c.task["status"].str() == "failed" || c.task["status"].str() == "stopped" ||
                           (!c.queueOutcome.empty() && c.queueOutcome != "completed"))) {
        c.queueArmed = false;
        return;
    }
    if (!c.queueArmed) return;
    auto& items = c.desk["queue"].items();
    auto item = std::find_if(items.begin(), items.end(), [](const Json& q) { return q["status"].str() == "pending"; });
    if (item == items.end()) {
        c.queueArmed = false;
        c.queueManual = false;
        c.stateDirty = true;
        return;
    }
    // Existing plan approval and dispatch checks remain authoritative.
    if (c.collaboration["plan"]["status"].str() == "draft") {
        c.queueArmed = false;
        return;
    }
    Json next = c.desk;
    const size_t index = std::distance(items.begin(), item);
    const auto text = (*item)["text"].str(), id = (*item)["id"].str();
    next["queue"].at(index)["status"] = "dispatching";
    if (!saveDesk(c, next)) {
        c.queueArmed = false;
        return;
    }
    c.queueManual = false;
    c.queueActive = id;
    c.queueOutcome = "failed";  // Synchronous dispatch rejection never becomes a silent success/retry.
    onUiMessage(J({{"type", "conv.send"}, {"id", c.id}, {"text", text}}).dump());
}

void App::captureContext(Conversation& c, const std::string& agent, const AgentConfig& cfg, const std::string& input) {
    c.contextActive.erase(agent);
    if (!c.desk["contexts"].isArray()) loadDesk(c);
    Json next = c.desk;
    Json entry = J({{"id", util::newId()},
                    {"agent", agent},
                    {"ts", util::nowMs()},
                    {"model", cfg.model},
                    {"permission", cfg.permission},
                    {"routing", cfg.routing},
                    {"cwd", util::narrow(cfg.cwd)},
                    {"request", excerpt(util::endsWith(agent, ":opinion") ? input
                                        : c.kind == "group"               ? c.group.task
                                                                          : c.lastRequest)},
                    {"input", excerpt(input)},
                    {"instructions", excerpt(cfg.instructions)},
                    {"snapshot", c.pendingSnapshot.empty() ? c.task["head_revision"].str() : c.pendingSnapshot},
                    {"handoffCount", (long long)c.task["handoffs"].size()},
                    {"history", cfg.resumeId.empty() ? "new-session" : "external-session"},
                    {"observations", Json::array()},
                    {"status", "submitted"}});
    next["contexts"].push(entry);
    while (next["contexts"].size() > 20) next["contexts"].items().erase(next["contexts"].items().begin());
    if (saveDesk(c, next)) c.contextActive[agent] = entry["id"].str();
    else toast("error", "上下文來源紀錄未能保存；本輪仍交由代理處理");
}

void App::contextEvent(Conversation& c, AgentKind kind, const AgentEvent& event) {
    using K = AgentEvent::Kind;
    if (event.kind != K::TurnStarted && event.kind != K::ToolStart && event.kind != K::TurnDone &&
        event.kind != K::Error)
        return;
    auto key = c.group.pipeline ? pipelineNode(c)["id"].str() : std::string(agentName(kind));
    auto active = c.contextActive.find(key);
    if (active == c.contextActive.end()) return;
    auto& entries = c.desk["contexts"].items();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if ((*it)["id"].str() != active->second) continue;
        if (event.kind == K::ToolStart && (*it)["observations"].size() < 80)
            (*it)["observations"].push(J({{"kind", "backend-reported"},
                                          {"ts", util::nowMs()},
                                          {"tool", event.data["tool"]},
                                          {"path", event.data["path"]},
                                          {"title", util::truncateUtf8(event.data["title"].str(), 1000)},
                                          {"version", "unknown"}}));
        else if (event.kind == K::TurnStarted) (*it)["status"] = "running";
        else if (event.kind == K::TurnDone) (*it)["status"] = event.data["status"].str("completed");
        else if (event.kind == K::Error) (*it)["status"] = "failed";
        // Tool events are saved with the next turn/end, avoiding synchronous disk writes per chunk.
        if (event.kind != K::ToolStart && !saveDesk(c, c.desk)) toast("error", "上下文紀錄保存失敗");
        if (event.kind == K::TurnDone || event.kind == K::Error) c.contextActive.erase(key);
        break;
    }
}
