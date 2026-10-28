// Finds the two CLIs, checks that each is signed in with a paid subscription,
// and runs their login flows.
#include <windows.h>

#include <algorithm>

#include "app.hpp"
#include "util.hpp"

namespace {

bool claudePlanOk(const std::string& plan) {
    return plan == "pro" || plan == "max" || plan == "team" || plan == "enterprise";
}

bool codexPlanOk(const std::string& plan) {
    static const char* ok[] = {"plus", "pro", "prolite", "promax", "team", "business", "enterprise",
                               "edu", "edu_plus", "edu_pro", "ent26"};
    for (const char* p : ok) {
        if (plan == p) return true;
    }
    return util::startsWith(plan, "self_serve_business") || util::startsWith(plan, "enterprise_");
}

std::string planLabel(const std::string& agent, const std::string& plan) {
    if (plan.empty()) return "";
    std::string p = plan;
    if (!p.empty()) p[0] = (char)toupper((unsigned char)p[0]);
    return (agent == "claude" ? "Claude " : "ChatGPT ") + p;
}

Json statusJson(const AgentStatus& s, const char* agent) {
    return J({{"state", s.state},
              {"plan", s.plan},
              {"planLabel", planLabel(agent, s.plan)},
              {"email", s.email},
              {"version", s.version},
              {"message", s.message},
              {"exe", util::narrow(s.exe)}});
}

// Newest claude.exe bundled with the Claude desktop app, as a last resort.
std::wstring findDesktopBundledClaude() {
    std::wstring base = util::joinPath(util::getEnv(L"LOCALAPPDATA"), L"Claude-Data");
    std::wstring best;
    std::wstring bestVersion;
    WIN32_FIND_DATAW pkg;
    HANDLE h1 = FindFirstFileW(util::joinPath(base, L"*").c_str(), &pkg);
    if (h1 == INVALID_HANDLE_VALUE) return {};
    do {
        if (!(pkg.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pkg.cFileName[0] == L'.') continue;
        std::wstring cc = util::joinPath(util::joinPath(base, pkg.cFileName), L"claude-code");
        WIN32_FIND_DATAW ver;
        HANDLE h2 = FindFirstFileW(util::joinPath(cc, L"*").c_str(), &ver);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(ver.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || ver.cFileName[0] == L'.') continue;
            std::wstring vdir = util::joinPath(cc, ver.cFileName);
            WIN32_FIND_DATAW sub;
            HANDLE h3 = FindFirstFileW(util::joinPath(vdir, L"*").c_str(), &sub);
            if (h3 == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sub.cFileName[0] == L'.') continue;
                std::wstring exe = util::joinPath(util::joinPath(vdir, sub.cFileName), L"claude.exe");
                // Compare dotted versions numerically.
                auto key = [](const std::wstring& v) {
                    std::vector<int> parts;
                    size_t s = 0;
                    while (s <= v.size()) {
                        size_t e = v.find(L'.', s);
                        if (e == std::wstring::npos) e = v.size();
                        parts.push_back(_wtoi(v.substr(s, e - s).c_str()));
                        s = e + 1;
                    }
                    return parts;
                };
                if (util::fileExists(exe) && (best.empty() || key(ver.cFileName) > key(bestVersion))) {
                    best = exe;
                    bestVersion = ver.cFileName;
                }
            } while (FindNextFileW(h3, &sub));
            FindClose(h3);
        } while (FindNextFileW(h2, &ver));
        FindClose(h2);
    } while (FindNextFileW(h1, &pkg));
    FindClose(h1);
    return best;
}

// npm installs codex as a .cmd shim; the real binary lives under node_modules.
std::wstring codexBinaryNextTo(const std::wstring& shim) {
    size_t k = shim.find_last_of(L"\\/");
    if (k == std::wstring::npos) return {};
    std::wstring dir = shim.substr(0, k);
    const wchar_t* candidates[] = {
        L"node_modules\\@openai\\codex\\node_modules\\@openai\\codex-win32-x64\\vendor\\x86_64-pc-windows-msvc\\bin\\codex.exe",
        L"node_modules\\@openai\\codex\\node_modules\\@openai\\codex-win32-arm64\\vendor\\aarch64-pc-windows-msvc\\bin\\codex.exe",
        L"node_modules\\@openai\\codex\\vendor\\x86_64-pc-windows-msvc\\codex\\codex.exe",
    };
    for (const wchar_t* c : candidates) {
        std::wstring p = util::joinPath(dir, c);
        if (util::fileExists(p)) return p;
    }
    return {};
}

}  // namespace

std::wstring App::resolveClaudeExe() const {
    std::wstring custom = util::widen(settings_["claudePath"].str());
    if (!custom.empty() && util::fileExists(custom)) return custom;
    std::wstring p = util::findOnPath(L"claude");
    if (!p.empty()) return p;
    p = util::joinPath(util::homeDir(), L".local\\bin\\claude.exe");
    if (util::fileExists(p)) return p;
    p = util::joinPath(util::getEnv(L"APPDATA"), L"npm\\claude.cmd");
    if (util::fileExists(p)) return p;
    return findDesktopBundledClaude();
}

std::wstring App::resolveCodexExe() const {
    std::wstring custom = util::widen(settings_["codexPath"].str());
    if (!custom.empty() && util::fileExists(custom)) return custom;
    std::wstring p = util::findOnPath(L"codex");
    if (!p.empty()) {
        if (_wcsicmp(p.substr(p.size() - 4).c_str(), L".exe") == 0) return p;
        std::wstring bin = codexBinaryNextTo(p);
        return bin.empty() ? p : bin;
    }
    std::wstring shim = util::joinPath(util::getEnv(L"APPDATA"), L"npm\\codex.cmd");
    if (util::fileExists(shim)) {
        std::wstring bin = codexBinaryNextTo(shim);
        return bin.empty() ? shim : bin;
    }
    return {};
}

void App::sendAuth() {
    bool ok = claudeStatus_.state == "ok" || codexStatus_.state == "ok" || geminiStatus_.state == "ok";
    Json gemini = statusJson(geminiStatus_, "gemini");
    gemini["planLabel"] = geminiStatus_.plan;
    send(J({{"type", "auth"},
            {"claude", statusJson(claudeStatus_, "claude")},
            {"codex", statusJson(codexStatus_, "codex")},
            {"gemini", gemini},
            {"ok", ok}}));
}

void App::runAuthCheck(bool claude, bool codex) {
    if (claude) checkClaude();
    if (codex) checkCodex();
}

void App::checkClaude() {
    claudeStatus_ = AgentStatus();
    claudeStatus_.exe = resolveClaudeExe();
    if (claudeStatus_.exe.empty()) {
        claudeStatus_.state = "missing";
        claudeStatus_.message = "找不到 Claude Code CLI。";
        sendAuth();
        return;
    }
    sendAuth();
    ProcessOptions opt;
    opt.exe = claudeStatus_.exe;
    opt.args = {L"auth", L"status", L"--json"};
    opt.cwd = util::homeDir();
    opt.dropEnv = ClaudeSession::envToDrop();
    std::wstring exe = claudeStatus_.exe;
    runCapture(opt, [this, exe](int code, std::string out, std::string err) {
        postToUi([this, exe, code, out, err] {
            if (claudeStatus_.exe != exe) return;  // a newer check replaced this one
            Json j = Json::parse(out.substr(std::min(out.size(), out.find('{'))));
            if (!j.isObject()) {
                claudeStatus_.state = "error";
                claudeStatus_.message = "無法讀取 Claude 登入狀態" + (err.empty() ? "" : "：" + util::tailUtf8(util::trim(err), 300));
            } else if (!j["loggedIn"].boolean()) {
                claudeStatus_.state = "loggedOut";
                claudeStatus_.message = "Claude Code 尚未登入。";
            } else if (j["authMethod"].str() != "claude.ai") {
                claudeStatus_.state = "wrongMethod";
                claudeStatus_.message = "目前是用 " + j["authMethod"].str() + " 登入。CCA 需要用 Claude 訂閱帳號（claude.ai）登入。";
            } else {
                claudeStatus_.plan = j["subscriptionType"].str();
                claudeStatus_.email = j["email"].str();
                if (claudePlanOk(claudeStatus_.plan)) {
                    claudeStatus_.state = "ok";
                } else {
                    claudeStatus_.state = "noPlan";
                    claudeStatus_.message = "這個帳號的方案是「" + (claudeStatus_.plan.empty() ? std::string("免費") : claudeStatus_.plan) +
                                            "」。CCA 需要 Claude Pro 或更高的方案。";
                }
            }
            (void)code;
            sendAuth();
            if (claudeStatus_.state == "ok" && claudeModels_.size() == 0) probeClaudeModels();
        });
    }, 30000);

    ProcessOptions vopt = opt;
    vopt.args = {L"--version"};
    runCapture(vopt, [this, exe](int, std::string out, std::string) {
        postToUi([this, exe, out] {
            if (claudeStatus_.exe != exe) return;
            claudeStatus_.version = util::trim(util::replaceAll(out, "(Claude Code)", ""));
            sendAuth();
        });
    }, 30000);
}

void App::probeClaudeModels() {
    if (claudeProbe_) return;
    claudeProbe_ = std::make_shared<ClaudeSession>(claudeStatus_.exe, [this](const AgentEvent& ev) {
        if (ev.kind == AgentEvent::Kind::SessionInfo && ev.data["models"].isArray()) {
            if (ev.data["models"].size() > 0) {
                claudeModels_ = ev.data["models"];
                sendModels();
            }
            // Never destroy a session from inside its own callback.
            postToUi([this] {
                if (claudeProbe_) claudeProbe_->shutdown();
                claudeProbe_.reset();
            });
        } else if (ev.kind == AgentEvent::Kind::Exited) {
            postToUi([this] { claudeProbe_.reset(); });
        }
    });
    AgentConfig cfg;
    cfg.cwd = util::homeDir();
    cfg.permission = "plan";
    claudeProbe_->configure(cfg);
    std::string err;
    if (!claudeProbe_->start(&err)) claudeProbe_.reset();
}

void App::checkCodex() {
    std::wstring exe = resolveCodexExe();
    codexStatus_ = AgentStatus();
    codexStatus_.exe = exe;
    if (exe.empty()) {
        codexStatus_.state = "missing";
        codexStatus_.message = "找不到 Codex CLI。";
        sendAuth();
        return;
    }
    if (codexServer_ && codexServer_->exe() != exe) {
        codexServer_->stop();
        codexServer_.reset();
    }
    sendAuth();
    ensureCodexServer();
    std::string err;
    if (!codexServer_->running() && !codexServer_->start(&err)) {
        codexStatus_.state = "error";
        codexStatus_.message = err;
        sendAuth();
        return;
    }
    codexServer_->request("account/read", J({{"refreshToken", false}}), [this, exe](const Json& r, const Json& e) {
        if (codexStatus_.exe != exe) return;
        if (!e.isNull()) {
            codexStatus_.state = "error";
            codexStatus_.message = "無法讀取 ChatGPT 帳號：" + e["message"].str();
        } else if (r["account"].isNull()) {
            codexStatus_.state = "loggedOut";
            codexStatus_.message = "Codex 尚未登入 ChatGPT 帳號。";
        } else if (r["account"]["type"].str() != "chatgpt") {
            codexStatus_.state = "wrongMethod";
            codexStatus_.message = "Codex 目前是用 API 金鑰登入。CCA 需要用 ChatGPT 訂閱帳號登入。";
        } else {
            codexStatus_.plan = r["account"]["planType"].str();
            codexStatus_.email = r["account"]["email"].str();
            if (codexPlanOk(codexStatus_.plan)) {
                codexStatus_.state = "ok";
            } else {
                codexStatus_.state = "noPlan";
                codexStatus_.message = "這個帳號的方案是「" + codexStatus_.plan + "」。CCA 需要 ChatGPT Plus 或更高的方案。";
            }
        }
        sendAuth();
        if (codexStatus_.state == "ok") refreshCodexAccount();
    });

    ProcessOptions vopt;
    vopt.exe = exe;
    vopt.args = {L"--version"};
    runCapture(vopt, [this, exe](int, std::string out, std::string) {
        postToUi([this, exe, out] {
            if (codexStatus_.exe != exe) return;
            codexStatus_.version = util::trim(util::replaceAll(out, "codex-cli", ""));
            sendAuth();
        });
    }, 30000);
}

void App::refreshCodexAccount() {
    if (!codexServer_ || !codexServer_->running()) return;
    codexServer_->request("account/rateLimits/read", Json::object(), [this](const Json& r, const Json& e) {
        if (!e.isNull()) return;
        usage_["codex"]["limits"] = r["rateLimits"];
        sendUsage();
    });
    codexServer_->request("model/list", Json::object(), [this](const Json& r, const Json& e) {
        if (!e.isNull()) return;
        Json models = Json::array();
        for (const auto& m : r["data"].items()) {
            if (m["hidden"].boolean()) continue;
            Json efforts = Json::array();
            Json effortInfo = Json::object();
            for (const auto& eff : m["supportedReasoningEfforts"].items()) {
                efforts.push(eff["reasoningEffort"].str());
                effortInfo[eff["reasoningEffort"].str()] = eff["description"].str();
            }
            models.push(J({{"value", m["id"].str()},
                           {"displayName", m["displayName"].str()},
                           {"description", m["description"].str()},
                           {"isDefault", m["isDefault"].boolean()},
                           {"defaultEffort", m["defaultReasoningEffort"].str()},
                           {"supportedEffortLevels", efforts},
                           {"effortInfo", effortInfo}}));
        }
        codexModels_ = models;
        sendModels();
    });
}

void App::loginClaude() {
    if (claudeStatus_.exe.empty()) claudeStatus_.exe = resolveClaudeExe();
    if (claudeStatus_.exe.empty()) {
        toast("error", "找不到 Claude Code CLI，請先安裝");
        return;
    }
    if (claudeLogin_ && claudeLogin_->running()) return;
    ProcessOptions opt;
    opt.exe = claudeStatus_.exe;
    opt.args = {L"auth", L"login", L"--claudeai"};  // subscription login, never API billing
    opt.cwd = util::homeDir();
    opt.dropEnv = ClaudeSession::envToDrop();
    opt.newConsole = true;
    claudeLogin_ = std::make_unique<ChildProcess>();
    std::string err;
    bool ok = claudeLogin_->start(opt, nullptr, nullptr, [this](unsigned long) {
        postToUi([this] { runAuthCheck(true, false); });
    }, &err);
    if (!ok) {
        toast("error", err);
        claudeLogin_.reset();
        return;
    }
    claudeStatus_.state = "loggingIn";
    claudeStatus_.message = "請在跳出的視窗和瀏覽器中完成 Claude 登入。";
    sendAuth();
}

void App::loginCodex() {
    ensureCodexServer();
    std::string err;
    if (!codexServer_->running() && !codexServer_->start(&err)) {
        toast("error", err);
        return;
    }
    codexServer_->request("account/login/start", J({{"type", "chatgpt"}}), [this](const Json& r, const Json& e) {
        if (!e.isNull()) {
            toast("error", "無法開始 ChatGPT 登入：" + e["message"].str());
            return;
        }
        codexLoginId_ = r["loginId"].str();
        std::string url = r["authUrl"].str();
        if (!url.empty() && openExternal) openExternal(util::widen(url));
        codexStatus_.state = "loggingIn";
        codexStatus_.message = "請在瀏覽器完成 ChatGPT 登入。";
        sendAuth();
    });
}
