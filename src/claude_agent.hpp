// Drives `claude -p` in stream-json mode for one conversation.
// All public methods must be called on the UI thread.
#pragma once

#include <map>
#include <memory>
#include <string>

#include "agent.hpp"
#include "process.hpp"

class ClaudeSession : public std::enable_shared_from_this<ClaudeSession> {
public:
    ClaudeSession(std::wstring exe, AgentEventFn onEvent);
    ~ClaudeSession();

    // Applies new settings. Model and permission changes go to the live process;
    // effort, folder or instruction changes restart it on the next message.
    void configure(const AgentConfig& cfg);
    // Starts the CLI (if needed) and sends the initialize request only.
    bool start(std::string* error);
    // Asks the running CLI for MCP connection/tool status without a model request.
    void requestMcpStatus();
    bool send(const std::string& text, std::string* error);
    void interrupt();
    void respondApproval(const std::string& id, const std::string& choice, const std::string& feedback);
    void respondQuestion(const std::string& id, const Json& answers);
    void shutdown();
    // Chat rollback: the next start resumes the conversation cut after this assistant
    // message (forked into a new session). Empty uuid starts a brand-new session.
    void rewindTo(const std::string& uuid);

    bool busy() const { return busy_; }
    bool alive() const { return proc_ && proc_->running(); }
    const std::string& sessionId() const { return sessionId_; }

    static std::string permissionMode(const std::string& preset);
    // Environment variables removed before starting any claude process.
    static std::vector<std::wstring> envToDrop();

private:
    struct Pending {
        std::string tool;
        Json input;
        Json suggestions;
    };
    struct ToolUse {
        std::string name;
        Json input;
    };

    void handleLine(const std::string& line);
    void handleStreamEvent(const Json& ev);
    void handleAssistant(const Json& msg);
    void handleToolResults(const Json& msg);
    void handlePermissionRequest(const std::string& requestId, const Json& req);
    void handleResult(const Json& msg);
    void sendControl(const std::string& subtype, Json extra);
    void sendControlResponse(const std::string& requestId, Json response);
    void emit(AgentEvent::Kind kind, const std::string& id, Json data);

    std::wstring exe_;
    AgentEventFn onEvent_;
    AgentConfig cfg_;
    AgentConfig runningCfg_;
    std::unique_ptr<ChildProcess> proc_;
    std::wstring promptFile_;
    std::wstring mcpFile_;
    std::string sessionId_;
    std::string stderrTail_;
    bool busy_ = false;
    bool interrupted_ = false;
    bool quotaExceeded_ = false;
    bool needsRestart_ = false;
    int controlSeq_ = 0;
    unsigned processGeneration_ = 0;

    // Streaming text blocks of the current assistant message, in order.
    std::string streamMsgId_;
    std::vector<std::string> openText_;
    std::map<int, std::string> streamIndexToId_;
    std::map<std::string, ToolUse> tools_;
    std::map<std::string, Pending> pending_;
    Json lastUsage_;
    std::string lastUuid_;
    std::string resumeAt_;
    bool freshStart_ = false;
};
