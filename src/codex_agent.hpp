// Talks to one shared `codex app-server` process (JSON-RPC over stdio) and
// runs one Codex thread per conversation. UI thread only.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent.hpp"
#include "process.hpp"

class CodexServer : public std::enable_shared_from_this<CodexServer> {
public:
    using Reply = std::function<void(const Json& result, const Json& error)>;
    // method, params, and the request id when the server expects an answer (null otherwise).
    using Handler = std::function<void(const std::string& method, const Json& params, const Json& requestId)>;

    explicit CodexServer(std::wstring exe);
    ~CodexServer();

    bool start(std::string* error);
    void stop();
    bool running() const { return proc_ && proc_->running(); }
    bool ready() const { return ready_; }
    const std::wstring& exe() const { return exe_; }

    // Requests sent before the handshake completes are queued.
    void request(const std::string& method, Json params, Reply reply = nullptr);
    void respond(const Json& id, Json result);
    void respondError(const Json& id, int code, const std::string& message);

    void setThreadHandler(const std::string& threadId, Handler h);
    void removeThreadHandler(const std::string& threadId);

    // Notifications that are not tied to a thread (account, rate limits, login).
    std::function<void(const std::string& method, const Json& params)> onGlobal;
    std::function<void()> onReady;
    std::function<void(const std::string& reason)> onExit;

private:
    friend struct CodexThreadTestAccess;
    void handleLine(const std::string& line);
    void write(const Json& msg);

    std::wstring exe_;
    std::unique_ptr<ChildProcess> proc_;
    bool ready_ = false;
    long long nextId_ = 1;
    std::map<long long, Reply> replies_;
    std::deque<Json> queued_;
    std::map<std::string, Handler> threads_;
    std::string stderrTail_;
};

class CodexThread : public std::enable_shared_from_this<CodexThread> {
public:
    CodexThread(std::shared_ptr<CodexServer> server, AgentEventFn onEvent);
    ~CodexThread();

    void configure(const AgentConfig& cfg);
    bool send(const std::string& text, std::string* error);
    void interrupt();
    void respondApproval(const std::string& id, const std::string& choice, const std::string& feedback);
    void respondQuestion(const std::string& id, const Json& answers);
    void shutdown();
    // The server process died: forget the thread binding but keep the id for resume.
    void serverLost();
    // Chat rollback: drop this turn and every later one from the thread's history.
    void revert(const std::string& beforeTurnId, std::function<void(bool ok, const std::string& error)> done);

    bool busy() const { return busy_; }
    const std::string& threadId() const { return threadId_; }

    static Json sandboxPolicy(const std::string& preset);
    static std::string sandboxMode(const std::string& preset);
    static std::string approvalPolicy(const std::string& preset);

private:
    friend struct CodexThreadTestAccess;
    void ensureThread(std::function<void(bool ok, const std::string& error)> then);
    void startTurn(const std::string& text);
    void handle(const std::string& method, const Json& params, const Json& requestId);
    void handleItem(const Json& item, bool completed);
    void emit(AgentEvent::Kind kind, const std::string& id, Json data);
    Json threadParams() const;

    std::shared_ptr<CodexServer> server_;
    AgentEventFn onEvent_;
    AgentConfig cfg_;
    std::string threadId_;
    std::string turnId_;
    bool busy_ = false;
    bool interrupted_ = false;
    bool binding_ = false;
    bool needsRebind_ = false;
    bool sandboxWarned_ = false;
    std::string pendingText_;
    Json lastUsage_;

    struct PendingApproval {
        Json rpcId;
        std::string kind;  // command | file | question | permissions
        Json questions;
    };
    std::map<std::string, PendingApproval> pending_;
    std::map<std::string, Json> fileChanges_;  // itemId -> changes, for approval previews
};
