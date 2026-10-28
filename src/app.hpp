// The hub between the web UI and the two agent backends. It owns conversations,
// turns agent events into Discord-style messages, persists them, and runs the
// Claude-writes / GPT-reviews loop for group conversations. UI thread only.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "agent.hpp"
#include "claude_agent.hpp"
#include "codex_agent.hpp"
#include "gemini_agent.hpp"
#include "guard.hpp"
#include "json.hpp"
#include "ollama.hpp"
#include "runtime.hpp"
#include "snapshot.hpp"
#include "task_ledger.hpp"
#include "workflow.hpp"

struct GroupRun {
    bool pipeline = false;
    bool active = false;
    bool stopRequested = false;
    bool solo = false;  // Claude is unavailable; GPT completes and checks the work.
    std::string task;
    int round = 0;         // reviewer turns so far
    std::string phase;     // write | review | recheck
    std::string current;   // claude | codex
    std::string turnText;  // final reply of the turn that just ran
    Json turnFiles = Json::array();
    Json allFiles = Json::object();                  // path -> {added, removed, change}
    int limit = 6;                                   // reviewer turns allowed before asking the user
    std::map<std::string, std::string> lastVerdict;  // agent -> write | changed | issues | approve
    std::string lastPrompt;                          // what the current agent was sent (for retry)
    bool paused = false;
    std::string pauseReason;
    std::string pendingAgent;  // who to send to when the user presses continue/retry
    std::string pendingPrompt;
    std::string pendingPhase;
    int pendingRound = 0;
    // Read-only review by the invited local models, between two Claude/GPT turns.
    std::vector<std::string> localQueue;  // local slots still to review this handoff
    std::string localActive;              // slot reviewing right now
    std::string localNotes;               // their suggestions, passed on unverified
    std::string nextAgent, nextPrompt, nextPhase;
    std::string reviewedBy;              // whose changes the locals are reviewing
    std::vector<std::string> notesNext;  // local reviewers whose notes go with the next handoff
    std::vector<std::string> notesFrom;  // local reviewers whose notes came with the current prompt
    std::string pauseKind;               // "guard" when paused over a reply that failed the checks
    std::string guardMsg;                // that reply, for "accept and continue"
    // Members are rotated in order (lead, partner, Gemini); quota-exhausted ones are skipped for this run.
    std::set<std::string> skipped;
    std::set<std::string> approvals;  // members who accepted the code since its latest change
    std::string lastChanger;          // author of the latest change; implicitly agrees with it
    std::string turnSnapshot;         // checkpoint taken before the turn that is being tested (empty: none)
};

struct Conversation {
    std::string id;
    std::string kind;  // claude | codex | gemini | group
    std::string name;
    std::wstring cwd;
    std::string permission = "edit";  // plan | ask | edit | bypass
    std::string claudeModel, claudeEffort, codexModel, codexEffort;
    std::string claudeSession, codexThread, geminiSession;
    bool geminiMember = false;  // groups: Gemini joins the review rotation after Claude and GPT
    Json adaptive =
        J({{"claude", J({{"model", true}, {"effort", true}})}, {"codex", J({{"model", true}, {"effort", true}})}});
    Json routes = Json::object();
    int maxRounds = 6;
    std::string lead = "claude";
    bool autoFallback = true;
    bool daily = false;                     // 日常模式: casual chat on the cheapest model (DMs only)
    bool regressionGuard = false;           // groups: undo a reviewer's edits that turn the tests red
    std::string testCommand;                // groups: the command the guard runs in the project folder
    std::string mode;                       // a CCA plugin mode applied here ("plugin/mode"), or empty
    std::vector<std::string> localMembers;  // groups: invited local reviewers (local1/local2)
    std::string avatar;                     // groups: own avatar, a file in App::mediaDir()
    long long createdAt = 0;
    std::vector<Json> messages;

    // Runtime only.
    std::shared_ptr<ClaudeSession> claude;
    std::shared_ptr<CodexThread> codex;
    std::shared_ptr<LocalSession> local[2];
    std::string activeMsg[5];  // Claude, Codex, two local slots, Gemini.
    bool thinking[5] = {};
    int guardTry[5] = {};      // Send-backs for the reply being checked.
    bool guardResend[5] = {};  // Preserve the counter on a send-back.
    std::string preparing;     // agent whose turn waits for the checkpoint to finish
    std::string pendingSnapshot;
    GroupRun group;
    WorkflowRun workflow;
    Json task;                       // shared task memory of the latest group task (task_ledger.hpp), persisted
    Json pipeline = Json::object();  // Optional ordered agents; old groups keep their original loop.
    Json collaboration = Json::object();
    std::shared_ptr<ClaudeSession> opinionClaude;
    std::shared_ptr<CodexThread> opinionCodex;
    std::map<std::string, std::shared_ptr<RuntimeSession>> opinionWorkers;
    std::map<std::string, std::shared_ptr<ClaudeSession>> pipelineClaude;
    std::map<std::string, std::shared_ptr<CodexThread>> pipelineCodex;
    std::shared_ptr<RuntimeSession> worker;
    std::shared_ptr<GeminiSession> gemini;
    std::map<std::string, std::shared_ptr<GeminiSession>> pipelineGemini;
    std::shared_ptr<void> workspaceLease;
    std::set<std::string> dirtyMsgs;
    bool stateDirty = false;
    bool needsSave = false;
    bool takeoverRequested = false;
    bool userStopped = false;
    bool contextPending = false;
    Json desk = Json::object();  // Persistent queue and auditable CCA inputs.
    bool queueArmed = false;
    bool queueManual = false;
    std::string queueActive;
    std::string queueOutcome;
    std::map<std::string, std::string> contextActive;  // Dispatch-bound receipt IDs, never guessed from history.
    std::string lastRequest;
};

struct AgentStatus {
    std::string state = "checking";  // checking | ok | missing | loggedOut | wrongMethod | noPlan | error | loggingIn
    std::string plan;
    std::string email;
    std::string version;
    std::string message;
    std::wstring exe;
};

// The project's own rules for the local reviewers, from AGENTS.md or CLAUDE.md in the
// project folder (local.cpp). Empty when the folder has neither.
std::string projectReviewRules(const std::wstring& cwd);

// Avatars and banners live as plain file names in App::mediaDir() (profile.cpp).
bool isMediaName(const std::string& name);
void removeMediaFile(const std::string& file);
// A cropped picture from the page ("data:image/png;base64,...") as PNG bytes; empty unless it is a PNG.
std::string decodePngDataUrl(const std::string& data, bool screenshot = false);
// Deletes pictures picked for cropping but never saved.
void cleanTempMedia();

class App {
public:
    using SendFn = std::function<void(const std::string& json)>;

    explicit App(SendFn send);
    ~App();

    void onUiMessage(const std::string& json);
    void tick();
    void shutdown();

    std::string hostFlavor = "desktop";  // Set by the CLI host; independent of executable renaming.

    // Host hooks (Win32 side).
    std::function<void(const std::string& action)> windowAction;  // min | max | close | drag | flash
    std::function<std::wstring(const std::wstring& initial)> pickFolder;
    std::function<std::wstring()> pickImage;
    std::function<void(const std::wstring& target)> openExternal;
    // Desktop only: opens the Haiku phone simulator for an APK. Empty in the CLI.
    std::function<bool(const std::wstring& apk, std::string& error)> openHaiku;

    // Where custom avatars and banners live (served to the page as https://media.cca.app/).
    static std::wstring mediaDir();

private:
    friend struct AppearanceTestAccess;
    friend struct GeminiTestAccess;
    bool handleTheme(const Json& message);
    bool handleRouting(const Json& message);
    bool routeConfig(Conversation& c, AgentKind kind, AgentConfig& config, const std::string& request,
                     bool fixedModel = false, bool fixedEffort = false);
    friend struct DeskTestAccess;
    bool handleDesk(const Json& message);
    bool saveDesk(Conversation& c, const Json& next);
    void loadDesk(Conversation& c);
    void tickQueue(Conversation& c);
    void captureContext(Conversation& c, const std::string& agent, const AgentConfig& cfg, const std::string& input);
    void contextEvent(Conversation& c, AgentKind kind, const AgentEvent& event);
    // ---- version-control pane (gitpanel.cpp)
    friend struct GitTestAccess;
    bool handleGit(const Json& message);
    // checkAuth asks the remote whether a sign-in is needed (a network call); otherwise the cached answer is sent.
    void gitRefresh(Conversation& c, const std::string& op = "", const Json& result = Json(), bool checkAuth = false);
    // Records whether a fetch, pull or push reached the remote; a sign-in failure re-checks for the sign-in page.
    void gitRemoteOutcome(Conversation& c, bool ok, const std::string& problem, const std::string& op,
                          const Json& result);
    Json gitSettings(const std::string& id) const;
    void tickGit(long long now);
    std::set<std::string> gitBusy_;                // conversations with a Git command running
    std::map<std::string, Json> gitAuth_;          // last known remote sign-in state per conversation
    std::map<std::string, long long> gitFetchAt_;  // last automatic fetch per conversation
    long long gitCheckAt_ = 0;
    // ---- quota-aware automatic resume (takeover.cpp)
    long long quotaResetAt(AgentKind k) const;  // ms when this agent's quota returns; 0 when unknown
    long long autoResumeAt(const Conversation& c) const;
    void tickAutoResume(long long now);
    long long autoResumeCheckAt_ = 0;
    bool handleUpdate(const Json& message);
    bool updateIdle() const;
    void tickUpdate(long long now);
    Json update_ = Json::object();
    bool updateBusy_ = false;
    bool updateInstalling_ = false;
    unsigned updateGeneration_ = 0;
    std::string updateStamp_ = "unscanned";  // never a real fingerprint, so the first scan always inspects
    std::string updateSkip_;                 // version whose automatic install was cancelled or failed
    long long updateScanAt_ = 0;
    long long updateAutoAt_ = 0;
    friend struct QualityTestAccess;
    bool handleQuality(const Json& message);
    void qualityResult(const Json& message);
    void invalidateEvidence(Conversation& c, const std::string& reason);
    std::shared_ptr<ChildProcess> qualityWorker_;
    unsigned long long qualityGeneration_ = 0;
    std::map<std::string, Json> qualityPending_;
    std::set<std::string> qualityBusy_;
    friend struct CollaborationTestAccess;
    bool handleCollaboration(const Json& message);
    bool persistCollaboration(Conversation& c, const Json& next);
    void finishPlan(Conversation& c, const std::string& outcome);
    std::string reviewFocus(const Conversation& c, AgentKind agent) const;
    void sendInbox();
    bool inboxDirty_ = true;
    long long lastInbox_ = 0;
    void startOpinion(Conversation& c, const Json& message);
    void stopOpinion(Conversation& c);
    void opinionEvent(const std::string& convId, const std::string& agent, const std::string& runId,
                      const AgentEvent& event);
    void opinionLaunch(Conversation& c, const std::string& agent, const std::string& text);
    void handleWorktree(Conversation& c, const Json& message);
    friend struct PipelineTestAccess;
    Json pipelineNode(const Conversation& c) const;
    void pipelineStart(Conversation& c, const std::string& request, bool resume = false);
    void pipelineNext(Conversation& c);
    bool startPipelineAgent(Conversation& c, const std::string& text, std::string* error);
    void pipelineDone(Conversation& c, const Json& message, const std::string& status);
    void pipelineBlock(Conversation& c, const std::string& reason);
    // Moves to the next node; at the end of a cycle it finishes, blocks or starts the next cycle.
    void pipelineAdvance(Conversation& c);
    bool pipelineNodeOut(const Json& node) const;  // a subscription agent with no quota (or no usable account)
    void pipelineSkipNode(Conversation& c, const Json& node);
    void clearPipelineSessions(Conversation& c);
    void invalidatePipelineTask(Conversation& c, const std::string& revision, const std::string& reason);
    bool handlePlatform(const Json& message);
    void importArtifact(Conversation& c, const Json& artifact);
    void sendPlatform();
    std::vector<std::shared_ptr<RuntimeSession>> controls_;
    friend struct TakeoverTestAccess;
    friend struct EditionTestAccess;
    friend struct ProfileTestAccess;
    bool claudeLimited() const;
    void recordClaudeLimit(const Json& info);
    // Live Claude usage (opt-in, settings claudeUsageLive): every 60 s, every 20 s while
    // Claude works, and right after each Claude turn or a manual refresh (takeover.cpp).
    void pollClaudeUsage(bool force);
    bool takeOver(Conversation& c, const std::string& reason, std::string* error);
    void handleTakeover(Conversation& c);
    std::string handoverPrompt(const Conversation& c, const std::string& request) const;
    // Provider quota per agent (takeover.cpp): known exhaustion, and whether a failed turn was one.
    bool agentExhausted(AgentKind k) const;
    bool quotaFailure(AgentKind k, const Json& message) const;
    void recordQuotaHit(AgentKind k, long long resetsAtMs);
    std::map<std::string, long long> quotaUntil_;  // agent -> ms; set by a failed turn with an unknown reset
    // ---- profiles, personas and daily mode (profile.cpp)
    std::string nameOf(AgentKind k) const;
    std::string userName() const;
    Json allPersonas() const;
    std::string personaPrompt(AgentKind k) const;
    std::string dailyModel(AgentKind k) const;
    std::string dailyEffort(AgentKind k) const;
    void ensureProfileDefaults();
    void sendSettings();
    Json badgeState() const;
    void handleBadgeDisplay(const Json& m);
    Json groupOrder() const;
    void handleGroupOrder(const Json& message);
    void reconfigureAll();
    bool persistProfile(const std::string& who, const Json& profile);
    bool persistPersonaSettings(const Json& previous);
    bool persistGroupAvatar(Conversation& c, const std::string& file);
    void handleProfileSet(const Json& m);
    void handleProfileImage(const Json& m);
    void beginImage(const std::string& who, const std::string& kind, const std::string& convId);
    void handleImageAction(const Json& m);
    void handlePersonaSave(const Json& m);
    void handlePersonaDelete(const Json& m);
    void handlePersonaAssign(const Json& m);
    void handleDailySet(const Json& m);
    // 去 AI 化 (humanize.cpp holds the rules)
    bool humanizeOn() const { return settings_["humanize"]["enabled"].boolean(); }
    void handleHumanizeSet(const Json& m);
    // Compliance checks on every reply (guard.hpp holds the rules)
    bool guardOn() const { return settings_["guard"]["enabled"].boolean(true); }
    int guardRetries() const { return (int)settings_["guard"]["retries"].i64(2); }
    GuardRules guardRulesFor(const Conversation& c, AgentKind k) const;
    // true: the reply was sent back for a rewrite, or the group paused over it.
    bool guardReply(Conversation& c, AgentKind k, const std::string& msgId);

    // ---- core (app.cpp)
    void send(const Json& msg);
    void toast(const std::string& level, const std::string& text);
    void load();
    void saveMeta();
    bool saveConversations(const std::string& omit = "");
    void saveMessages(Conversation& c);
    void ensureDefaults();
    Json convMeta(const Conversation& c) const;
    Json convState(const Conversation& c) const;
    void sendConvs();
    void sendBoot();
    Conversation* find(const std::string& id);
    Json* findMsg(Conversation& c, const std::string& msgId);
    Json& pushMessage(Conversation& c, Json msg);
    void markDirty(Conversation& c, const std::string& msgId);
    AgentConfig configFor(const Conversation& c, AgentKind k, bool routed = true) const;
    ClaudeSession& claudeFor(Conversation& c);
    CodexThread* codexFor(Conversation& c, std::string* error);
    bool sendToAgent(Conversation& c, AgentKind k, const std::string& text, std::string* error);
    void onAgentEvent(const std::string& convId, AgentKind k, const AgentEvent& ev);
    void applySettings(Conversation& c, const Json& s);
    void resetConversation(Conversation& c);
    void handleApproval(const Json& m);
    void handleQuestion(const Json& m);
    void ensureCodexServer();
    void onCodexGlobal(const std::string& method, const Json& params);
    void sendUsage();
    void sendModels();
    bool anyBusy(const Conversation& c) const;
    void startAgent(Conversation& c, AgentKind k, const std::string& text);
    void sendFailed(Conversation& c, AgentKind k, const std::string& error);
    void recordUsage(const std::string& agent, const Json& usage);
    bool saveUsage();

    // ---- checkpoints and rollback (rollback.cpp)
    void handleRollback(const Json& m);
    void handleRollbackUndo(const Json& m);
    // ---- review pane (review.cpp): changes since a checkpoint, reverting a file or one hunk
    std::string reviewBase(const Conversation& c, const std::string& scope) const;
    void handleReviewOpen(const Json& m);
    void handleReviewRevert(const Json& m);

    // ---- local models through Ollama (local.cpp)
    bool localEnabled(AgentKind k) const;
    std::string localModel(AgentKind k) const;
    LocalSession& localFor(Conversation& c, AgentKind k);
    void ensureOllama(std::function<void(bool ok, const std::string& error)> then);
    void sendLocalStatus();
    void handleLocalPull(const Json& m);
    void handleLocalDelete(const Json& m);
    void handleLocalSet(const Json& m);
    std::string localReviewPrompt(const Conversation& c) const;
    void groupRunLocals(Conversation& c);
    void groupLocalDone(Conversation& c, AgentKind k, const Json& msg, const std::string& status);
    void groupProceed(Conversation& c);
    // Ollama capabilities per model ("tools", "thinking"...), fetched once and cached.
    void localCapabilities(const std::string& model, std::function<void(const Json& caps)> then);

    // ---- subscription checks and login (auth.cpp)
    void runAuthCheck(bool claude, bool codex);
    void checkClaude();
    void checkCodex();
    void loginClaude();
    void loginCodex();
    void checkGemini();
    void loginGemini();
    std::wstring resolveGeminiExe() const;
    void sendAuth();
    std::wstring resolveClaudeExe() const;
    std::wstring resolveCodexExe() const;
    void probeClaudeModels();
    void refreshCodexAccount();

    // ---- editions (editions.cpp): keep what a special edition does not support, restore it on return
    // `now` is this build's edition; tests pass another one to exercise both directions in one binary.
    void editionTransition(const std::string& now);
    std::string editionNotice_;

    // ---- CCA plugins (extensions.cpp): data-only themes, conversation modes and prompt commands
    void loadExtensions();
    Json extensionState() const;
    const Json* extensionMode(const std::string& id) const;
    std::string extensionModeInstructions(const Conversation& c) const;
    void expandExtensionCommand(const Conversation& c, std::string& message);
    bool handleExtensions(const Json& m);
    void sendExtensions();
    Json extensions_ = Json::array();

    // ---- group collaboration (group.cpp)
    void groupStart(Conversation& c, const std::string& text);
    void groupTurnDone(Conversation& c, AgentKind k, const Json& msg, const std::string& status);
    void groupTurnJudged(Conversation& c, AgentKind k, int verdict);
    // ---- reviewer regression guard (regression_guard.cpp)
    bool groupGuardApplies(const Conversation& c, AgentKind k) const;
    void groupGuardStart(Conversation& c, AgentKind k, int verdict);
    void groupGuardDone(const std::string& convId, AgentKind k, int verdict, const Json& outcome);
    void groupGuardReverted(Conversation& c, AgentKind k, const Json& outcome, const Json& attempted);
    void groupHandOff(Conversation& c, AgentKind next, const std::string& prompt, const std::string& phase);
    void groupFinish(Conversation& c, const std::string& outcome, const std::string& detail);
    void groupPause(Conversation& c, const std::string& reason, AgentKind next, const std::string& prompt,
                    const std::string& phase, int round, const std::string& kind = "");
    void groupContinue(Conversation& c);
    void groupAccept(Conversation& c);  // take a reply that failed the checks as it is
    void groupStop(Conversation& c);
    // Picks up a task CCA closed in the middle of, from its saved handoffs (group.cpp).
    void taskResume(Conversation& c);
    std::string reviewPrompt(const Conversation& c, AgentKind from, AgentKind to) const;
    std::string groupInstructions(const Conversation& c, AgentKind k) const;
    std::vector<AgentKind> groupMembers(const Conversation& c) const;
    bool groupCanRun(const Conversation& c, AgentKind k) const;
    // Records the skip, then hands the work to the next member with quota or stops the whole task.
    void groupSkip(Conversation& c, AgentKind k, const std::string& reason);
    void groupQuotaStop(Conversation& c, const std::string& detail);
    GeminiSession& geminiFor(Conversation& c);

    // ---- plugins and MCP servers (plugins.cpp)
    void listPlugins(const std::string& agent, bool refresh);
    void installPlugin(const Json& m);
    void uninstallPlugin(const Json& m);
    void listMcp();
    void addMcp(const Json& m);
    void removeMcp(const Json& m);
    void restartIdleSessions();
    void setBusy(const std::string& key, bool busy);

    SendFn send_;
    std::vector<std::unique_ptr<Conversation>> convs_;
    Json settings_ = Json::object();
    Json pendingImage_ = Json::object();  // The host-issued crop token binds a temporary image to its target.
    AgentStatus claudeStatus_, codexStatus_;
    AgentStatus geminiStatus_;
    std::unique_ptr<ChildProcess> geminiLogin_;
    bool geminiChecking_ = false;
    unsigned geminiGeneration_ = 0;
    long long lastGeminiUsage_ = 0;
    std::shared_ptr<CodexServer> codexServer_;
    std::shared_ptr<ClaudeSession> claudeProbe_;
    std::unique_ptr<ChildProcess> claudeLogin_;
    Json claudeModels_ = Json::array();
    Json codexModels_ = Json::array();
    Json usage_ = J({{"claude", Json::object()}, {"codex", Json::object()}});
    std::string codexLoginId_;
    Snapshots snapshots_;
    Json usageHistory_ = Json::object();   // "YYYY-MM-DD" -> agent -> totals
    Json personaPresets_ = Json::array();  // optional user-owned file, never embedded
    Json badgeGrants_ = Json::object();    // local decoration grants, never inferred from a name
    std::string badgeGrantProblem_;
    std::string personaPresetProblem_;
    bool usageDirty_ = false;
    std::unique_ptr<ChildProcess> ollamaServe_;
    Json hardware_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> pulls_;
    Json localCaps_ = Json::object();  // model -> Ollama capabilities array
    bool uiReady_ = false;
    bool metaDirty_ = false;
    long long lastSave_ = 0;
    long long lastClaudeUsage_ = 0;
    bool claudeUsageBusy_ = false;
    bool shuttingDown_ = false;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
