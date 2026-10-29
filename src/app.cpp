#include "app.hpp"
#include "collaboration.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#include "activity.hpp"
#include "badges.hpp"
#include "diff.hpp"
#include "editions.hpp"
#include "humanize.hpp"
#include "model_dates.hpp"
#include "personas.hpp"
#include "pipeline.hpp"
#include "sandbox_mcp.hpp"
#include "takeover.hpp"
#include "updater.hpp"
#include "util.hpp"
#include "version.hpp"

namespace {

const char* kVersion = CCA_VERSION;
const size_t kMaxOutput = 16 * 1024;
const size_t kMaxMessages = 2000;

int slot(AgentKind k) { return (int)k; }

// The guard's test command is typed by the user and run through cmd.exe: one printable line, nothing else.
std::string sanitizeTestCommand(const std::string& raw) {
    std::string text = util::trim(raw);
    if (text.size() > 500) return "";
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7f) return "";
    return text;
}

std::wstring dataFile(const wchar_t* name) { return util::joinPath(util::appDataDir(), name); }

std::wstring messagesFile(const std::string& convId) {
    std::wstring dir = util::joinPath(util::appDataDir(), L"messages");
    util::ensureDir(dir);
    return util::joinPath(dir, util::widen(convId + ".json"));
}

Json* findBlock(Json& msg, const std::string& id) {
    if (id.empty()) return nullptr;
    for (auto& b : msg["blocks"].items()) {
        if (b["id"].str() == id) return &b;
    }
    return nullptr;
}

std::string messageText(const Json& msg) {
    std::string out;
    for (const auto& b : msg["blocks"].items()) {
        if (b["type"].str() != "text") continue;
        if (!out.empty()) out += "\n\n";
        out += b["text"].str();
    }
    return out;
}

bool validPermission(const std::string& p) { return p == "plan" || p == "ask" || p == "edit" || p == "bypass"; }

// Daily chats without a project folder run in this empty folder.
std::wstring dailyDir() {
    std::wstring d = util::joinPath(util::localAppDataDir(), L"daily");
    util::ensureDir(d);
    return d;
}

const char* kDailyNote =
    "# 日常模式\n"
    "現在是日常聊天：使用者想輕鬆聊天，話題不一定和程式有關。回覆自然、簡短，"
    "不要主動讀檔案、搜尋或執行指令，除非使用者明確要求。";

}  // namespace

App::App(SendFn send) : send_(std::move(send)) {
    load();
    personaPresets_ = loadPersonaPresets(dataFile(L"persona-presets.json"), personaPresetProblem_);
    badgeGrants_ = loadBadgeGrants(dataFile(L"badges.json"), badgeGrantProblem_);
    ensureProfileDefaults();
    ensureDefaults();
    editionTransition(editionId());
    loadExtensions();
    cleanTempMedia();
}

App::~App() { shutdown(); }

void App::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    lifetime_.reset();
    if (qualityWorker_) qualityWorker_->terminate();
    for (auto& worker : controls_) worker->shutdown();
    removeMediaFile(pendingImage_["file"].str());
    pendingImage_ = Json::object();
    for (auto& c : convs_) {
        if (c->worker) c->worker->shutdown();
        for (auto& session : c->pipelineGemini) session.second->shutdown();
        stopOpinion(*c);
        for (auto& session : c->pipelineClaude) session.second->shutdown();
        for (auto& session : c->pipelineCodex) session.second->shutdown();
        c->workspaceLease.reset();
        for (auto& m : c->messages) {
            if (m["streaming"].boolean()) {
                m["streaming"] = false;
                m["status"] = "interrupted";
                c->needsSave = true;
            }
        }
        if (c->needsSave) saveMessages(*c);
        if (c->claude) c->claude->shutdown();
        if (c->codex) c->codex->shutdown();
    }
    saveMeta();
    saveUsage();
    if (claudeProbe_) claudeProbe_->shutdown();
    if (codexServer_) codexServer_->stop();
}

// ------------------------------------------------------------ persistence

void App::load() {
    std::string text;
    if (util::readFile(dataFile(L"settings.json"), text)) {
        Json s = Json::parse(text);
        if (s.isObject()) settings_ = s;
    }
    if (util::readFile(dataFile(L"usage.json"), text)) {
        Json u = Json::parse(text);
        if (u.isObject()) usageHistory_ = u;
        usage_["daily"] = usageHistory_;
    }
    if (!util::readFile(dataFile(L"conversations.json"), text)) return;
    Json list = Json::parse(text);
    for (const auto& m : list.items()) {
        auto c = std::make_unique<Conversation>();
        c->id = m["id"].str();
        c->kind = m["kind"].str();
        if (c->id.empty() || (c->kind != "claude" && c->kind != "codex" && c->kind != "gemini" && c->kind != "group"))
            continue;
        c->collaboration = loadCollaboration(c->id);
        c->name = m["name"].str();
        c->pipeline = m["pipeline"].isObject() ? m["pipeline"] : Json::object();
        c->cwd = util::widen(m["cwd"].str());
        if (c->collaboration["workspace"]["ok"].boolean() &&
            util::dirExists(util::widen(c->collaboration["workspace"]["path"].str())))
            c->cwd = util::widen(c->collaboration["workspace"]["path"].str());
        c->permission = validPermission(m["permission"].str()) ? m["permission"].str() : "edit";
        c->claudeModel = m["claudeModel"].str();
        c->claudeEffort = m["claudeEffort"].str();
        c->codexModel = m["codexModel"].str();
        c->codexEffort = m["codexEffort"].str();
        for (const std::string agent : {"claude", "codex"}) {
            c->adaptive[agent]["model"] = m["adaptive"][agent]["model"].boolean(m[agent + "Model"].str().empty());
            c->adaptive[agent]["effort"] = m["adaptive"][agent]["effort"].boolean(m[agent + "Effort"].str().empty());
        }
        c->claudeSession = m["claudeSession"].str();
        c->codexThread = m["codexThread"].str();
        c->geminiSession = m["geminiSession"].str();
        c->geminiMember = c->kind == "group" && m["geminiMember"].boolean();
        c->lead = m["lead"].str() == "codex" ? "codex" : "claude";
        c->autoFallback = m["autoFallback"].boolean(true);
        c->contextPending = c->lead == "codex";
        c->maxRounds = (int)std::clamp<long long>(m["maxRounds"].i64(6), 1, 20);
        c->regressionGuard = c->kind == "group" && m["regressionGuard"].boolean();
        c->testCommand = c->kind == "group" ? sanitizeTestCommand(m["testCommand"].str()) : "";
        c->mode = m["mode"].str().size() <= 160 ? m["mode"].str() : "";
        c->daily = c->kind != "group" && m["daily"].boolean();
        for (const auto& l : m["localMembers"].items()) {
            if (l.str() == "local1" || l.str() == "local2") c->localMembers.push_back(l.str());
        }
        c->createdAt = m["createdAt"].i64();
        loadDesk(*c);
        if (c->kind == "group" && isMediaName(m["avatar"].str())) c->avatar = m["avatar"].str();
        if (c->kind == "group") {
            c->task = loadTask(c->id);
            // CCA closed while the task ran: it is resumable, not silently "running".
            std::string st = c->task["status"].str();
            if (st == "running" || st == "paused") {
                c->task["status"] = "interrupted";
                saveTask(c->task);
            }
        }
        std::string msgs;
        if (util::readFile(messagesFile(c->id), msgs)) {
            Json arr = Json::parse(msgs);
            for (auto& msg : arr.items()) {
                // A turn that was running when CCA closed cannot continue.
                if (msg["streaming"].boolean()) {
                    msg["streaming"] = false;
                    msg["status"] = "interrupted";
                }
                for (auto& b : msg["blocks"].items()) {
                    std::string t = b["type"].str();
                    if ((t == "approval" || t == "question") && b["state"].str() == "pending") b["state"] = "cancelled";
                    if (t == "tool" && b["status"].str() == "running") b["status"] = "interrupted";
                }
                c->messages.push_back(msg);
            }
        }
        convs_.push_back(std::move(c));
    }
}

bool App::saveConversations(const std::string& omit) {
    Json list = Json::array();
    for (const auto& c : convs_) {
        if (c->id == omit) continue;
        list.push(J({{"id", c->id},
                     {"kind", c->kind},
                     {"name", c->name},
                     {"pipeline", c->pipeline},
                     {"cwd", util::narrow(c->cwd)},
                     {"permission", c->permission},
                     {"adaptive", c->adaptive},
                     {"claudeModel", c->claudeModel},
                     {"claudeEffort", c->claudeEffort},
                     {"codexModel", c->codexModel},
                     {"codexEffort", c->codexEffort},
                     {"claudeSession", c->claudeSession},
                     {"codexThread", c->codexThread},
                     {"geminiSession", c->geminiSession},
                     {"geminiMember", c->geminiMember},
                     {"maxRounds", c->maxRounds},
                     {"regressionGuard", c->regressionGuard},
                     {"testCommand", c->testCommand},
                     {"mode", c->mode},
                     {"lead", c->lead},
                     {"autoFallback", c->autoFallback},
                     {"daily", c->daily},
                     {"localMembers",
                      [&] {
                          Json a = Json::array();
                          for (const auto& l : c->localMembers) a.push(l);
                          return a;
                      }()},
                     {"avatar", c->avatar},
                     {"createdAt", c->createdAt}}));
    }
    const bool ok = util::writeFileAtomic(dataFile(L"conversations.json"), list.dump());
    return ok;
}

void App::saveMeta() {
    saveConversations();
    util::writeFileAtomic(dataFile(L"settings.json"), settings_.dump());
    metaDirty_ = false;
}

void App::saveMessages(Conversation& c) {
    if (c.messages.size() > kMaxMessages) {
        c.messages.erase(c.messages.begin(), c.messages.end() - kMaxMessages);
    }
    Json arr = Json::array();
    for (const auto& m : c.messages) arr.push(m);
    util::writeFileAtomic(messagesFile(c.id), arr.dump());
    c.needsSave = false;
}

void App::ensureDefaults() {
    const bool firstRun = convs_.empty();
    auto has = [&](const std::string& kind) {
        for (const auto& c : convs_) {
            if (c->kind == kind) return true;
        }
        return false;
    };
    auto add = [&](const std::string& id, const std::string& kind, const std::string& name) {
        auto c = std::make_unique<Conversation>();
        c->id = id;
        c->kind = kind;
        c->name = name;
        c->createdAt = util::nowMs();
        convs_.push_back(std::move(c));
        metaDirty_ = true;
    };
    if (!has("claude")) add("dm-claude", "claude", "Claude");
    if (!has("codex")) add("dm-codex", "codex", "GPT");
    if (!has("gemini")) add("dm-gemini", "gemini", "Gemini");
    // Deleting the final group must leave an empty rail, rather than silently creating another one.
    if (firstRun && !has("group")) add(util::newId(), "group", "協作群組");
    if (metaDirty_) saveMeta();
}

// ------------------------------------------------------------ UI plumbing

void App::send(const Json& msg) {
    if (msg["type"].str() == "convState" || (msg["type"].str() == "msg" && !msg["msg"]["streaming"].boolean()))
        inboxDirty_ = true;
    if (send_ && uiReady_) send_(msg.dump());
}

void App::toast(const std::string& level, const std::string& text) {
    send(J({{"type", "toast"}, {"level", level}, {"text", text}}));
}

Conversation* App::find(const std::string& id) {
    for (auto& c : convs_) {
        if (c->id == id) return c.get();
    }
    return nullptr;
}

Json* App::findMsg(Conversation& c, const std::string& msgId) {
    if (msgId.empty()) return nullptr;
    for (auto it = c.messages.rbegin(); it != c.messages.rend(); ++it) {
        if ((*it)["id"].str() == msgId) return &*it;
    }
    return nullptr;
}

Json& App::pushMessage(Conversation& c, Json msg) {
    if (!msg.has("id")) msg["id"] = util::newId();
    if (!msg.has("ts")) msg["ts"] = util::nowMs();
    c.messages.push_back(std::move(msg));
    markDirty(c, c.messages.back()["id"].str());
    metaDirty_ = true;  // conversation list shows the latest message
    return c.messages.back();
}

void App::markDirty(Conversation& c, const std::string& msgId) {
    c.dirtyMsgs.insert(msgId);
    c.needsSave = true;
}

Json App::convMeta(const Conversation& c) const {
    Json m = J({{"id", c.id},
                {"kind", c.kind},
                {"name", c.name},
                {"pipeline", c.pipeline},
                {"collaboration", c.collaboration},
                {"cwd", util::narrow(c.cwd)},
                {"isGit", !c.cwd.empty() && (util::dirExists(util::joinPath(c.cwd, L".git")) ||
                                             util::fileExists(util::joinPath(c.cwd, L".git")))},
                {"folderMissing", !c.cwd.empty() && !util::dirExists(c.cwd)},
                {"permission", c.permission},
                {"adaptive", c.adaptive},
                {"claudeModel", c.claudeModel},
                {"claudeEffort", c.claudeEffort},
                {"codexModel", c.codexModel},
                {"codexEffort", c.codexEffort},
                {"maxRounds", c.maxRounds},
                {"regressionGuard", c.regressionGuard},
                {"testCommand", c.testCommand},
                {"mode", c.mode},
                {"lead", c.lead},
                {"autoFallback", c.autoFallback},
                {"claudeLimited", claudeLimited()},
                {"exhausted", J({{"claude", agentExhausted(AgentKind::Claude)},
                                 {"codex", agentExhausted(AgentKind::Codex)},
                                 {"gemini", agentExhausted(AgentKind::Gemini)}})},
                {"geminiMember", c.geminiMember},
                {"daily", c.daily},
                {"localMembers",
                 [&] {
                     Json a = Json::array();
                     for (const auto& l : c.localMembers) a.push(l);
                     return a;
                 }()},
                {"avatar", c.avatar},
                {"createdAt", c.createdAt}});
    for (auto it = c.messages.rbegin(); it != c.messages.rend(); ++it) {
        const Json& msg = *it;
        if (msg["author"].str() == "system") continue;
        std::string text = msg["text"].str();
        if (text.empty()) text = messageText(msg);
        m["lastTs"] = msg["ts"].i64();
        m["lastAuthor"] = msg["author"].str();
        m["preview"] = util::truncateUtf8(util::replaceAll(util::trim(text), "\n", " "), 120);
        break;
    }
    return m;
}

Json App::convState(const Conversation& c) const {
    Json typing = Json::array();
    const auto participant = c.group.pipeline ? pipelineNode(c)["backend"].str("codex") : "codex";
    if (c.worker && c.worker->busy()) typing.push(participant);
    if (c.gemini && c.gemini->busy()) typing.push("gemini");
    if (c.claude && c.claude->busy()) typing.push("claude");
    if (c.codex && c.codex->busy()) typing.push("codex");
    if (c.local[0] && c.local[0]->busy()) typing.push("local1");
    if (c.local[1] && c.local[1]->busy()) typing.push("local2");
    for (const auto& entry : c.collaboration["opinion"]["results"].fields()) {
        const auto status = c.collaboration["opinion"]["status"].str();
        if (status != "running" && status != "summarizing") continue;
        if (entry.second["status"].str() != "running") continue;
        typing.push(entry.first == "summary" ? c.collaboration["opinion"]["coordinator"].str() : entry.first);
    }
    bool running = anyBusy(c) || c.group.active;
    Json state = J({{"routing", c.routes},
                    {"queue", c.desk["queue"]},
                    {"queueArmed", c.queueArmed},
                    {"id", c.id},
                    {"running", running},
                    {"typing", typing},
                    {"thinking", J({{"claude", c.thinking[0]},
                                    {"codex", c.thinking[1]},
                                    {"local1", c.thinking[2]},
                                    {"local2", c.thinking[3]},
                                    {"gemini", c.thinking[4]}})},
                    {"preparing", c.preparing},
                    {"group", J({{"active", c.group.active},
                                 {"phase", c.group.phase},
                                 {"round", c.group.round},
                                 {"maxRounds", c.maxRounds},
                                 {"current", c.group.current},
                                 {"paused", c.group.paused},
                                 {"stopRequested", c.group.stopRequested},
                                 {"pauseReason", c.group.pauseReason},
                                 {"pauseKind", c.group.pauseKind},
                                 {"files", c.group.allFiles},
                                 {"localActive", c.group.localActive}})}});
    if (c.group.pipeline && (participant == "api" || participant == "local")) {
        state["thinking"][participant] = c.thinking[(int)AgentKind::Codex];
        if (state["preparing"].str() == "codex") state["preparing"] = participant;
    }
    if (c.task.isObject()) state["task"] = taskView(c.task);
    state["collaboration"] = c.collaboration;
    Json members = Json::array();
    for (const auto& id : c.localMembers) members.push(id);
    for (const auto& node : c.pipeline["nodes"].items())
        if (node["backend"].str() == "gemini") {
            members.push("gemini");
            break;
        }
    state["workflow"] = workflowView(J({{"kind", c.kind},
                                        {"lead", c.lead},
                                        {"pipeline", c.pipeline},
                                        {"names", J({{"claude", nameOf(AgentKind::Claude)},
                                                     {"codex", nameOf(AgentKind::Codex)},
                                                     {"gemini", nameOf(AgentKind::Gemini)},
                                                     {"local1", nameOf(AgentKind::Local1)},
                                                     {"local2", nameOf(AgentKind::Local2)}})},
                                        {"members", members},
                                        {"models", J({{"claude", c.claudeModel},
                                                      {"codex", c.codexModel},
                                                      {"local1", localModel(AgentKind::Local1)},
                                                      {"local2", localModel(AgentKind::Local2)}})}}),
                                     state, c.workflow, c.messages);
    return state;
}

void App::sendConvs() {
    Json items = Json::array();
    for (const auto& c : convs_) items.push(convMeta(*c));
    send(J({{"type", "convs"}, {"items", items}, {"groupOrder", groupOrder()}}));
}

void App::sendBoot() {
    usage_["activity"] = tokenActivity(usageHistory_, activityToday());
    Json items = Json::array();
    Json states = Json::array();
    for (const auto& c : convs_) {
        items.push(convMeta(*c));
        states.push(convState(*c));
    }
    send(J({{"type", "boot"},
            {"version", kVersion},
            {"edition", J({{"id", editionId()}, {"name", editionName()}})},
            {"user", J({{"name", util::windowsUserName()}})},
            {"personas", allPersonas()},
            {"badges", badgeState()},
            {"convs", items},
            {"groupOrder", groupOrder()},
            {"states", states},
            {"settings", settings_},
            {"modelDates", modelDateTable()},
            {"modelDateAliases", modelDateAliases()},
            {"models", J({{"claude", claudeModels_}, {"codex", codexModels_}})},
            {"usage", usage_}}));
    sendAuth();
}

void App::sendUsage() {
    usage_["activity"] = tokenActivity(usageHistory_, activityToday());
    send(J({{"type", "usage"}, {"usage", usage_}}));
}

void App::sendModels() {
    send(J({{"type", "models"}, {"models", J({{"claude", claudeModels_}, {"codex", codexModels_}})}}));
}

void App::tick() {
    long long now = util::nowMs();
    if (uiReady_ && geminiStatus_.state == "ok" && now - lastGeminiUsage_ >= 60000) checkGemini();
    pollClaudeUsage(false);
    tickUpdate(now);
    tickGit(now);
    tickAutoResume(now);
    for (auto& cp : convs_) {
        Conversation& c = *cp;
        tickQueue(c);
        if (!c.dirtyMsgs.empty()) {
            // IDs have a random suffix. Iterating the dirty set can put a summary
            // before the reply when both were created in the same millisecond.
            for (const auto& m : c.messages) {
                if (c.dirtyMsgs.count(m["id"].str())) send(J({{"type", "msg"}, {"convId", c.id}, {"msg", m}}));
            }
            c.dirtyMsgs.clear();
        }
        if (c.stateDirty) {
            send(J({{"type", "convState"}, {"state", convState(c)}}));
            c.stateDirty = false;
        }
    }
    if (inboxDirty_ && now - lastInbox_ > 400) sendInbox();
    if (now - lastSave_ > 1500) {
        lastSave_ = now;
        for (auto& c : convs_) {
            if (c->needsSave) saveMessages(*c);
        }
        if (metaDirty_) {
            saveMeta();
            sendConvs();
        }
        saveUsage();
    }
}

// ------------------------------------------------------------ compliance checks

GuardRules App::guardRulesFor(const Conversation& c, AgentKind k) const {
    GuardRules r;
    r.speaker = nameOf(k);
    Json persona = findPersona(settings_["personas"], settings_["persona"][agentName(k)].str(), personaPresets_);
    if (persona.isObject() && !util::trim(persona["prompt"].str()).empty()) {
        r.personaName = persona["name"].str();
        r.address = util::trim(persona["address"].str());
    }
    r.checkTestClaims = !(c.daily && c.kind != "group");
    const GroupRun& g = c.group;
    if (!isLocal(k) && c.kind == "group" && g.active && g.current == agentName(k)) {
        bool reviewing = g.phase == "review" || g.phase == "recheck" || g.phase == "selfcheck" || g.phase == "verify";
        r.requireVerdict = r.requireEvidence = reviewing;
        r.changedFiles = (long long)g.turnFiles.size();
        r.localNames = g.notesFrom;
    }
    return r;
}

bool App::guardReply(Conversation& c, AgentKind k, const std::string& msgId) {
    if (!guardOn() || isLocal(k)) return false;
    Json* m = findMsg(c, msgId);
    if (!m) return false;
    int i = slot(k), tries = c.guardTry[i], max = std::max(0, guardRetries());
    GuardRules rules = guardRulesFor(c, k);
    std::vector<GuardIssue> issues = guardCheck(rules, messageText(*m), (*m)["blocks"]);
    if (issues.empty()) {
        if (tries > 0) (*m)["guard"] = J({{"status", "passed"}, {"attempt", tries}});
        c.guardTry[i] = 0;
        markDirty(c, msgId);
        return false;
    }
    Json list = guardIssuesJson(issues);
    (*m)["guard"] = J({{"status", "rejected"}, {"attempt", tries + 1}, {"issues", list}});
    markDirty(c, msgId);
    // m is not used past this point: pushMessage may move the messages.
    std::string summary = guardSummary(issues);
    bool inGroup = c.kind == "group" && c.group.active && c.group.current == agentName(k);
    if (tries < max) {
        c.guardTry[i] = tries + 1;
        std::string prompt = guardCorrection(rules, issues, tries + 1, max);
        pushMessage(c, J({{"author", "system"},
                          {"kind", "guard"},
                          {"agent", agentName(k)},
                          {"issues", list},
                          {"attempt", tries + 1},
                          {"max", max},
                          {"text", "規範審查未通過（" + summary + "），已退回給 " + nameOf(k) + " 重寫（第 " +
                                       std::to_string(tries + 1) + "/" + std::to_string(max) + " 次）"}}));
        if (inGroup) c.group.lastPrompt = prompt;
        c.guardResend[i] = true;
        std::string err;
        if (!sendToAgent(c, k, prompt, &err)) sendFailed(c, k, err);
        return true;
    }
    c.guardTry[i] = 0;
    std::string reason = nameOf(k) + " 退回 " + std::to_string(max) + " 次仍未通過規範審查（" + summary + "）。";
    if (inGroup) {
        c.group.guardMsg = msgId;
        groupPause(c, reason + "可以再退回一次，或接受這則回覆繼續。", k, guardCorrection(rules, issues, max, max),
                   c.group.phase, c.group.round, "guard");
        return true;
    }
    pushMessage(c, J({{"author", "system"},
                      {"kind", "guard"},
                      {"agent", agentName(k)},
                      {"issues", list},
                      {"final", true},
                      {"text", reason + "這則回覆保留在上面，請自行判斷是否採用。"}}));
    return false;
}

// ------------------------------------------------------------ agents

AgentConfig App::configFor(const Conversation& c, AgentKind k, bool routed) const {
    AgentConfig cfg;
    cfg.cwd = c.cwd;
    cfg.permission = c.permission;
    cfg.model = k == AgentKind::Claude ? c.claudeModel : c.codexModel;
    cfg.effort = k == AgentKind::Claude ? c.claudeEffort : c.codexEffort;
    cfg.resumeId = k == AgentKind::Claude ? c.claudeSession : c.codexThread;
    if (k == AgentKind::Gemini) {
        // No catalogue is reported by the CLI, so CCA never guesses a model; the session continues the chat.
        cfg.model.clear();
        cfg.effort.clear();
        cfg.resumeId = c.geminiSession;
    }
    std::string instr = personaPrompt(k);
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!instr.empty()) instr += "\n\n";
        instr += part;
    };
    if (c.kind == "group") add(groupInstructions(c, k));
    add(extensionModeInstructions(c));
    if (c.kind == "group" && !c.localMembers.empty()) {
        add("群組裡還有本地模型擔任唯讀審查員，它們的建議會附在交接訊息後面。那些建議來自較小的模型，可能出錯："
            "不要百分之百相信，採納前一定要自己查證（開檔案、搜尋、跑測試），並說明證據。");
    }
    if (c.daily && c.kind != "group") {
        // Cheapest model, nothing edited without asking, and no project folder needed.
        cfg.permission = "ask";
        cfg.model = k == AgentKind::Gemini ? "" : dailyModel(k);
        cfg.effort = k == AgentKind::Gemini ? "" : dailyEffort(k);
        if (cfg.cwd.empty() || !util::dirExists(cfg.cwd)) cfg.cwd = dailyDir();
        add(kDailyNote);
    }
    // Both agents get the same Docker sandbox as a tool, so a failing agent-side sandbox does not stop the work.
    if (settings_["sandboxShare"].boolean(true) && !c.daily && k != AgentKind::Gemini) {
        cfg.sandbox = findSandboxMcp();
        if (cfg.sandbox.on()) add(sandboxInstructions());
    }
    if (humanizeOn()) add(humanizeInstructions(settings_["humanize"]["extra"].str()));
    if (guardOn()) add(guardInstructions());
    cfg.instructions = instr;
    const auto& route = c.routes[agentName(k)];
    if (routed && !c.daily && route.isObject()) {
        cfg.model = route["model"].str();
        cfg.effort = route["effort"].str();
        cfg.routing = route;
    }
    return cfg;
}

ClaudeSession& App::claudeFor(Conversation& c) {
    if (!c.claude) {
        std::string id = c.id;
        std::wstring exe = claudeStatus_.exe.empty() ? resolveClaudeExe() : claudeStatus_.exe;
        c.claude = std::make_shared<ClaudeSession>(
            exe, [this, id](const AgentEvent& ev) { onAgentEvent(id, AgentKind::Claude, ev); });
    }
    c.claude->configure(configFor(c, AgentKind::Claude));
    return *c.claude;
}

GeminiSession& App::geminiFor(Conversation& c) {
    if (!c.gemini) {
        std::string id = c.id;
        c.gemini = std::make_shared<GeminiSession>(
            [this, id](const AgentEvent& ev) { onAgentEvent(id, AgentKind::Gemini, ev); });
    }
    return *c.gemini;
}

void App::ensureCodexServer() {
    if (codexServer_) return;
    std::wstring exe = codexStatus_.exe.empty() ? resolveCodexExe() : codexStatus_.exe;
    codexServer_ = std::make_shared<CodexServer>(exe);
    codexServer_->onGlobal = [this](const std::string& m, const Json& p) { onCodexGlobal(m, p); };
    codexServer_->onExit = [this](const std::string& reason) {
        for (auto& c : convs_) {
            if (c->codex) c->codex->serverLost();
            if (c->opinionCodex) c->opinionCodex->serverLost();
        }
        if (!shuttingDown_) toast("error", reason);
    };
}

CodexThread* App::codexFor(Conversation& c, std::string* error) {
    ensureCodexServer();
    if (!codexServer_->running() && !codexServer_->start(error)) return nullptr;
    if (!c.codex) {
        std::string id = c.id;
        c.codex = std::make_shared<CodexThread>(
            codexServer_, [this, id](const AgentEvent& ev) { onAgentEvent(id, AgentKind::Codex, ev); });
    }
    c.codex->configure(configFor(c, AgentKind::Codex));
    return c.codex.get();
}

bool App::sendToAgent(Conversation& c, AgentKind k, const std::string& text, std::string* error) {
    std::string transport = pipelineNode(c)["backend"].str();
    // A new prompt starts a new count of send-backs; a send-back keeps counting.
    if (!c.guardResend[slot(k)]) c.guardTry[slot(k)] = 0;
    c.guardResend[slot(k)] = false;
    if (!c.group.pipeline && k == AgentKind::Claude && c.autoFallback && claudeLimited())
        return takeOver(c, "Claude 額度已耗盡", error);
    bool daily = c.daily && c.kind != "group";
    if (!daily && (c.cwd.empty() || !util::dirExists(c.cwd))) {
        if (error) *error = "請先選擇專案資料夾";
        return false;
    }
    // Re-checked here too, for folders saved by an older version.
    std::string problem = c.cwd.empty() ? "" : util::projectFolderProblem(c.cwd);
    if (!problem.empty()) {
        if (error) *error = problem;
        return false;
    }
    if (k == AgentKind::Claude && claudeStatus_.state != "ok") {
        if (error) *error = "Claude 尚未通過訂閱檢查";
        return false;
    }
    if (k == AgentKind::Codex && codexStatus_.state != "ok" && (transport.empty() || transport == "codex")) {
        if (error) *error = "GPT 尚未通過訂閱檢查";
        return false;
    }
    if (k == AgentKind::Gemini && transport.empty() && geminiStatus_.state != "ok") {
        if (error) *error = geminiStatus_.message.empty() ? "Gemini 尚未通過帳號檢查" : geminiStatus_.message;
        return false;
    }
    if (daily || c.cwd.empty() || !snapshots_.available()) {
        startAgent(c, k, text);
        return true;
    }
    // Checkpoint the project before the agent can touch it; git runs off the UI thread.
    c.preparing = agentName(k);
    c.stateDirty = true;
    std::string convId = c.id;
    std::wstring folder = c.cwd;
    int n = 0;
    for (const auto& m : c.messages) n += m["author"].str() == agentName(k);
    std::string label = std::string(agentName(k)) + " turn " + std::to_string(n + 1);
    Snapshots snaps = snapshots_;
    std::string expected = c.group.pipeline ? c.task["head_revision"].str() : "";
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, convId, folder, label, k, text, snaps, expected]() mutable {
        SnapshotResult r = snaps.take(convId, folder, label);
        Json delta = r.ok && !expected.empty() ? snaps.compare(convId, folder, expected, r.commit) : Json();
        postToUi([this, life, convId, k, text, r, expected, delta] {
            if (life.expired()) return;
            Conversation* c = find(convId);
            if (!c) return;
            c->preparing.clear();
            c->stateDirty = true;
            if (c->userStopped || c->group.stopRequested) {
                c->pendingSnapshot.clear();
                return;
            }
            if (c->group.pipeline && c->task["pause_requested"].boolean()) {
                c->task["pause_requested"] = false;
                if (r.ok) c->task["head_revision"] = r.commit;
                pipelineBlock(*c, "使用者已暫停，沒有啟動下一位代理");
                return;
            }
            if (c->group.pipeline && !expected.empty() && (!delta.isObject() || delta["changed"].boolean())) {
                c->task["conflict"] = true;
                c->task["conflict_head"] = r.commit;
                c->task["conflict_reference"] = delta["reference"];
                pipelineBlock(*c, "工作區與上一筆交接版本不同；已停止寫入。檢查差異後，按繼續才採用目前版本");
                return;
            }
            if (r.ok) c->pendingSnapshot = r.commit;
            else {
                if (c->group.pipeline) {
                    pipelineBlock(*c, "建立版本檢查點失敗：" + r.error);
                    return;
                }
                pushMessage(*c,
                            J({{"author", "system"}, {"kind", "info"}, {"text", "這一輪沒有建立檢查點：" + r.error}}));
            }
            if (c->group.pipeline) {
                if (c->task["base_revision"].str().empty()) c->task["base_revision"] = r.commit;
                c->task["active_base_revision"] = r.commit;
                if (!saveTask(c->task)) {
                    pipelineBlock(*c, "執行前的版本紀錄保存失敗，沒有啟動代理");
                    return;
                }
            }
            startAgent(*c, k, text);
        });
    }).detach();
    return true;
}

void App::startAgent(Conversation& c, AgentKind k, const std::string& text) {
    std::string err;
    if (c.group.pipeline) {
        if (!startPipelineAgent(c, text, &err)) pipelineBlock(c, err);
        return;
    }
    AgentConfig selected = configFor(c, k, false);
    if (!routeConfig(c, k, selected, c.kind == "group" ? c.group.task : c.lastRequest)) {
        sendFailed(c, k, selected.routing["reason"].str());
        return;
    }
    bool ok;
    std::string input = c.contextPending ? handoverPrompt(c, text) : text;
    // The rules ride along with every prompt, not only in the system prompt, so long sessions do not drift.
    if (guardOn()) input = guardRuleCard(guardRulesFor(c, k)) + "\n" + input;
    captureContext(c, agentName(k), configFor(c, k), input);
    if (k == AgentKind::Claude) {
        ok = claudeFor(c).send(input, &err);
    } else if (k == AgentKind::Gemini) {
        ok = geminiFor(c).send(resolveGeminiExe(), configFor(c, k), input, &err);
    } else {
        CodexThread* t = codexFor(c, &err);
        ok = t && t->send(input, &err);
    }
    if (ok) c.contextPending = false;
    if (!ok) sendFailed(c, k, err);
}

void App::sendFailed(Conversation& c, AgentKind k, const std::string& error) {
    contextEvent(c, k, {AgentEvent::Kind::Error, "", J({{"text", error}})});
    c.pendingSnapshot.clear();
    if (c.kind == "group" && c.group.active && c.group.current == agentName(k)) {
        groupPause(c, "無法把工作交給 " + nameOf(k) + "：" + error, k, c.group.lastPrompt, c.group.phase, c.group.round);
    } else {
        pushMessage(c, J({{"author", "system"}, {"kind", "error"}, {"text", error}}));
        finishPlan(c, "failed");
    }
    c.stateDirty = true;
}

bool App::anyBusy(const Conversation& c) const {
    if (c.gemini && c.gemini->busy()) return true;
    if (qualityBusy_.count(c.id)) return true;
    const auto opinion = c.collaboration["opinion"]["status"].str();
    if (opinion == "running" || opinion == "summarizing") return true;
    if (c.opinionClaude && c.opinionClaude->busy()) return true;
    if (c.opinionCodex && c.opinionCodex->busy()) return true;
    for (const auto& worker : c.opinionWorkers)
        if (worker.second->busy()) return true;
    if (c.worker && c.worker->busy()) return true;
    if (!c.preparing.empty()) return true;
    if (c.claude && c.claude->busy()) return true;
    if (c.codex && c.codex->busy()) return true;
    for (const auto& l : c.local) {
        if (l && l->busy()) return true;
    }
    return false;
}

void App::recordUsage(const std::string& agent, const Json& u) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    Json& a = usageHistory_[day][agent];
    a["input"] = a["input"].i64() + u["inputTokens"].i64();
    a["output"] = a["output"].i64() + u["outputTokens"].i64();
    a["cached"] = a["cached"].i64() + u["cachedTokens"].i64();
    a["cost"] = a["cost"].num() + u["costUsd"].num();
    a["turns"] = a["turns"].i64() + 1;
    // Daily totals are small; preserve history for yearly activity and cumulative totals.
    usage_["daily"] = usageHistory_;
    usageDirty_ = true;
}

bool App::saveUsage() {
    if (!usageDirty_) return true;
    if (!util::writeFileAtomic(dataFile(L"usage.json"), usageHistory_.dump())) return false;
    usageDirty_ = false;
    return true;
}

void App::onCodexGlobal(const std::string& method, const Json& p) {
    if (method == "account/rateLimits/updated") {
        usage_["codex"]["limits"] = p["rateLimits"];
        sendUsage();
    } else if (method == "account/updated") {
        if (!p["planType"].str().empty()) codexStatus_.plan = p["planType"].str();
        sendAuth();
    } else if (method == "account/login/completed") {
        if (p["success"].boolean()) {
            runAuthCheck(false, true);
        } else {
            codexStatus_.state = "loggedOut";
            codexStatus_.message = "登入沒有完成：" + p["error"].str("已取消");
            sendAuth();
        }
    }
}

void App::onAgentEvent(const std::string& convId, AgentKind k, const AgentEvent& ev) {
    using K = AgentEvent::Kind;
    Conversation* cp = find(convId);
    if (!cp) return;
    Conversation& c = *cp;
    int i = slot(k);
    contextEvent(c, k, ev);
    const Json& d = ev.data;

    switch (ev.kind) {
    case K::SessionInfo: {
        if (c.group.pipeline) {
            std::string session = d["sessionId"].str(d["threadId"].str());
            if (!session.empty()) {
                c.task["sessions"][pipelineNode(c)["id"].str()] = session;
                saveTask(c.task);
            }
        }
        if (k == AgentKind::Claude && d.has("sessionId") && !d["sessionId"].str().empty() &&
            d["sessionId"].str() != c.claudeSession) {
            c.claudeSession = d["sessionId"].str();
            metaDirty_ = true;
        }
        if (k == AgentKind::Gemini && !c.group.pipeline && !d["sessionId"].str().empty() &&
            d["sessionId"].str() != c.geminiSession) {
            c.geminiSession = d["sessionId"].str();
            metaDirty_ = true;
        }
        if (d.has("threadId") && !d["threadId"].str().empty() && d["threadId"].str() != c.codexThread) {
            c.codexThread = d["threadId"].str();
            metaDirty_ = true;
        }
        if (d.has("permissionPreset")) {
            c.permission = d["permissionPreset"].str();
            metaDirty_ = true;
            sendConvs();
        }
        if (d["models"].size() > 0 && k == AgentKind::Claude) {
            claudeModels_ = d["models"];
            sendModels();
        }
        if (!d["model"].str().empty()) {
            if (Json* m = findMsg(c, c.activeMsg[i])) {
                (*m)["resolvedModel"] = d["model"].str();
                markDirty(c, c.activeMsg[i]);
            }
        }
        return;
    }
    case K::RateLimit: {
        if (k == AgentKind::Claude) {
            std::string type = d["rateLimitType"].str("five_hour");
            usage_["claude"]["limits"][type] = d;
            recordClaudeLimit(d);
            if (isClaudeQuotaRejected(d) && c.autoFallback && !c.userStopped && !c.group.stopRequested &&
                c.claude && c.claude->busy()) c.claude->interrupt();
        } else {
            usage_["codex"]["limits"] = d;
        }
        sendUsage();
        return;
    }
    case K::Exited:
        return;
    case K::TurnStarted: {
        AgentConfig cfg = isLocal(k) ? AgentConfig() : configFor(c, k);
        if (isLocal(k)) {
            cfg.model = localModel(k);
            cfg.permission = "readonly";
        }
        Json msg = J({{"routing", cfg.routing},
                      {"author", agentName(k)},
                      {"blocks", Json::array()},
                      {"streaming", true},
                      {"model", cfg.model},
                      {"effort", cfg.effort},
                      {"permission", cfg.permission}});
        if (c.group.pipeline) {
            Json node = pipelineNode(c);
            msg["task_id"] = c.task["task_id"];
            msg["agent_id"] = node["id"];
            msg["agent_label"] = node["name"].str(node["id"].str());
            const auto backend = node["backend"].str();
            if (backend == "gemini") msg["author"] = "gemini";
            const bool adaptive = backend == "claude" || backend == "codex";
            msg["routing"] = adaptive ? cfg.routing : Json::object();
            msg["model"] = adaptive ? Json(cfg.model) : node["model"];
            if (backend == "gemini") msg["effort"] = "";
            msg["billing"] = node["backend"].str() == "api"     ? "api"
                             : node["backend"].str() == "local" ? "local"
                                                                : "subscription";
            if (backend == "gemini") msg["billing"] = "google-account";
        }
        if (c.daily && c.kind != "group") msg["daily"] = true;
        if (!isLocal(k) && !c.pendingSnapshot.empty()) {
            msg["snapshot"] = c.pendingSnapshot;
            // The checkpoint before this turn is the state the previous agent handed over.
            if (c.kind == "group" && c.task.isObject()) {
                setHeadRevision(c.task, c.pendingSnapshot);
                saveTask(c.task);
            }
            c.pendingSnapshot.clear();
        }
        if (c.kind == "group" && c.group.active) {
            msg["phase"] = isLocal(k) ? "advise" : c.group.phase;
            msg["round"] = c.group.round;
        }
        c.activeMsg[i] = pushMessage(c, msg)["id"].str();
        workflowTurn(c.workflow, c.group.pipeline ? "pipeline:" + pipelineNode(c)["id"].str() : msg["author"].str(),
                     c.activeMsg[i],
                     isLocal(k)              ? "advise"
                     : c.group.phase.empty() ? "write"
                                             : c.group.phase,
                     util::nowMs());
        c.thinking[i] = false;
        c.stateDirty = true;
        return;
    }
    default:
        break;
    }

    Json* m = findMsg(c, c.activeMsg[i]);
    if (!m) {
        // Something arrived outside a turn (for example a warning at startup).
        if (ev.kind != K::Notice && ev.kind != K::Error && ev.kind != K::Approval && ev.kind != K::Question) return;
        Json msg = J({{"author", agentName(k)}, {"blocks", Json::array()}, {"streaming", false}});
        m = &pushMessage(c, msg);
    }
    Json& msg = *m;
    std::string msgId = msg["id"].str();
    Json& blocks = msg["blocks"];
    if (!blocks.isArray()) blocks = Json::array();
    // Text/output chunks do not change the diagram. Publish state on transitions,
    // rather than serializing the workflow again for every streamed token.
    if (ev.kind == K::ToolStart || ev.kind == K::ToolEnd || ev.kind == K::Approval ||
        ev.kind == K::Question || ev.kind == K::Resolved || ev.kind == K::Retry) c.stateDirty = true;

    switch (ev.kind) {
    case K::TextDelta: {
        Json* b = findBlock(msg, ev.id);
        if (!b) {
            blocks.push(J({{"type", "text"}, {"id", ev.id}, {"text", ""}}));
            b = &blocks.items().back();
        }
        (*b)["text"] = (*b)["text"].str() + d["delta"].str();
        if (msg.has("retry")) c.stateDirty = true;
        msg.erase("retry");
        if (c.thinking[i]) { c.thinking[i] = false; c.stateDirty = true; }
        break;
    }
    case K::Text: {
        Json* b = findBlock(msg, ev.id);
        if (!b) {
            if (d["text"].str().empty()) break;
            blocks.push(J({{"type", "text"}, {"id", ev.id}, {"text", ""}}));
            b = &blocks.items().back();
        }
        (*b)["text"] = d["text"].str();
        (*b)["done"] = true;
        break;
    }
    case K::Thinking:
        if (!c.thinking[i]) {
            c.thinking[i] = true;
            c.stateDirty = true;
        }
        return;
    case K::ToolStart: {
        Json b = d;
        b["type"] = "tool";
        b["id"] = ev.id;
        b["status"] = "running";
        b["startedAt"] = util::nowMs();
        blocks.push(b);
        if (c.thinking[i]) { c.thinking[i] = false; c.stateDirty = true; }
        break;
    }
    case K::ToolOutput: {
        if (Json* b = findBlock(msg, ev.id)) {
            (*b)["output"] = util::tailUtf8((*b)["output"].str() + d["delta"].str(), kMaxOutput);
        }
        break;
    }
    case K::ToolEnd: {
        if (Json* b = findBlock(msg, ev.id)) {
            for (const auto& f : d.fields()) {
                if (f.first == "output" && f.second.str().empty() && !(*b)["output"].str().empty()) continue;
                (*b)[f.first] = f.second;
            }
            if (!(*b).has("durationMs") || (*b)["durationMs"].i64() == 0) {
                (*b)["durationMs"] = util::nowMs() - (*b)["startedAt"].i64();
            }
        }
        break;
    }
    case K::FileChange: {
        invalidateEvidence(c, "模型回報來源變更，需重新核對驗收證據");
        Json b = d;
        b["type"] = "file";
        b["id"] = ev.id;
        if (Json* old = findBlock(msg, ev.id)) *old = b;
        else blocks.push(b);
        if (c.kind == "group" && c.group.active) {
            Json rec = J({{"path", d["path"]}, {"change", d["change"]}, {"added", d["added"]},
                          {"removed", d["removed"]}, {"diff", d["diff"]}});
            c.group.turnFiles.push(rec);
            Json& tot = c.group.allFiles[d["path"].str()];
            tot["added"] = tot["added"].i64() + d["added"].i64();
            tot["removed"] = tot["removed"].i64() + d["removed"].i64();
            if (!tot.has("change")) tot["change"] = d["change"];
            c.stateDirty = true;
        }
        break;
    }
    case K::Approval:
    case K::Question: {
        Json b = d;
        b["type"] = ev.kind == K::Approval ? "approval" : "question";
        b["id"] = ev.id;
        b["state"] = "pending";
        b["agent"] = agentName(k);
        blocks.push(b);
        if (windowAction) windowAction("flash");
        break;
    }
    case K::Resolved: {
        if (Json* b = findBlock(msg, ev.id)) {
            (*b)["state"] = d["state"];
            if (d.has("feedback") && !d["feedback"].str().empty()) (*b)["feedback"] = d["feedback"];
            if (d.has("answers")) (*b)["answers"] = d["answers"];
        } else {
            // The card may live in an earlier message of the same turn.
            for (auto it = c.messages.rbegin(); it != c.messages.rend(); ++it) {
                if (Json* b = findBlock(*it, ev.id)) {
                    (*b)["state"] = d["state"];
                    markDirty(c, (*it)["id"].str());
                    break;
                }
            }
        }
        break;
    }
    case K::Todo: {
        Json* b = nullptr;
        for (auto& x : blocks.items()) {
            if (x["type"].str() == "todo") b = &x;
        }
        if (!b) {
            blocks.push(J({{"type", "todo"}, {"id", util::newId()}}));
            b = &blocks.items().back();
        }
        (*b)["items"] = d["items"];
        break;
    }
    case K::Usage: {
        msg["usage"] = d;
        Json node = pipelineNode(c);
        if (c.group.pipeline && (node["backend"].str() == "api" || node["backend"].str() == "local")) {
            Json& usage = c.task["costs"];
            usage["inputTokens"] = usage["inputTokens"].i64() + d["inputTokens"].i64();
            usage["outputTokens"] = usage["outputTokens"].i64() + d["outputTokens"].i64();
            usage["costUsd"] = usage["costUsd"].num() + d["costUsd"].num();
            usage["costKnown"] = d["costKnown"].boolean() && (!usage.has("costKnown") || usage["costKnown"].boolean());
            usage["billing"] = d["billing"];
            Json& individual = c.task["agent_costs"][node["id"].str()];
            individual["tokens"] = individual["tokens"].i64() + d["inputTokens"].i64() + d["outputTokens"].i64();
            individual["costUsd"] = individual["costUsd"].num() + d["costUsd"].num();
            recordUsage(node["backend"].str(), d);
            sendUsage();
            c.stateDirty = true;
            break;
        }
        const std::string usageAgent = node["backend"].str() == "gemini" ? "gemini" : agentName(k);
        Json& sess = usage_[usageAgent]["session"];
        sess["costUsd"] = sess["costUsd"].num() + d["costUsd"].num();
        sess["inputTokens"] = sess["inputTokens"].i64() + d["inputTokens"].i64();
        sess["outputTokens"] = sess["outputTokens"].i64() + d["outputTokens"].i64();
        sess["turns"] = sess["turns"].i64() + 1;
        usage_[usageAgent]["context"] =
            J({{"used", d["contextUsed"]}, {"window", d["contextWindow"]}, {"convId", c.id}});
        recordUsage(usageAgent, d);
        sendUsage();
        break;
    }
    case K::Retry:
        msg["retry"] = d;
        break;
    case K::Notice:
        blocks.push(J({{"type", "notice"}, {"id", util::newId()}, {"text", d["text"]}, {"level", d["level"].str("info")}}));
        break;
    case K::Artifact:
        importArtifact(c, d);
        break;
    case K::Error:
        blocks.push(J({{"type", "error"}, {"id", util::newId()}, {"text", d["text"]}, {"code", d["code"].str()}}));
        break;
    case K::TurnDone: {
        std::string status = d["status"].str("completed");
        c.queueOutcome = status;
        // Agent-produced screenshots are imported only through the project boundary and WIC validation.
        std::vector<std::string> pictures;
        for (const auto& b : msg["blocks"].items())
            if (b["type"].str() == "text") {
                for (const auto& line : util::splitLines(b["text"].str())) {
                    if (util::startsWith(line, "CCA_ARTIFACT: ")) pictures.push_back(util::trim(line.substr(14)));
                }
            }
        // Importing adds a message and can reallocate c.messages; defer until this turn's reference is released.
        if (!pictures.empty()) {
            std::weak_ptr<int> life = lifetime_;
            postToUi([this, life, convId, pictures] {
                if (life.expired()) return;
                if (Conversation* current = find(convId))
                    for (const auto& path : pictures)
                        importArtifact(*current,
                                       J({{"path", path}, {"source", "agent-report"}, {"operation", "瀏覽器測試"}}));
            });
        }
        if (k == AgentKind::Claude && d["quotaExceeded"].boolean() && !claudeLimited())
            recordClaudeLimit(J({{"status", "rejected"}}));
        msg["streaming"] = false;
        msg["status"] = status;
        msg["endedAt"] = util::nowMs();
        msg.erase("retry");
        if (k == AgentKind::Claude) pollClaudeUsage(true);
        // The inserts and erase above reallocated or shifted msg's members, so `blocks` from the top of this function
        // may dangle; look the array up again.
        Json& finalBlocks = msg["blocks"];
        if (status == "failed" && !d["error"].str().empty()) {
            finalBlocks.push(J({{"type", "error"}, {"id", util::newId()}, {"text", d["error"]}}));
        }
        // A reply with no text (for example only tool calls) still gets the CLI's final summary.
        if (messageText(msg).empty() && !d["text"].str().empty() && status == "completed") {
            finalBlocks.push(J({{"type", "text"}, {"id", util::newId()}, {"text", d["text"]}, {"done", true}}));
        }
        for (auto& b : finalBlocks.items()) {
            if (b["type"].str() == "tool" && b["status"].str() == "running") b["status"] = "interrupted";
            if ((b["type"].str() == "approval" || b["type"].str() == "question") && b["state"].str() == "pending") {
                b["state"] = "cancelled";
            }
        }
        // What a later rollback needs to rewind each agent's memory.
        if (!d["uuid"].str().empty()) msg["claudeUuid"] = d["uuid"];
        if (!d["turnId"].str().empty()) msg["codexTurn"] = d["turnId"];
        c.activeMsg[i].clear();
        if (c.group.pipeline) {
            c.thinking[i] = false;
            c.stateDirty = true;
            markDirty(c, msgId);
            Json copy = msg;
            pipelineDone(c, copy, status);
            return;
        }
        c.thinking[i] = false;
        c.stateDirty = true;
        markDirty(c, msgId);
        if (isLocal(k)) {
            // The answer the review settled on, and the files its tools actually read (local_tools.hpp).
            if (d.has("text")) msg["finalText"] = d["text"];
            if (d.has("readPaths")) msg["readPaths"] = d["readPaths"];
            if (d["corrections"].i64() > 0) msg["corrections"] = d["corrections"];
            if (c.kind == "group" && c.group.active && c.group.localActive == agentName(k)) {
                Json copy = msg;
                groupLocalDone(c, k, copy, status);
            }
            if (c.kind != "group") finishPlan(c, status);
            return;
        }
        // Provider refusals are remembered so later turns skip this member until the window resets.
        const bool quotaHit = status == "failed" && quotaFailure(k, msg);
        if (quotaHit) recordQuotaHit(k, 0);
        if (k == AgentKind::Claude && !c.userStopped && !c.group.stopRequested &&
            (c.takeoverRequested || (c.autoFallback && (d["quotaExceeded"].boolean() || quotaHit)))) {
            const bool quota = d["quotaExceeded"].boolean() || quotaHit;
            if (quota && !claudeLimited()) recordClaudeLimit(J({{"status", "rejected"}}));
            std::string err;
            if (!takeOver(c, quota ? "Claude 額度已耗盡" : "使用者指定 GPT 接手", &err))
                sendFailed(c, AgentKind::Codex, err);
            return;
        }
        if (quotaHit && c.kind != "group") {
            sendConvs();
            pushMessage(c,
                        J({{"author", "system"},
                           {"kind", "skip"},
                           {"agent", agentName(k)},
                           {"text", nameOf(k) + " 額度不足，這則需求沒有完成；需求已保留，額度恢復後可以重新送出。"}}));
            finishPlan(c, status);
            return;
        }
        // Compliance checks: a reply that fails goes back to the same agent before anyone else sees it.
        if (status == "completed" && !c.userStopped && !c.group.stopRequested && guardReply(c, k, msgId)) return;
        if (c.kind == "group" && c.group.active && c.group.current == agentName(k)) {
            Json* live = findMsg(c, msgId);
            Json copy = live ? *live : Json::object();
            groupTurnDone(c, k, copy, status);
        }
        if (c.kind != "group") finishPlan(c, status);
        return;
    }
    default:
        break;
    }
    markDirty(c, msgId);
}

// ------------------------------------------------------------ conversation actions

void App::applySettings(Conversation& c, const Json& s) {
    if (s.has("lead")) {
        std::string lead = s["lead"].str();
        if (lead != "claude" && lead != "codex") { toast("error", "未知的主刀手"); return; }
        if (anyBusy(c) || c.group.active) { toast("error", "工作進行中請使用「GPT 接手」或先停止"); return; }
        c.contextPending = c.contextPending || c.lead != lead;
        c.lead = lead;
        if (lead == "claude") settings_["claudeQuota"]["blocked"] = false; // Explicit retry.
    }
    if (s.has("autoFallback")) c.autoFallback = s["autoFallback"].boolean();

    if (s.has("name")) {
        std::string n = util::trim(s["name"].str());
        if (!n.empty()) c.name = util::truncateUtf8(n, 80);
    }
    if (s.has("permission")) {
        std::string p = s["permission"].str();
        if (!validPermission(p)) {
            toast("error", "未知的權限模式");
        } else if (p == "bypass" && !s["confirmBypass"].boolean()) {
            toast("error", "切換到「跳過所有確認」需要先在對話框中確認風險");
        } else {
            c.permission = p;
        }
    }
    if (s.has("cwd")) {
        std::wstring cwd = util::widen(s["cwd"].str());
        bool busy = anyBusy(c) || c.group.active;
        if (busy) {
            toast("error", "AI 正在工作，停止後才能更換資料夾");
        } else if (!cwd.empty() && !util::dirExists(cwd)) {
            toast("error", "找不到這個資料夾");
        } else if (!cwd.empty() && !util::projectFolderProblem(cwd).empty()) {
            toast("error", util::projectFolderProblem(cwd));
        } else if (c.collaboration["workspace"]["ok"].boolean() && cwd != c.cwd) {
            toast("error", "此對話已綁定隔離 worktree；其他專案請使用另一個對話");
        } else {
            c.cwd = cwd;
        }
    }
    if (s.has("claudeModel")) {
        c.claudeModel = s["claudeModel"].str();
        c.adaptive["claude"]["model"] = false;
        c.routes.erase("claude");
    }
    if (s.has("claudeEffort")) {
        c.claudeEffort = s["claudeEffort"].str();
        c.adaptive["claude"]["effort"] = false;
        c.routes.erase("claude");
    }
    if (s.has("codexModel")) {
        c.codexModel = s["codexModel"].str();
        c.adaptive["codex"]["model"] = false;
        c.routes.erase("codex");
    }
    if (s.has("codexEffort")) {
        c.codexEffort = s["codexEffort"].str();
        c.adaptive["codex"]["effort"] = false;
        c.routes.erase("codex");
    }
    if (s.has("maxRounds")) c.maxRounds = (int)std::clamp<long long>(s["maxRounds"].i64(6), 1, 20);
    if (c.kind == "group" && s.has("testCommand")) {
        const std::string command = sanitizeTestCommand(s["testCommand"].str());
        if (command.empty() && !s["testCommand"].str().empty()) toast("error", "測試指令只能是單行、500 字以內的文字");
        else c.testCommand = command;
    }
    if (c.kind == "group" && s.has("regressionGuard")) {
        c.regressionGuard = s["regressionGuard"].boolean() && !c.testCommand.empty();
        if (s["regressionGuard"].boolean() && c.testCommand.empty()) toast("error", "請先設定測試指令，再開啟測試守門");
    }
    if (c.testCommand.empty()) c.regressionGuard = false;
    if (s.has("daily") && c.kind != "group") c.daily = s["daily"].boolean();
    if (s.has("geminiMember") && c.kind == "group") c.geminiMember = s["geminiMember"].boolean();
    if (s.has("localMembers") && c.kind == "group") {
        c.localMembers.clear();
        for (const auto& l : s["localMembers"].items()) {
            std::string v = l.str();
            if ((v == "local1" || v == "local2") && std::find(c.localMembers.begin(), c.localMembers.end(), v) == c.localMembers.end())
                c.localMembers.push_back(v);
        }
    }
    if (c.claude) c.claude->configure(configFor(c, AgentKind::Claude));
    if (c.codex) c.codex->configure(configFor(c, AgentKind::Codex));
    metaDirty_ = true;
    c.stateDirty = true;
    sendConvs();
}

void App::resetConversation(Conversation& c) {
    bool busy = anyBusy(c) || c.group.active;
    if (busy) {
        toast("error", "AI 正在工作，請先停止");
        return;
    }
    if (!saveDesk(c, J({{"queue", Json::array()}, {"contexts", Json::array()}}))) {
        toast("error", "待送資料未能清除，對話已保留");
        return;
    }
    c.queueArmed = c.queueManual = false;
    c.queueActive.clear();
    c.queueOutcome.clear();
    c.contextActive.clear();
    if (c.claude) { c.claude->shutdown(); c.claude.reset(); }
    if (c.codex) { c.codex->shutdown(); c.codex.reset(); }
    c.claudeSession.clear();
    c.codexThread.clear();
    c.geminiSession.clear();
    clearPipelineSessions(c);
    c.messages.clear();
    c.workflow = WorkflowRun();
    c.lastRequest.clear();
    c.contextPending = c.takeoverRequested = c.userStopped = false;
    c.group = GroupRun();
    c.task = Json();
    deleteTask(c.id);
    c.dirtyMsgs.clear();
    DeleteFileW(messagesFile(c.id).c_str());
    metaDirty_ = true;
    send(J({{"type", "history"}, {"id", c.id}, {"messages", Json::array()}}));
    send(J({{"type", "convState"}, {"state", convState(c)}}));
    sendConvs();
}

void App::handleApproval(const Json& m) {
    Conversation* c = find(m["convId"].str());
    if (!c) return;
    if (c->worker && c->worker->busy()) {
        c->worker->respond(m["blockId"].str(), m["choice"].str());
        return;
    }
    std::string blockId = m["blockId"].str();
    std::string ch = m["choice"].str();
    std::string agent;
    for (auto it = c->messages.rbegin(); it != c->messages.rend() && agent.empty(); ++it) {
        if (Json* b = findBlock(*it, blockId)) {
            if ((*b)["state"].str() != "pending") return;
            agent = (*b)["agent"].str();
        }
    }
    if (agent == "claude" && c->claude) c->claude->respondApproval(blockId, ch, m["feedback"].str());
    else if (agent == "codex" && c->codex) c->codex->respondApproval(blockId, ch, m["feedback"].str());
}

void App::handleQuestion(const Json& m) {
    Conversation* c = find(m["convId"].str());
    if (!c) return;
    std::string blockId = m["blockId"].str();
    std::string agent;
    for (auto it = c->messages.rbegin(); it != c->messages.rend() && agent.empty(); ++it) {
        if (Json* b = findBlock(*it, blockId)) agent = (*b)["agent"].str();
    }
    if (agent == "claude" && c->claude) c->claude->respondQuestion(blockId, m["answers"]);
    else if (agent == "codex" && c->codex) c->codex->respondQuestion(blockId, m["answers"]);
}

void App::onUiMessage(const std::string& text) {
    const Json m = Json::parse(text);
    std::string type = m["type"].str();
    if (handleUpdate(m)) return;
    if (updateInstalling_ && type != "ready" && type != "win") {
        toast("info", "更新準備中，完成後重新開啟 CCA");
        return;
    }
    if (handleQuality(m)) return;
    if (handleCollaboration(m)) return;
    if (handlePlatform(m)) return;
    if (type == "groups.reorder") {
        handleGroupOrder(m);
        return;
    }

    if (handleTheme(m) || handleRouting(m) || handleDesk(m) || handleGit(m) || handleExtensions(m)) return;
    if (type == "ready") {
        updater::confirmStartup();
        uiReady_ = true;
        sendBoot();
        sendExtensions();
        checkGemini();
        if (!personaPresetProblem_.empty()) toast("error", personaPresetProblem_);
        if (!badgeGrantProblem_.empty()) toast("error", badgeGrantProblem_);
        if (!editionNotice_.empty()) toast("info", editionNotice_);
        editionNotice_.clear();
        if (claudeStatus_.state == "checking" || codexStatus_.state == "checking") runAuthCheck(true, true);
        return;
    }
    if (type == "auth.check") {
        if (m["agent"].str().empty() || m["agent"].str() == "gemini") checkGemini();
        if (m["agent"].str() == "gemini") return;
        runAuthCheck(m["agent"].str() != "codex", m["agent"].str() != "claude");
        return;
    }
    if (type == "auth.login") {
        if (m["agent"].str() == "gemini") {
            loginGemini();
            return;
        }
        if (m["agent"].str() == "claude") loginClaude();
        else loginCodex();
        return;
    }
    if (type == "win") {
        if (windowAction) windowAction(m["action"].str());
        return;
    }
    if (type == "file.reveal") {
        const auto target = util::replaceAll(m["target"].str(), "/", "\\");
        const bool drive = target.size() > 3 &&
                           ((target[0] >= 'A' && target[0] <= 'Z') || (target[0] >= 'a' && target[0] <= 'z')) &&
                           target[1] == ':' && target[2] == '\\';
        const bool safe = drive && target.size() < 32700 && target.find(':', 2) == std::string::npos &&
                          std::none_of(target.begin(), target.end(), [](unsigned char ch) { return ch < 32; });
        if (!safe || !util::fileExists(util::widen(target))) {
            toast("error", "找不到本機檔案，或路徑格式不支援");
            return;
        }
        const auto end = target.find_last_of('\\');
        const auto folder = util::widen(target.substr(0, end == 2 ? 3 : end));
        // Model-provided file links reveal a folder; they never execute an executable or script.
        if (util::dirExists(folder) && openExternal) openExternal(folder);
        return;
    }
    if (type == "open") {
        std::string target = m["target"].str();
        bool url = util::startsWith(target, "https://") || util::startsWith(target, "http://");
        bool folder = !url && util::dirExists(util::widen(target));
        if ((url || folder) && openExternal) openExternal(util::widen(target));
        return;
    }
    if (type == "folder.pick") {
        std::wstring picked = pickFolder ? pickFolder(util::widen(m["initial"].str())) : L"";
        send(J({{"type", "folder.picked"}, {"reqId", m["reqId"]}, {"path", util::narrow(picked)}}));
        return;
    }
    if (type == "settings.set") {
        // Only the two CLI paths come through here, and CCA later starts whatever they name,
        // so each must be empty (auto-detect) or an existing program file. Every other setting
        // has its own validated message (profile.set, persona.*, daily.set, ...).
        const Json& in = m["settings"];
        bool claude = false, codex = false, gemini = false;
        for (const char* key : {"claudePath", "codexPath", "geminiPath"}) {
            if (!in.has(key)) continue;
            if (std::string(key) == "geminiPath") {
                bool busy = geminiChecking_ || (geminiLogin_ && geminiLogin_->running());
                for (const auto& c : convs_) busy |= c->gemini && c->gemini->busy();
                if (busy) {
                    toast("error", "請等待 Gemini 查詢或工作結束，再變更執行檔");
                    continue;
                }
            }
            std::string path = util::trim(in[key].str());
            std::string ext = util::toLower(path.size() > 4 ? path.substr(path.size() - 4) : "");
            if (!path.empty() &&
                (!util::fileExists(util::widen(path)) || (ext != ".exe" && ext != ".cmd" && ext != ".bat"))) {
                toast("error", std::string("找不到這個執行檔，或它不是 .exe、.cmd 或 .bat：") + path);
                continue;
            }
            settings_[key] = path;
            if (std::string(key) == "geminiPath") gemini = true;
            else (std::string(key) == "claudePath" ? claude : codex) = true;
        }
        if (!claude && !codex && !gemini) return;
        metaDirty_ = true;
        if (codex && codexServer_) {
            codexServer_->stop();
            codexServer_.reset();
            for (auto& c : convs_) {
                if (c->codex) {
                    c->codex->shutdown();
                    c->codex.reset();
                }
            }
        }
        runAuthCheck(claude, codex);
        if (gemini) checkGemini();
        sendSettings();
        return;
    }
    if (type == "usage.refresh") {
        checkGemini();
        sendUsage();
        refreshCodexAccount();
        pollClaudeUsage(true);
        return;
    }
    if (type == "claudeUsage.set") {
        settings_["claudeUsageLive"] = m["enabled"].boolean();
        metaDirty_ = true;
        sendSettings();
        pollClaudeUsage(true);
        return;
    }
    if (type == "profile.set") { handleProfileSet(m); return; }
    if (type == "badges.display") {
        handleBadgeDisplay(m);
        return;
    }
    if (type == "rollback") { handleRollback(m); return; }
    if (type == "rollback.undo") { handleRollbackUndo(m); return; }
    if (type == "review.open") {
        handleReviewOpen(m);
        return;
    }
    if (type == "review.revert") {
        handleReviewRevert(m);
        return;
    }
    if (type == "local.status") { sendLocalStatus(); return; }
    if (type == "local.pull") { handleLocalPull(m); return; }
    if (type == "local.cancel") { if (pulls_.count(m["model"].str())) *pulls_[m["model"].str()] = true; return; }
    if (type == "local.delete") { handleLocalDelete(m); return; }
    if (type == "local.set") { handleLocalSet(m); return; }
    if (type == "profile.image") { handleProfileImage(m); return; }
    if (type == "image.commit" || type == "image.cancel") {
        handleImageAction(m);
        return;
    }
    if (type == "persona.save") { handlePersonaSave(m); return; }
    if (type == "persona.delete") { handlePersonaDelete(m); return; }
    if (type == "persona.assign") { handlePersonaAssign(m); return; }
    if (type == "daily.set") { handleDailySet(m); return; }
    if (type == "humanize.set") { handleHumanizeSet(m); return; }
    if (type == "visual.set") {
        std::string mode = m["mode"].str();
        if (mode == "auto" || mode == "compact" || mode == "off") settings_["visual"]["mode"] = mode;
        if (m["motion"].isBool()) settings_["visual"]["motion"] = m["motion"];
        metaDirty_ = true;
        sendSettings();
        return;
    }
    if (type == "plugins.list") { listPlugins(m["agent"].str(), m["refresh"].boolean()); return; }
    if (type == "plugins.install") { installPlugin(m); return; }
    if (type == "plugins.uninstall") { uninstallPlugin(m); return; }
    if (type == "mcp.list") { listMcp(); return; }
    if (type == "mcp.add") { addMcp(m); return; }
    if (type == "mcp.remove") { removeMcp(m); return; }
    if (type == "autoResume.set") {
        // Only known fields with bounded values are stored; the page sends the whole section each time.
        const Json& in = m["settings"];
        Json previous = settings_;
        Json& out = settings_["autoResume"];
        out["enabled"] = in["enabled"].boolean();
        out["delayMinutes"] = std::clamp<long long>(in["delayMinutes"].i64(3), 0, 1440);
        out["maxAttempts"] = std::clamp<long long>(in["maxAttempts"].i64(3), 1, 20);
        for (const char* agent : {"claude", "codex", "gemini"})
            out["fallback"][agent] = std::clamp<long long>(in["fallback"][agent].i64(60), 1, 10080);
        const auto clock = [](const std::string& t) {
            return t.size() == 5 && t[2] == ':' && isdigit((unsigned char)t[0]) && isdigit((unsigned char)t[1]) &&
                   isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]) && std::stoi(t.substr(0, 2)) < 24 &&
                   std::stoi(t.substr(3)) < 60;
        };
        out["window"] = J({{"enabled", in["window"]["enabled"].boolean()},
                           {"from", clock(in["window"]["from"].str()) ? in["window"]["from"].str() : "00:00"},
                           {"to", clock(in["window"]["to"].str()) ? in["window"]["to"].str() : "08:00"}});
        if (!util::writeFileAtomic(dataFile(L"settings.json"), settings_.dump())) {
            settings_ = previous;
            toast("error", "自動接續設定未能保存，原設定已保留");
        }
        sendSettings();
        return;
    }
    if (type == "conv.create") {
        auto c = std::make_unique<Conversation>();
        c->id = util::newId();
        c->kind = "group";
        std::string name = util::trim(m["name"].str());
        c->name = name.empty() ? "協作群組" : util::truncateUtf8(name, 80);
        std::wstring cwd = util::widen(m["cwd"].str());
        if (!cwd.empty() && util::dirExists(cwd)) {
            std::string problem = util::projectFolderProblem(cwd);
            if (problem.empty()) c->cwd = cwd;
            else toast("error", problem);
        }
        c->createdAt = util::nowMs();
        std::string id = c->id;
        convs_.push_back(std::move(c));
        metaDirty_ = true;
        saveMeta();
        sendConvs();
        send(J({{"type", "conv.created"}, {"id", id}}));
        return;
    }

    Conversation* c = find(m["id"].str().empty() ? m["convId"].str() : m["id"].str());
    if (type == "approval") { handleApproval(m); return; }
    if (type == "question") { handleQuestion(m); return; }
    if (!c) return;

    bool opinionBusy = (c->opinionClaude && c->opinionClaude->busy()) || (c->opinionCodex && c->opinionCodex->busy());
    for (const auto& worker : c->opinionWorkers) opinionBusy |= worker.second->busy();
    if (type == "conv.stop" && (opinionBusy || c->collaboration["opinion"]["status"].str() == "running" ||
                                c->collaboration["opinion"]["status"].str() == "summarizing")) {
        stopOpinion(*c);
        return;
    }
    if (type == "conv.takeover") { handleTakeover(*c); return; }
    if (type == "conv.image") {
        beginImage("", "avatar", c->id);
        return;
    }
    if (type == "conv.avatar.remove" && c->kind == "group") {
        if (!persistGroupAvatar(*c, "")) toast("error", "無法儲存群組頭像設定，已保留原圖");
        sendConvs();
        return;
    }
    if (type == "conv.open") {
        Json arr = Json::array();
        for (const auto& msg : c->messages) arr.push(msg);
        send(J({{"type", "history"}, {"id", c->id}, {"messages", arr}}));
        send(J({{"type", "convState"}, {"state", convState(*c)}}));
        settings_["lastConv"] = c->id;
        if (c->kind != "group") settings_["lastDm"] = c->id;
        metaDirty_ = true;
    } else if (type == "conv.settings") {
        applySettings(*c, m["settings"]);
    } else if (type == "conv.delete") {
        if (c->kind != "group") {
            toast("error", "私訊不能刪除，可以改用「清除對話」");
            return;
        }
        if (c->group.active || anyBusy(*c)) {
            toast("error", "群組正在工作，請先停止");
            return;
        }
        // Commit the removal before deleting messages or the avatar it referenced.
        if (!saveConversations(c->id)) {
            toast("error", "無法儲存群組變更，群組與頭像已保留");
            return;
        }
        std::string id = c->id;
        if (c->claude) c->claude->shutdown();
        if (c->codex) c->codex->shutdown();
        if (c->gemini) c->gemini->shutdown();
        DeleteFileW(messagesFile(id).c_str());
        deleteTask(id);
        removeMediaFile(c->avatar);
        convs_.erase(std::remove_if(convs_.begin(), convs_.end(), [&](const auto& x) { return x->id == id; }),
                     convs_.end());
        ensureDefaults();
        saveMeta();
        sendConvs();
    } else if (type == "conv.reset") {
        resetConversation(*c);
    } else if (type == "conv.send") {
        std::string text = util::trim(m["text"].str());
        if (text.empty()) return;
        // A CCA plugin command ("/name rest") becomes its template; what the agents receive is the expanded text.
        expandExtensionCommand(*c, text);
        for (const auto& other : convs_) {
            if (other.get() == c || other->preparing != "worktree") continue;
            auto normalize = [](const std::wstring& path) {
                return util::toLower(util::replaceAll(util::narrow(path), "\\", "/"));
            };
            const auto path = normalize(c->cwd);
            if (path == normalize(other->cwd) ||
                path == normalize(util::widen(other->collaboration["workspace"]["origin"].str()))) {
                toast("error", "此工作區正在建立或合併隔離任務，請稍後再傳送");
                return;
            }
        }
        if (c->kind == "group") {
            if (c->group.pipeline && c->group.active) {
                if (!c->group.paused || anyBusy(*c)) {
                    toast("info", "請先暫停，再傳送補充需求");
                    return;
                }
                Json update = J({{"task_id", c->task["task_id"]},
                                 {"agent_id", "user"},
                                 {"role", "instruction"},
                                 {"base_revision", c->task["head_revision"]},
                                 {"head_revision", c->task["head_revision"]},
                                 {"changed_files", Json::array()},
                                 {"diff_reference", "user-update"},
                                 {"verdict", "NEEDS_FIX"},
                                 {"summary", util::truncateUtf8(text, 1500)},
                                 {"report", text},
                                 {"issues", Json::array()},
                                 {"test_results", Json::array()},
                                 {"next_action", "先套用使用者補充，再重新審查"},
                                 {"ts", util::nowMs()}});
                addHandoff(c->task, update);
                c->task["latest_update"] =
                    J({{"seq", c->task["handoffs"].items().back()["seq"]}, {"text", util::truncateUtf8(text, 4000)}});
                c->task["cursor"] = 0;
                c->task["cycle"] = 1;
                c->task["passes"] = Json::object();
                c->task["tests_passed"] = false;
                c->task["guard_attempts"] = Json::object();
                if (!saveTask(c->task)) {
                    toast("error", "補充需求無法保存，仍維持暫停，請排除寫入問題後重試");
                    return;
                }
                pushMessage(*c, J({{"author", "user"}, {"text", text}, {"task_id", c->task["task_id"]}}));
                pipelineNext(*c);
                return;
            }
            groupStart(*c, text);
            return;
        }
        AgentKind k = c->kind == "gemini"                         ? AgentKind::Gemini
                      : c->kind == "claude" && c->lead != "codex" ? AgentKind::Claude
                                                                  : AgentKind::Codex;
        if (anyBusy(*c)) {
            toast("error", "上一則訊息還在處理中");
            return;
        }
        c->lastRequest = text;
        c->workflow = WorkflowRun();
        c->userStopped = false;
        pushMessage(*c, J({{"author", "user"}, {"text", text}}));
        std::string err;
        if (!sendToAgent(*c, k, text, &err)) {
            pushMessage(*c, J({{"author", "system"}, {"kind", "error"}, {"text", err}}));
        }
        c->stateDirty = true;
    } else if (type == "conv.stop") {
        c->userStopped = true;
        c->takeoverRequested = false;
        if (c->kind == "group" && c->group.active) {
            groupStop(*c);
        } else {
            if (c->claude) c->claude->interrupt();
            if (c->codex) c->codex->interrupt();
            for (auto& l : c->local) if (l) l->interrupt();
            // The headless Gemini CLI has no interrupt request; ending its process ends the turn.
            if (c->gemini && c->gemini->busy()) {
                c->gemini->shutdown();
                onAgentEvent(c->id, AgentKind::Gemini,
                             {AgentEvent::Kind::TurnDone, "", J({{"status", "interrupted"}})});
            }
        }
    } else if (type == "group.continue") {
        groupContinue(*c);
    } else if (type == "task.resume") {
        taskResume(*c);
    } else if (type == "task.autoResume") {
        // Cancelling keeps the stop record; the task can still be resumed by hand.
        if (c->task["quota_stop"].isObject()) {
            c->task["quota_stop"]["resumeAt"] = m["enabled"].boolean() ? autoResumeAt(*c) : 0;
            saveTask(c->task);
            c->stateDirty = true;
        }
    } else if (type == "group.accept") {
        groupAccept(*c);
    } else if (type == "group.end") {
        if (c->group.active && c->group.paused) groupFinish(*c, "stopped", "使用者結束了協作");
    }
}
