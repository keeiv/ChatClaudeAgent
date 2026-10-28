// Optional provider worker: bounded JSON-lines IPC, sharing the existing event vocabulary.
#pragma once

#include <memory>

#include "agent.hpp"
#include "process.hpp"

// Python for the worker scripts, or empty when only the Microsoft Store stub (or nothing) is on PATH.
std::wstring runtimePython();
// Path of a bundled worker script, extracted from the exe's resources (or tools\ in a source checkout).
std::wstring runtimeScript(const wchar_t* resourceName, const wchar_t* fileName);

class RuntimeSession : public std::enable_shared_from_this<RuntimeSession> {
public:
    RuntimeSession(AgentEventFn events, std::function<void(const Json&)> control = {});
    ~RuntimeSession();
    bool start(const Json& request, std::string* error);
    void respond(const std::string& id, const std::string& choice);
    void shutdown();
    bool busy() const { return busy_; }

private:
    AgentEventFn events_;
    std::function<void(const Json&)> control_;
    std::unique_ptr<ChildProcess> process_;
    bool busy_ = false;
    std::string sandboxOwner_;
};
