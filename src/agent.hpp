// Shared vocabulary between native agent backends
// and the rest of CCA. Each backend turns its own protocol into AgentEvents.
#pragma once

#include <functional>
#include <string>

#include "json.hpp"

// Runs fn on the UI thread. Implemented by the host (main.cpp) or by tests.
void postToUi(std::function<void()> fn);

// Local1/Local2 are the two optional Ollama models; they only ever review read-only.
enum class AgentKind { Claude, Codex, Local1, Local2, Gemini };

inline const char* agentName(AgentKind k) {
    switch (k) {
    case AgentKind::Claude: return "claude";
    case AgentKind::Codex: return "codex";
    case AgentKind::Local1: return "local1";
    case AgentKind::Gemini:
        return "gemini";
    default: return "local2";
    }
}
inline bool isLocal(AgentKind k) { return k == AgentKind::Local1 || k == AgentKind::Local2; }

struct AgentEvent {
    enum class Kind {
        TurnStarted,  // data: {}
        TextDelta,    // id, data: {delta}
        Text,         // id, data: {text} final text of a block
        Thinking,     // data: {} the model is reasoning
        ToolStart,    // id, data: {kind, tool, title, detail, command, cwd, path}
        ToolOutput,   // id, data: {delta}
        ToolEnd,      // id, data: {status, output, exitCode, durationMs}
        FileChange,   // id, data: {path, change, added, removed, diff}
        Approval,     // id, data: approval card (see app.cpp)
        Question,     // id, data: {questions}
        Resolved,     // id, data: {state} approval/question was answered or cancelled
        Todo,         // data: {items:[{text, status}]}
        Usage,      // data: {inputTokens, outputTokens, cachedTokens, contextUsed, contextWindow, costUsd, durationMs}
        RateLimit,  // data: agent specific limits, normalized in app.cpp
        Retry,      // data: {attempt, max, delayMs, error}
        Notice,     // data: {text, level}
        Artifact,   // data: project-relative PNG path and test provenance; validated by the host
        Error,      // data: {text}
        TurnDone,   // data: {status: completed|interrupted|failed, error}
        SessionInfo,  // data: {sessionId|threadId, model, models, account, permissionMode}
        Exited,       // data: {code, stderr}
    };
    Kind kind;
    std::string id;
    Json data;
};

using AgentEventFn = std::function<void(const AgentEvent&)>;

// How to start tools/sandbox_mcp.py, the Docker sandbox both agents share. Empty script: not offered.
struct SandboxMcp {
    std::wstring python, script, dataDir, docker;
    bool on() const { return !script.empty(); }
};

// Per-conversation settings that both backends understand.
struct AgentConfig {
    std::wstring cwd;
    std::string model;         // empty: the CLI's default
    std::string effort;        // empty: the model's default
    std::string permission;    // plan | ask | edit | bypass
    std::string instructions;  // extra role instructions (used by groups)
    Json routing = Json::object();  // Host selection receipt; not sent as model instructions.
    std::string resumeId;      // Claude session id or Codex thread id
    SandboxMcp sandbox;
};

// Choices offered on approval cards.
namespace choice {
inline const char* kAllow = "allow";
inline const char* kAllowSession = "allowSession";
inline const char* kDeny = "deny";
inline const char* kAbort = "abort";
}  // namespace choice
