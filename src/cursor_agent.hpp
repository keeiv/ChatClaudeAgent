// Cursor Agent CLI headless transport (stream-json); login, models and command approval stay in the official CLI.
#pragma once

#include <memory>

#include "agent.hpp"
#include "process.hpp"

// The CLI is a Node bundle; CCA starts its own node.exe directly instead of the .cmd/.ps1 launchers,
// so multi-line prompts and non-ASCII project paths never pass through cmd.exe or PowerShell parsing.
struct CursorLaunch {
    std::wstring node;
    std::wstring script;
    std::wstring version;  // the version folder name, for the account card
    bool ok() const { return !node.empty() && !script.empty(); }
};

class CursorSession : public std::enable_shared_from_this<CursorSession> {
public:
    explicit CursorSession(AgentEventFn events) : events_(std::move(events)) {}
    ~CursorSession() { shutdown(); }
    bool send(const CursorLaunch& launch, const AgentConfig& config, const std::string& text, std::string* error);
    void shutdown();
    bool busy() const { return busy_; }

    static std::vector<std::wstring> envToDrop();
    // Empty when CCA can start Cursor with this permission in this folder, otherwise the reason for the user.
    static std::string permissionProblem(const std::string& permission, const std::wstring& cwd);
    // `agent models` prints "id - Display Name" lines; only those become catalogue entries.
    static Json parseModels(const std::string& text);
    // Resolves an install folder, a version folder or an index.js to the node.exe and bundle CCA starts.
    static CursorLaunch resolve(const std::wstring& configured);

private:
    void line(const std::string& text);
    void emit(AgentEvent::Kind kind, const std::string& id, Json data);
    AgentEventFn events_;
    std::unique_ptr<ChildProcess> process_;
    bool busy_ = false, resultOk_ = false, gotResult_ = false, wroteText_ = false, thinking_ = false;
    unsigned generation_ = 0;
    std::string stderr_;
    Json usage_ = Json::object();
};
