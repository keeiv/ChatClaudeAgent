// Gemini CLI headless transport; permissions remain enforced by the official CLI.
#pragma once

#include <memory>

#include "agent.hpp"
#include "process.hpp"

class GeminiSession : public std::enable_shared_from_this<GeminiSession> {
public:
    explicit GeminiSession(AgentEventFn events) : events_(std::move(events)) {}
    ~GeminiSession() { shutdown(); }
    bool send(const std::wstring& exe, const AgentConfig& config, const std::string& text, std::string* error);
    void shutdown();
    bool busy() const { return busy_; }
    static std::vector<std::wstring> envToDrop();

private:
    void line(const std::string& text);
    void emit(AgentEvent::Kind kind, const std::string& id, Json data);
    AgentEventFn events_;
    std::unique_ptr<ChildProcess> process_;
    bool busy_ = false, resultOk_ = false, gotResult_ = false;
    unsigned generation_ = 0;
    std::string stderr_;
};
