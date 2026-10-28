#include "process.hpp"

#include <algorithm>
#include <condition_variable>
#include <memory>

#include "util.hpp"

namespace {

bool isScript(const std::wstring& exe) {
    size_t dot = exe.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = exe.substr(dot);
    return _wcsicmp(ext.c_str(), L".cmd") == 0 || _wcsicmp(ext.c_str(), L".bat") == 0;
}

bool dropped(const std::wstring& name, const std::vector<std::wstring>& drops) {
    for (const auto& d : drops) {
        if (!d.empty() && d.back() == L'*') {
            std::wstring prefix = d.substr(0, d.size() - 1);
            if (name.size() >= prefix.size() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0) return true;
        } else if (_wcsicmp(name.c_str(), d.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<wchar_t> buildEnvironment(const ProcessOptions& opt) {
    std::vector<std::wstring> entries;
    LPWCH env = GetEnvironmentStringsW();
    for (LPWCH p = env; *p; p += wcslen(p) + 1) {
        std::wstring e = p;
        size_t eq = e.find(L'=', 1);  // entries like "=C:=C:\" start with '='
        std::wstring name = eq == std::wstring::npos ? e : e.substr(0, eq);
        if (dropped(name, opt.dropEnv)) continue;
        bool overridden = false;
        for (const auto& kv : opt.setEnv) {
            if (_wcsicmp(kv.first.c_str(), name.c_str()) == 0) { overridden = true; break; }
        }
        if (!overridden) entries.push_back(e);
    }
    FreeEnvironmentStringsW(env);
    for (const auto& kv : opt.setEnv) entries.push_back(kv.first + L"=" + kv.second);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

HANDLE makeKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    // BREAKAWAY_OK: Codex's Windows sandbox starts its restricted processes with
    // CREATE_BREAKAWAY_FROM_JOB and manages them in its own job.
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    return job;
}

}  // namespace

std::wstring buildCommandLine(const ProcessOptions& opt) {
    std::wstring inner = util::quoteArg(opt.exe);
    for (const auto& a : opt.args) inner += L" " + util::quoteArg(a);
    if (!isScript(opt.exe)) return inner;
    // /s strips exactly the outer quotes, leaving the inner line untouched.
    return L"cmd.exe /d /s /c \"" + inner + L"\"";
}

ChildProcess::~ChildProcess() {
    terminate();
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    if (waitThread_.joinable()) waitThread_.join();
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
}

bool ChildProcess::start(const ProcessOptions& opt, LineFn onStdout, LineFn onStderr, ExitFn onExit,
                         std::string* error) {
    onExit_ = std::move(onExit);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    auto closeAll = [&] {
        for (HANDLE h : {inR, inW, outR, outW, errR, errW}) {
            if (h) CloseHandle(h);
        }
    };

    if (!opt.newConsole) {
        if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 1 << 16) ||
            !CreatePipe(&errR, &errW, &sa, 1 << 16)) {
            if (error) *error = "CreatePipe failed";
            closeAll();
            return false;
        }
        // Parent ends must not leak into the child.
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    std::vector<char> attrBuf;
    HANDLE inherit[3] = {inR, outW, errW};
    if (!opt.newConsole) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = inR;
        si.StartupInfo.hStdOutput = outW;
        si.StartupInfo.hStdError = errW;
        // Only these three handles are inherited, even if another thread is creating pipes too.
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrBuf.resize(size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                  nullptr, nullptr);
    }

    std::wstring cmd = buildCommandLine(opt);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    std::vector<wchar_t> env = buildEnvironment(opt);

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    flags |= opt.newConsole ? CREATE_NEW_CONSOLE : (CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, opt.newConsole ? FALSE : TRUE, flags,
                             env.data(), opt.cwd.empty() ? nullptr : opt.cwd.c_str(),
                             &si.StartupInfo, &pi);
    DWORD lastErr = GetLastError();
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        if (error) {
            *error = "無法啟動 " + util::narrow(opt.exe) + "（錯誤碼 " + std::to_string(lastErr) + "）";
        }
        closeAll();
        return false;
    }

    job_ = makeKillOnCloseJob();
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    running_ = true;

    if (opt.newConsole) {
        waitThread_ = std::thread([this] {
            WaitForSingleObject(process_, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(process_, &code);
            running_ = false;
            if (onExit_) onExit_(code);
        });
        return true;
    }

    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    stdinWrite_ = inW;
    outThread_ = std::thread([this, outR, fn = std::move(onStdout)] { readLoop(outR, fn, true); });
    errThread_ = std::thread([this, errR, fn = std::move(onStderr)] { readLoop(errR, fn, false); });
    return true;
}

void ChildProcess::readLoop(HANDLE h, LineFn fn, bool isStdout) {
    std::string pending;
    char buf[8192];
    while (true) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr) || n == 0) break;
        pending.append(buf, n);
        size_t start = 0;
        while (true) {
            size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = pending.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (fn) fn(line);
            start = nl + 1;
        }
        pending.erase(0, start);
    }
    if (!pending.empty() && fn) fn(pending);
    CloseHandle(h);
    if (isStdout) {
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        running_ = false;
        if (onExit_) onExit_(code);
    }
}

bool ChildProcess::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(writeMu_);
    if (!stdinWrite_) return false;
    std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size()) {
        DWORD n = 0;
        if (!WriteFile(stdinWrite_, data.data() + off, (DWORD)(data.size() - off), &n, nullptr)) return false;
        off += n;
    }
    return true;
}

void ChildProcess::closeStdin() {
    std::lock_guard<std::mutex> lock(writeMu_);
    if (stdinWrite_) {
        CloseHandle(stdinWrite_);
        stdinWrite_ = nullptr;
    }
}

void ChildProcess::terminate() {
    closeStdin();
    if (job_) TerminateJobObject(job_, 1);
    else if (process_ && running_) TerminateProcess(process_, 1);
}

int runWait(const ProcessOptions& opt, std::string* out, std::string* err, DWORD timeoutMs) {
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        std::string out, err;
        bool exited = false;
        unsigned long code = 0;
    };
    auto st = std::make_shared<State>();
    ChildProcess proc;
    std::string startErr;
    bool ok = proc.start(
        opt,
        [st](const std::string& l) { std::lock_guard<std::mutex> g(st->mu); st->out += l + "\n"; },
        [st](const std::string& l) { std::lock_guard<std::mutex> g(st->mu); st->err += l + "\n"; },
        [st](unsigned long c) {
            std::lock_guard<std::mutex> g(st->mu);
            st->exited = true;
            st->code = c;
            st->cv.notify_all();
        },
        &startErr);
    if (!ok) {
        if (err) *err = startErr;
        return -1;
    }
    proc.closeStdin();
    bool timedOut;
    {
        std::unique_lock<std::mutex> lock(st->mu);
        timedOut = !st->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return st->exited; });
    }
    if (timedOut) proc.terminate();
    std::lock_guard<std::mutex> g(st->mu);
    if (out) *out = st->out;
    if (err) *err = st->err;
    return timedOut ? -2 : (int)st->code;
}

void runCapture(const ProcessOptions& opt, std::function<void(int, std::string, std::string)> done,
                DWORD timeoutMs) {
    std::thread([opt, done = std::move(done), timeoutMs] {
        struct State {
            std::mutex mu;
            std::condition_variable cv;
            std::string out, err;
            bool exited = false;
            unsigned long code = 0;
        };
        auto st = std::make_shared<State>();
        auto proc = std::make_unique<ChildProcess>();
        std::string startErr;
        bool ok = proc->start(
            opt,
            [st](const std::string& l) { std::lock_guard<std::mutex> g(st->mu); st->out += l + "\n"; },
            [st](const std::string& l) { std::lock_guard<std::mutex> g(st->mu); st->err += l + "\n"; },
            [st](unsigned long c) {
                std::lock_guard<std::mutex> g(st->mu);
                st->exited = true;
                st->code = c;
                st->cv.notify_all();
            },
            &startErr);
        if (!ok) {
            done(-1, "", startErr);
            return;
        }
        proc->closeStdin();
        bool timedOut = false;
        {
            std::unique_lock<std::mutex> lock(st->mu);
            timedOut = !st->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return st->exited; });
        }
        if (timedOut) proc->terminate();
        proc.reset();  // joins reader threads
        std::lock_guard<std::mutex> g(st->mu);
        done(timedOut ? -2 : (int)st->code, st->out, st->err + (timedOut ? "\n逾時" : ""));
    }).detach();
}

// Deliberately separate from ChildProcess: terminating CCA must not terminate its update broker.
bool launchUpdateBroker(const std::wstring& exe, const std::wstring& plan, std::string* error) {
    ProcessOptions opt;
    opt.exe = exe;
    opt.args = {L"--cca-update-helper", plan};
    auto cmd = buildCommandLine(opt);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    DWORD flags = CREATE_NO_WINDOW | (inJob ? CREATE_BREAKAWAY_FROM_JOB : 0);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        if (error) *error = "無法啟動獨立更新程式，錯誤碼 " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

HANDLE launchUpdatedApplication(const std::wstring& exe, const std::wstring& ready, bool console, std::string* error) {
    ProcessOptions opt;
    opt.exe = exe;
    if (!ready.empty()) opt.args = {L"--cca-update-started", ready};
    auto cmd = buildCommandLine(opt);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // CLI relaunch gets its own terminal rather than inheriting the broker's hidden console.
    DWORD flags = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        if (error) *error = "無法重新開啟 CCA，錯誤碼 " + std::to_string(GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
