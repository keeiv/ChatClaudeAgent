// Child processes with piped stdio, read line by line on background threads.
// Every child is placed in a job object so closing CCA also ends its whole tree.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct ProcessOptions {
    std::wstring exe;  // full path; .cmd and .bat files run through cmd.exe
    std::vector<std::wstring> args;
    std::wstring cwd;                    // empty inherits CCA's directory
    std::vector<std::wstring> dropEnv;   // exact names, or prefixes ending in '*'
    std::vector<std::pair<std::wstring, std::wstring>> setEnv;
    bool newConsole = false;             // visible console, no pipes (interactive login)
};

class ChildProcess {
public:
    using LineFn = std::function<void(const std::string&)>;
    using ExitFn = std::function<void(unsigned long)>;

    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Callbacks run on background threads. onExit runs after stdout has been fully read.
    bool start(const ProcessOptions& opt, LineFn onStdout, LineFn onStderr, ExitFn onExit, std::string* error);
    bool writeLine(const std::string& line);
    void closeStdin();
    void terminate();
    bool running() const { return running_; }
    DWORD pid() const { return pid_; }

private:
    void readLoop(HANDLE h, LineFn fn, bool isStdout);

    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    DWORD pid_ = 0;
    std::thread outThread_, errThread_, waitThread_;
    std::mutex writeMu_;
    std::atomic<bool> running_{false};
    ExitFn onExit_;
};

// Builds the command line CreateProcess will see (exposed for tests).
std::wstring buildCommandLine(const ProcessOptions& opt);

// Runs a command and waits for it on the calling thread. Returns the exit code,
// -1 if it could not start, -2 on timeout (the process is then killed).
int runWait(const ProcessOptions& opt, std::string* out, std::string* err, DWORD timeoutMs);

// Runs a command to completion on a worker thread and reports its output.
// `done` runs on that worker thread.
void runCapture(const ProcessOptions& opt,
                std::function<void(int exitCode, std::string out, std::string err)> done,
                DWORD timeoutMs = 120000);

// The update broker is the sole child that must outlive CCA's job. It only accepts a staged EXE/plan.
bool launchUpdateBroker(const std::wstring& exe, const std::wstring& plan, std::string* error);
// Broker-only relaunch. The caller closes the returned process handle after verifying startup.
HANDLE launchUpdatedApplication(const std::wstring& exe, const std::wstring& ready, bool console, std::string* error);
