// Runs a console program inside a pseudoconsole (ConPTY), types a script into it and
// records everything the terminal receives, so the CLI can be checked without a person
// at the keyboard. tests/pty_view.html replays the recording in a terminal emulator.
//
//   build\pty_run.exe <cols> <rows> <out.vt> <script.txt> <program> [args...]
//
// Script lines:  wait <ms> | type <text> | key <enter|esc|up|down|left|right|tab|backspace|ctrl-c|ctrl-d|shift-enter>
//                until <text> [timeout ms] | snap <name> | resize <cols> <rows>
// "snap" writes the byte offset of the recording so far to <out.vt>.snaps (name=offset per line).
// "memory <name>" writes child-process memory metrics to <out.vt>.memory.jsonl.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_mu;
static std::string g_out;

static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: pty_run <cols> <rows> <out.vt> <script.txt> <program> [args...]\n");
        return 2;
    }
    SHORT cols = (SHORT)_wtoi(argv[1]), rows = (SHORT)_wtoi(argv[2]);
    std::wstring outPath = argv[3], scriptPath = argv[4];
    if (cols < 10 || rows < 5) { std::fprintf(stderr, "invalid console size\n"); return 2; }
    std::ifstream script(scriptPath, std::ios::binary);
    std::ofstream snaps(outPath + L".snaps", std::ios::binary);
    std::ofstream memory(outPath + L".memory.jsonl", std::ios::binary);
    std::ofstream sizes(outPath + L".sizes", std::ios::binary);
    if (!script || !snaps || !memory || !sizes) { std::fprintf(stderr, "cannot open script or output files\n"); return 2; }
    sizes << "0 " << cols << " " << rows << "\n";
    sizes.flush();
    std::wstring cmd;
    for (int i = 5; i < argc; ++i) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        cmd += argv[i];
        cmd += L'"';
    }

    HANDLE inRead, inWrite, outRead, outWrite;
    CreatePipe(&inRead, &inWrite, nullptr, 0);
    CreatePipe(&outRead, &outWrite, nullptr, 0);
    HPCON pc;
    if (FAILED(CreatePseudoConsole({cols, rows}, inRead, outWrite, 0, &pc))) {
        std::fprintf(stderr, "CreatePseudoConsole failed\n");
        return 1;
    }
    CloseHandle(inRead);
    CloseHandle(outWrite);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    // Our own stdio may be pipes; the child must not inherit them instead of the pseudoconsole.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrs(size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc, nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &si.StartupInfo, &pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        DeleteProcThreadAttributeList(si.lpAttributeList);
        ClosePseudoConsole(pc);
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return 1;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);

    std::thread reader([&] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof buf, &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> l(g_mu);
            g_out.append(buf, n);
        }
    });

    int failures = 0;
    auto send = [&](const std::string& s) {
        DWORD n = 0;
        if (!WriteFile(inWrite, s.data(), (DWORD)s.size(), &n, nullptr) || n != s.size()) ++failures;
    };
    std::string line;
    try {
    while (std::getline(script, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string op = line.substr(0, line.find(' '));
        std::string arg = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
        if (op == "wait") {
            Sleep(std::stoi(arg));
        } else if (op == "type") {
            send(arg);
        } else if (op == "key") {
            if (arg == "enter") send("\r");
            else if (arg == "esc") send("\x1b");
            else if (arg == "up") send("\x1b[A");
            else if (arg == "down") send("\x1b[B");
            else if (arg == "right") send("\x1b[C");
            else if (arg == "left") send("\x1b[D");
            else if (arg == "tab") send("\t");
            else if (arg == "backspace") send("\x7f");
            else if (arg == "ctrl-c") send("\x03");
            else if (arg == "ctrl-d") send("\x04");
            else if (arg == "ctrl-j") send("\x0a");
            else if (arg == "ctrl-u") send("\x15");
            else if (arg == "ctrl-l") send("\x0c");
            else if (arg == "shift-enter") send("\x1b[13;28;13;1;16;1_");  // win32-input-mode: Enter with Shift held
            else { std::fprintf(stderr, "unknown key: %s\n", arg.c_str()); ++failures; break; }
        } else if (op == "until") {
            std::string text = arg;
            int timeout = 20000;
            size_t sp = arg.rfind(' ');
            if (sp != std::string::npos && arg.find_first_not_of("0123456789", sp + 1) == std::string::npos) {
                text = arg.substr(0, sp);
                timeout = std::stoi(arg.substr(sp + 1));
            }
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            bool found = false;
            while (std::chrono::steady_clock::now() < end) {
                {
                    std::lock_guard<std::mutex> l(g_mu);
                    found = g_out.find(text) != std::string::npos;
                }
                if (found) break;
                Sleep(100);
            }
            std::printf("until \"%s\": %s\n", text.c_str(), found ? "found" : "TIMEOUT");
            if (!found) { ++failures; break; }
        } else if (op == "snap") {
            std::lock_guard<std::mutex> l(g_mu);
            snaps << arg << "=" << g_out.size() << "\n";
            snaps.flush();
        } else if (op == "resize") {
            std::istringstream in(arg);
            int c = 0, r = 0;
            in >> c >> r;
            { std::lock_guard<std::mutex> l(g_mu); sizes << g_out.size() << " " << c << " " << r << "\n"; sizes.flush(); }
            if (c < 10 || r < 5 || FAILED(ResizePseudoConsole(pc, {(SHORT)c, (SHORT)r}))) { ++failures; break; }
        } else if (op == "memory") {
            // The tree includes agent subprocesses, but excludes the terminal host.
            std::map<DWORD, DWORD> parents;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof entry;
            if (snapshot != INVALID_HANDLE_VALUE) {
                if (Process32FirstW(snapshot, &entry)) do { parents[entry.th32ProcessID] = entry.th32ParentProcessID; } while (Process32NextW(snapshot, &entry));
                CloseHandle(snapshot);
            }
            unsigned long long working = 0, priv = 0, treeWorking = 0, treePriv = 0;
            int count = 0;
            for (const auto& [pid, parent] : parents) {
                DWORD ancestor = pid;
                for (size_t n = 0; n < parents.size() && ancestor != pi.dwProcessId; ++n) {
                    auto at = parents.find(ancestor);
                    if (at == parents.end() || at->second == ancestor || at->second == 0) break;
                    ancestor = at->second;
                }
                if (ancestor != pi.dwProcessId) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof pm;
                if (process && GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
                    treeWorking += pm.WorkingSetSize; treePriv += pm.PrivateUsage; ++count;
                    if (pid == pi.dwProcessId) { working = pm.WorkingSetSize; priv = pm.PrivateUsage; }
                }
                if (process) CloseHandle(process);
            }
            if (!working || arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) { ++failures; break; }
            memory << "{\"name\":\"" << arg << "\",\"workingSetBytes\":" << working << ",\"privateBytes\":" << priv
                   << ",\"treeWorkingSetBytes\":" << treeWorking << ",\"treePrivateBytes\":" << treePriv << ",\"processes\":" << count << "}\n";
            memory.flush();
        } else { std::fprintf(stderr, "unknown operation: %s\n", op.c_str()); ++failures; break; }
    }
    } catch (const std::exception& e) { std::fprintf(stderr, "invalid script: %s\n", e.what()); ++failures; }
    if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT) {
        std::printf("program still running; terminating\n");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        ++failures;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    ClosePseudoConsole(pc);
    CloseHandle(inWrite);
    reader.join();
    CloseHandle(outRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::ofstream out(outPath, std::ios::binary);
    out << g_out;
    if (!out) ++failures;
    std::printf("exit code %lu, %zu bytes recorded\n", code, g_out.size());
    return code || failures ? 1 : 0;
}
