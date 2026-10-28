// Per-provider automatic controls preserve saved manual choices as explicit fallbacks.
#include "app.hpp"

#include "editions.hpp"
#include "routing.hpp"
#include "util.hpp"

namespace {
std::string previousTask(const Conversation& c, const std::string& current) {
    if (current.size() > 160 || !analyzeTask(current)["followup"].boolean()) return "";
    int inspected = 0;
    for (auto it = c.messages.rbegin(); it != c.messages.rend() && inspected < 12; ++it) {
        if ((*it)["author"].str() != "user") continue;
        ++inspected;
        const auto text = util::truncateUtf8((*it)["text"].str(), 12000);
        if (text.empty() || text == current || analyzeTask(text)["followup"].boolean()) continue;
        return text;
    }
    return "";
}
}  // namespace

bool App::routeConfig(Conversation& c, AgentKind kind, AgentConfig& config, const std::string& request, bool fixedModel,
                      bool fixedEffort) {
    if (isLocal(kind) || kind == AgentKind::Gemini || c.daily) return true;
    const auto agent = agentName(kind);
    bool deepest = false;
    Json choice = chooseRoute(request, kind == AgentKind::Claude ? claudeModels_ : codexModels_, config.model,
                              config.effort, c.adaptive[agent]["model"].boolean() && !fixedModel,
                              c.adaptive[agent]["effort"].boolean() && !fixedEffort, previousTask(c, request), deepest);
    config.model = choice["model"].str();
    config.effort = choice["effort"].str();
    config.routing = choice;
    c.routes[agent] = choice;
    c.stateDirty = true;
    return !choice["blocked"].boolean();
}

bool App::handleRouting(const Json& message) {
    const auto type = message["type"].str();
    if (type != "routing.set" && type != "routing.manual" && type != "routing.preview") return false;
    auto c = find(message["id"].str());
    const auto agent = message["agent"].str();
    bool ok = c && (agent == "claude" || agent == "codex");
    if (type == "routing.preview") {
        if (!ok) return true;
        const auto request = message["text"].str();
        auto kind = agent == "claude" ? AgentKind::Claude : AgentKind::Codex;
        const auto config = configFor(*c, kind, false);
        bool deepest = false;
        auto result =
            chooseRoute(util::truncateUtf8(request, 65536), agent == "claude" ? claudeModels_ : codexModels_,
                        config.model, config.effort, c->adaptive[agent]["model"].boolean() && !c->daily,
                        c->adaptive[agent]["effort"].boolean() && !c->daily, previousTask(*c, request), deepest);
        if (c->daily) result["reason"] = "日常模式沿用固定的省額度設定";
        send(J({{"type", "routing.preview"},
                {"id", c->id},
                {"agent", agent},
                {"result", result},
                {"requestToken", message["requestToken"]}}));
        return true;
    }
    if (ok && (anyBusy(*c) || c->group.active)) ok = false;
    if (!ok) {
        send(J({{"type", "routing.result"},
                {"ok", false},
                {"id", message["id"]},
                {"requestToken", message["requestToken"]},
                {"error", "先等候任務結束或停止，再調整自動選擇"}}));
        return true;
    }
    Json previous = c->adaptive;
    const auto oldModel = agent == "claude" ? c->claudeModel : c->codexModel;
    const auto oldEffort = agent == "claude" ? c->claudeEffort : c->codexEffort;
    std::string model = oldModel, effort = oldEffort;
    if (type == "routing.set") {
        for (const char* field : {"model", "effort"}) {
            if (!message.has(field)) continue;
            if (!message[field].isBool()) ok = false;
            else c->adaptive[agent][field] = message[field];
        }
    } else {
        const auto& settings = message["settings"];
        const auto mk = agent + "Model", ek = agent + "Effort";
        auto safe = [](const Json& value) {
            return value.isString() && value.str().size() <= 128 &&
                   value.str().find_first_of("\r\n\t") == std::string::npos;
        };
        if (settings.has(mk)) {
            ok = safe(settings[mk]);
            model = settings[mk].str();
            c->adaptive[agent]["model"] = false;
        }
        if (settings.has(ek)) {
            ok = ok && safe(settings[ek]);
            effort = settings[ek].str();
            c->adaptive[agent]["effort"] = false;
        }
        if (!settings.has(mk) && !settings.has(ek)) ok = false;
        (agent == "claude" ? c->claudeModel : c->codexModel) = model;
        (agent == "claude" ? c->claudeEffort : c->codexEffort) = effort;
    }
    ok = ok && saveConversations();
    if (!ok) {
        c->adaptive = previous;
        (agent == "claude" ? c->claudeModel : c->codexModel) = oldModel;
        (agent == "claude" ? c->claudeEffort : c->codexEffort) = oldEffort;
    } else {
        c->routes.erase(agent);
        c->stateDirty = true;
        sendConvs();
    }
    send(J({{"type", "routing.result"},
            {"id", c->id},
            {"ok", ok},
            {"requestToken", message["requestToken"]},
            {"adaptive", c->adaptive},
            {"error", ok ? "" : "自動選擇設定未能保存，原選擇已保留"}}));
    return true;
}
