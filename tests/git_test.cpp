// Version-control pane and quota-aware resume against real Git: a bare remote, two clones, real commits and pulls.
// Nothing touches the user's data, projects or Git settings; every path lives under a fresh temporary folder.
#include <windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "app.hpp"
#include "process.hpp"
#include "util.hpp"

#pragma comment(lib, "ws2_32.lib")

static std::mutex mu;
static std::deque<std::function<void()>> queue;
void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mu);
    queue.push_back(std::move(fn));
}
static void pump() {
    std::deque<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lock(mu);
        q.swap(queue);
    }
    for (auto& fn : q) fn();
}
static int failures = 0, checks = 0;
static void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

static std::string git(const std::wstring& cwd, std::vector<std::wstring> args) {
    ProcessOptions opt;
    opt.exe = util::findOnPath(L"git");
    opt.args = std::move(args);
    opt.cwd = cwd;
    opt.setEnv = {{L"GIT_TERMINAL_PROMPT", L"0"}};
    std::string out, err;
    runWait(opt, &out, &err, 30000);
    return util::trim(out);
}
static void identity(const std::wstring& repo) {
    git(repo, {L"config", L"user.name", L"CCA Test"});
    git(repo, {L"config", L"user.email", L"test@example.invalid"});
    git(repo, {L"config", L"commit.gpgsign", L"false"});
}

// Minimal HTTP remote: 401 with a Basic challenge until "cca:secret" is sent, then an empty dumb-protocol ref list.
struct AuthRemote {
    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::atomic<int> authorized{0};
    std::thread worker;
    AuthRemote() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener, (sockaddr*)&addr, sizeof addr);
        int len = sizeof addr;
        getsockname(listener, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        listen(listener, 8);
        worker = std::thread([this] {
            for (;;) {
                SOCKET s = accept(listener, nullptr, nullptr);
                if (s == INVALID_SOCKET) return;
                std::string request;
                char buf[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const int n = recv(s, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    request.append(buf, n);
                }
                const bool ok = request.find("Authorization: Basic Y2NhOnNlY3JldA==") != std::string::npos;
                if (ok) ++authorized;
                const std::string reply =
                    ok ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                       : "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"cca\"\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n";
                send(s, reply.data(), (int)reply.size(), 0);
                shutdown(s, SD_SEND);
                closesocket(s);
            }
        });
    }
    ~AuthRemote() {
        closesocket(listener);
        worker.join();
    }
};

struct GitTestAccess {
    static void run(const std::wstring& root) {
        const auto remote = util::joinPath(root, L"remote.git"), work = util::joinPath(root, L"work"),
                   other = util::joinPath(root, L"other");
        git(root, {L"init", L"--bare", L"-b", L"main", remote});
        git(root, {L"clone", remote, work});
        identity(work);
        git(work, {L"checkout", L"-b", L"main"});
        util::writeFileAtomic(util::joinPath(work, L"README.md"), "hello\n");
        git(work, {L"add", L"README.md"});
        git(work, {L"commit", L"-m", L"Initial"});
        git(work, {L"push", L"-u", L"origin", L"main"});

        std::vector<Json> sent;
        App app([&](const std::string& s) { sent.push_back(Json::parse(s)); });
        app.uiReady_ = true;
        Conversation* c = nullptr;
        for (auto& p : app.convs_)
            if (p->kind == "group") c = p.get();
        c->cwd = work;
        auto await = [&](const std::string& type, std::function<bool(const Json&)> ok = nullptr) {
            for (int i = 0; i < 1500; ++i) {
                pump();
                for (auto it = sent.rbegin(); it != sent.rend(); ++it)
                    if ((*it)["type"].str() == type && (!ok || ok(*it))) return *it;
                Sleep(10);
            }
            return Json();
        };
        auto state = [&] {
            sent.clear();
            app.onUiMessage(J({{"type", "git.open"}, {"id", c->id}}).dump());
            // The first answer says "checking"; waiting for the second keeps it from arriving in a later step.
            return await("git.state", [](const Json& m) { return m["auth"]["state"].str() != "checking"; });
        };
        auto act = [&](Json m) {
            sent.clear();
            m["id"] = c->id;
            app.onUiMessage(m.dump());
            return await("git.result");
        };

        Json s = state();
        check(s["ok"].boolean() && s["branch"].str() == "main" && s["upstream"].str() == "origin/main",
              "pane reads branch and tracked remote");
        check(s["files"].size() == 0 && s["commits"].size() == 1 && s["commits"].at(0)["subject"].str() == "Initial",
              "clean tree with the initial commit");

        util::writeFileAtomic(util::joinPath(work, L"README.md"), "hello\nworld\n");
        util::writeFileAtomic(util::joinPath(work, L"feature.txt"), "new\n");
        util::writeFileAtomic(util::joinPath(work, L".env"), "TOKEN=abc\n");
        util::writeFileAtomic(util::joinPath(work, L"config.js"),
                              "const key = \"sk-abcdefghijklmnopqrstuvwxyz0123456789\";\n");
        s = state();
        auto file = [&](const std::string& path) {
            for (const auto& f : s["files"].items())
                if (f["path"].str() == path) return f;
            return Json();
        };
        check(file("README.md")["tone"].str() == "changed" && file("feature.txt")["tone"].str() == "new",
              "yellow for a change, green for a new file");
        check(file(".env")["locked"].boolean() && file(".env")["tone"].str() == "risk",
              "default never-commit rule locks .env");

        Json r = act(J({{"type", "git.commit"},
                        {"paths", Json(Json::Array{"README.md", "feature.txt"})},
                        {"message", "Add feature"},
                        {"task", false}}));
        check(r["result"]["ok"].boolean(), "selected files commit");
        check(git(work, {L"log", L"-1", L"--format=%s"}) == "Add feature" &&
                  git(work, {L"show", L"--name-only", L"--format=", L"HEAD"}) == "README.md\nfeature.txt",
              "only the selected files enter the commit");
        r = act(J({{"type", "git.commit"}, {"paths", Json(Json::Array{".env"})}, {"message", "leak"}}));
        check(!r["result"]["ok"].boolean() && git(work, {L"log", L"-1", L"--format=%s"}) == "Add feature",
              "a locked file is refused by the host, not only the page");
        r = act(J({{"type", "git.commit"}, {"paths", Json(Json::Array{"config.js"})}, {"message", "config"}}));
        check(!r["result"]["ok"].boolean() && r["result"]["secrets"].size() == 1 &&
                  git(work, {L"diff", L"--cached", L"--name-only"}).empty(),
              "the secret scan stops the commit and leaves the index clean");
        check(r.dump().find("abcdefghijklmnop") == std::string::npos, "the finding never repeats the secret");
        r = act(J({{"type", "git.commit"},
                   {"paths", Json(Json::Array{"config.js"})},
                   {"message", "Add config"},
                   {"allowSecrets", true},
                   {"push", true}}));
        check(r["result"]["ok"].boolean() && r["result"]["pushed"].boolean(), "confirmed commit and push");
        check(git(work, {L"rev-parse", L"HEAD"}) == git(remote, {L"rev-parse", L"main"}),
              "remote has the pushed commit");

        // A teammate pushes; automatic fetch with fast-forward pull brings it in while the agents are idle.
        git(root, {L"clone", remote, other});
        identity(other);
        util::writeFileAtomic(util::joinPath(other, L"team.txt"), "from a teammate\n");
        git(other, {L"add", L"team.txt"});
        git(other, {L"commit", L"-m", L"Teammate change"});
        git(other, {L"push"});
        app.onUiMessage(
            J({{"type", "git.auto"}, {"id", c->id}, {"fetch", true}, {"pull", true}, {"minutes", 1}}).dump());
        check(app.gitSettings(c->id)["pull"].boolean(), "auto sync settings persist");
        sent.clear();
        app.gitCheckAt_ = 0;
        app.gitFetchAt_.clear();
        app.tickGit(util::nowMs());
        Json synced = await("git.state", [](const Json& m) { return m["last"]["op"].str() == "auto"; });
        check(synced["last"]["pulled"].boolean() && util::fileExists(util::joinPath(work, L"team.txt")),
              "automatic sync fast-forwards a clean branch");
        app.onUiMessage(
            J({{"type", "git.auto"}, {"id", c->id}, {"fetch", false}, {"pull", false}, {"minutes", 1}}).dump());

        // Sign-in: a local HTTP remote answers 401 until Basic credentials arrive. The global Git config is an
        // empty temporary file, so no credential helper of this computer takes part.
        AuthRemote server;
        const auto signin = util::joinPath(root, L"signin");
        git(root, {L"init", L"-b", L"main", signin});
        git(signin, {L"remote", L"add", L"origin", L"http://127.0.0.1:" + std::to_wstring(server.port) + L"/r.git"});
        c->cwd = signin;
        // Answers still on their way for the first repository are skipped.
        auto settled = [](const Json& m) {
            return m["remote"]["host"].str() == "127.0.0.1" && m["auth"]["state"].str() != "checking";
        };
        sent.clear();
        app.onUiMessage(J({{"type", "git.open"}, {"id", c->id}}).dump());
        bool sawChecking = false;
        s = await("git.state", settled);
        for (const auto& m : sent) sawChecking |= m["auth"]["state"].str() == "checking";
        check(sawChecking && s["auth"]["state"].str() == "required" && s["auth"]["host"].str() == "127.0.0.1",
              "opening the pane checks the remote and asks for a sign-in");
        check(s["identity"]["name"].str().empty() && s["auth"]["helper"].str().empty(),
              "missing author and credential helper are reported for the sign-in page");
        r = act(J({{"type", "git.fetch"}}));
        check(!r["result"]["ok"].boolean() && r["result"]["authProblem"].str() == "auth" &&
                  r["result"]["error"].str() == "遠端倉庫需要登入",
              "a fetch that needs a sign-in is named as such");
        sent.clear();
        app.onUiMessage(J({{"type", "git.identity"},
                           {"id", c->id},
                           {"name", "CCA Tester"},
                           {"email", "tester@example.invalid"},
                           {"global", false}})
                            .dump());
        s = await("git.state", [](const Json& m) { return m["last"]["op"].str() == "identity"; });
        check(s["identity"]["name"].str() == "CCA Tester" &&
                  git(signin, {L"config", L"--local", L"user.email"}) == "tester@example.invalid",
              "the author is saved to the repository");
        sent.clear();
        app.onUiMessage(J({{"type", "git.identity"}, {"id", c->id}, {"name", "x"}, {"email", "no-at"}}).dump());
        for (int i = 0; i < 30; ++i) pump();
        check(git(signin, {L"config", L"--local", L"user.name"}) == "CCA Tester", "an invalid e-mail is refused");
        // Stands in for Git Credential Manager: the helper hands over the account the server expects.
        git(signin,
            {L"config", L"--global", L"credential.helper", L"!f() { echo username=cca; echo password=secret; }; f"});
        r = act(J({{"type", "git.login"}, {"setupHelper", false}}));
        check(r["result"]["ok"].boolean() && r["result"]["host"].str() == "127.0.0.1", "sign-in succeeds");
        s = await("git.state", [](const Json& m) { return m["last"]["op"].str() == "login"; });
        check(s["auth"]["state"].str() == "ok" && server.authorized > 0, "the pane leaves the sign-in page");
        check(r.dump().find("secret") == std::string::npos && s.dump().find("secret") == std::string::npos,
              "the password never reaches the page");
        c->cwd = work;

        // Quota-aware resume: the schedule follows the earliest known reset plus the delay, inside the allowed hours.
        app.settings_["autoResume"]["enabled"] = true;
        app.settings_["autoResume"]["delayMinutes"] = 5;
        app.settings_["autoResume"]["window"]["enabled"] = false;
        app.settings_["claudeQuota"]["blocked"] = false;
        const long long now = util::nowMs();
        app.quotaUntil_["claude"] = now + 30 * 60000;
        app.quotaUntil_["codex"] = now + 90 * 60000;
        app.geminiStatus_.state = "retired";
        long long at = app.autoResumeAt(*c);
        check(at >= now + 34 * 60000 && at <= now + 36 * 60000, "earliest reset plus delay");
        app.settings_["autoResume"]["enabled"] = false;
        check(app.autoResumeAt(*c) == 0, "disabled resume schedules nothing");
        app.settings_["autoResume"]["enabled"] = true;
        c->task = newTask(c->id, "ORIGINAL_TASK", now);
        c->task["status"] = "interrupted";
        c->task["quota_stop"] = J({{"at", now}, {"resumeAt", now - 1}, {"next", "claude:write"}});
        saveTask(c->task);
        app.claudeStatus_.state = app.codexStatus_.state = "ok";
        app.quotaUntil_.clear();
        app.autoResumeCheckAt_ = 0;
        app.settings_["guard"]["enabled"] = false;
        app.settings_["claudePath"] = "C:\\missing\\cli.exe";
        app.settings_["codexPath"] = "C:\\missing\\cli.exe";
        app.tickAutoResume(util::nowMs());
        bool announced = false;
        for (const auto& m : c->messages)
            announced |= m["text"].str().find("自動接續任務（第 1 次）") != std::string::npos;
        check(announced && c->task["auto_resume_attempts"].i64() == 1 && c->group.active,
              "a due schedule resumes the task once and counts the attempt");
        app.onUiMessage(J({{"type", "conv.stop"}, {"id", c->id}}).dump());
        for (int i = 0; i < 200; ++i) pump();
        app.shutdown();
        pump();
    }
};

int wmain() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const auto root = util::joinPath(tmp, util::widen("cca-git-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"data").c_str());
    // Git reads neither the user's ~/.gitconfig nor the installation's credential helper during the test.
    SetEnvironmentVariableW(L"GIT_CONFIG_GLOBAL", util::joinPath(root, L"gitconfig").c_str());
    SetEnvironmentVariableW(L"GIT_CONFIG_NOSYSTEM", L"1");
    if (util::findOnPath(L"git").empty()) {
        std::printf("Git not found; git-test was not run\n");
        return 1;
    }
    GitTestAccess::run(root);
    std::printf("%d git checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
