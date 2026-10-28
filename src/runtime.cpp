// Provider isolation from the UI process; packaged worker source is extracted from resources.
#include "runtime.hpp"

#include <windows.h>

#include <map>

#include "util.hpp"

std::wstring runtimeScript(const wchar_t* resourceName, const wchar_t* fileName) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, resourceName, RT_RCDATA);
    if (resource) {
        DWORD size = SizeofResource(module, resource);
        const char* bytes = (const char*)LockResource(LoadResource(module, resource));
        // All worker scripts share one folder: sandbox_mcp.py imports agent_runtime.py from its own directory.
        std::wstring dir = util::joinPath(util::appDataDir(), L"runtime");
        util::ensureDir(dir);
        std::wstring file = util::joinPath(dir, fileName);
        std::string text;
        if (!util::readFile(file, text) || text != std::string(bytes, size)) {
            if (!util::writeFileAtomic(file, std::string(bytes, size))) return L"";
        }
        return file;
    }
    return util::joinPath(util::exeDir(), std::wstring(L"..\\tools\\") + fileName);
}

std::wstring runtimePython() {
    std::wstring python = util::findOnPath(L"python");
    // The Microsoft Store stub under WindowsApps only opens the Store; it is not an interpreter.
    if (util::toLower(util::narrow(python)).find("\\windowsapps\\") != std::string::npos) return L"";
    return python;
}

RuntimeSession::RuntimeSession(AgentEventFn events, std::function<void(const Json&)> control)
    : events_(std::move(events)), control_(std::move(control)) {}

RuntimeSession::~RuntimeSession() {
    shutdown();
}

bool RuntimeSession::start(const Json& request, std::string* error) {
    std::wstring python = runtimePython();
    std::wstring script = runtimeScript(L"RUNTIME_AGENT_PY", L"agent_runtime.py");
    if (python.empty() || !util::fileExists(script)) {
        if (error) *error = "API 與工具工作程序需要 Python 3.10 以上；沒有自動安裝或啟動模型";
        return false;
    }
    ProcessOptions options;
    options.exe = python;
    options.args = {L"-I", L"-u", script};
    options.cwd = util::appDataDir();
    options.dropEnv = {L"OPENAI_API_KEY", L"ANTHROPIC_API_KEY", L"CLAUDE_CODE_OAUTH_TOKEN", L"PYTHONPATH"};
    process_ = std::make_unique<ChildProcess>();
    auto weak = weak_from_this();
    busy_ = true;
    bool started = process_->start(
        options,
        [weak](const std::string& line) {
            if (line.size() > 12 * 1024 * 1024) return;
            Json message = Json::parse(line);
            postToUi([weak, message] {
                auto self = weak.lock();
                if (!self) return;
                if (message.has("control")) {
                    self->busy_ = false;
                    if (self->control_) self->control_(message["control"]);
                    return;
                }
                static const std::map<std::string, AgentEvent::Kind> kinds = {
                    {"TurnStarted", AgentEvent::Kind::TurnStarted},
                    {"Thinking", AgentEvent::Kind::Thinking},
                    {"Text", AgentEvent::Kind::Text},
                    {"ToolStart", AgentEvent::Kind::ToolStart},
                    {"ToolEnd", AgentEvent::Kind::ToolEnd},
                    {"FileChange", AgentEvent::Kind::FileChange},
                    {"Approval", AgentEvent::Kind::Approval},
                    {"Resolved", AgentEvent::Kind::Resolved},
                    {"Usage", AgentEvent::Kind::Usage},
                    {"Retry", AgentEvent::Kind::Retry},
                    {"Artifact", AgentEvent::Kind::Artifact},
                    {"TurnDone", AgentEvent::Kind::TurnDone}};
                auto it = kinds.find(message["event"].str());
                if (it == kinds.end() || !self->busy_) return;
                if (it->second == AgentEvent::Kind::TurnDone) self->busy_ = false;
                if (self->events_) self->events_({it->second, message["id"].str(), message["data"]});
            });
        },
        [](const std::string&) {},
        [weak](unsigned long) {
            postToUi([weak] {
                auto self = weak.lock();
                if (!self || !self->busy_) return;
                self->busy_ = false;
                if (self->events_)
                    self->events_(
                        {AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}, {"error", "代理工作程序已中斷"}})});
                if (self->control_) self->control_(J({{"ok", false}, {"error", "管理工作程序已中斷"}}));
            });
        },
        error);
    if (!started) {
        busy_ = false;
        return false;
    }
    Json initial = request;
    sandboxOwner_ = util::newId();
    initial["sandbox_owner"] = sandboxOwner_;
    process_->writeLine(initial.dump());
    return true;
}

void RuntimeSession::respond(const std::string& id, const std::string& choice) {
    if (process_) process_->writeLine(J({{"id", id}, {"choice", choice}}).dump());
}

void RuntimeSession::shutdown() {
    bool cancel = busy_;
    busy_ = false;
    if (process_) process_->terminate();
    process_.reset();
    if (cancel && !sandboxOwner_.empty() && !util::findOnPath(L"docker").empty()) {
        ProcessOptions options;
        options.exe = util::findOnPath(L"docker");
        options.args = {L"rm", L"--force", L"cca-" + util::widen(sandboxOwner_)};
        options.dropEnv = {L"DOCKER_*"};
        runCapture(options, [](int, std::string, std::string) {}, 10000);
    }
    sandboxOwner_.clear();
}
