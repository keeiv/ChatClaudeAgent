// Account login stays in the official CLI; only sanitized plan and quota metadata reaches CCA.
#include "app.hpp"

#include "util.hpp"

std::wstring App::resolveGeminiExe() const {
    std::wstring custom = util::widen(settings_["geminiPath"].str());
    if (!custom.empty() && util::fileExists(custom)) return custom;
    std::wstring exe = util::findOnPath(L"gemini");
    if (!exe.empty()) return exe;
    exe = util::joinPath(util::getEnv(L"APPDATA"), L"npm\\gemini.cmd");
    return util::fileExists(exe) ? exe : L"";
}

void App::checkGemini() {
    if (geminiChecking_) return;
    lastGeminiUsage_ = util::nowMs();
    geminiStatus_ = AgentStatus();
    geminiStatus_.exe = resolveGeminiExe();
    if (geminiStatus_.exe.empty()) {
        geminiStatus_.state = "missing";
        geminiStatus_.message = "找不到 Gemini CLI；請安裝官方 @google/gemini-cli。";
        sendAuth();
        return;
    }
    ProcessOptions opt;
    opt.exe = util::findOnPath(L"node");
    if (opt.exe.empty()) {
        for (const auto& folder : {util::getEnv(L"ProgramFiles"), util::getEnv(L"LOCALAPPDATA")}) {
            auto node = util::joinPath(folder, L"nodejs\\node.exe");
            if (!folder.empty() && util::fileExists(node)) {
                opt.exe = node;
                break;
            }
        }
    }
    auto script = runtimeScript(L"GEMINI_ACCOUNT_JS", L"gemini_account.cjs");
    if (opt.exe.empty() || !util::fileExists(script)) {
        geminiStatus_.state = "error";
        geminiStatus_.message = "Gemini 帳號檢查需要 Node.js 與隨附的帳號查詢工具。";
        sendAuth();
        return;
    }
    opt.args = {script, geminiStatus_.exe};
    opt.cwd = util::appDataDir();
    opt.dropEnv = GeminiSession::envToDrop();
    geminiChecking_ = true;
    unsigned generation = ++geminiGeneration_;
    std::weak_ptr<int> life = lifetime_;
    sendAuth();
    runCapture(
        opt,
        [this, life, generation](int code, std::string out, std::string) {
            postToUi([this, life, generation, code, out] {
                if (life.expired() || generation != geminiGeneration_) return;
                geminiChecking_ = false;
                Json result;
                for (const auto& line : util::splitLines(out))
                    if (util::startsWith(line, "CCA_GEMINI_STATUS:")) result = Json::parse(line.substr(18));
                geminiStatus_.state = code == 0 ? result["state"].str("error") : "error";
                geminiStatus_.plan = result["planLabel"].str();
                geminiStatus_.email = result["email"].str();
                geminiStatus_.version = result["version"].str();
                geminiStatus_.message = result["message"].str();
                if (geminiStatus_.state == "error" && geminiStatus_.message.empty())
                    geminiStatus_.message = "無法取得 Gemini 帳號；請重新登入或檢查官方 CLI 版本。";
                usage_["gemini"]["quota"] =
                    J({{"ok", geminiStatus_.state == "ok" && result["quotaError"].str().empty()},
                       {"checkedAt", util::nowMs()},
                       {"buckets", result["buckets"]},
                       {"error", result["quotaError"].str(geminiStatus_.message)}});
                sendAuth();
                sendUsage();
            });
        },
        30000);
}

void App::loginGemini() {
    if (geminiLogin_ && geminiLogin_->running()) return;
    ++geminiGeneration_;
    geminiChecking_ = false;
    ProcessOptions opt;
    opt.exe = resolveGeminiExe();
    opt.cwd = util::appDataDir();
    opt.newConsole = true;
    opt.dropEnv = GeminiSession::envToDrop();
    if (opt.exe.empty()) {
        checkGemini();
        return;
    }
    // Interactive authentication is deliberately visible and never sends a model prompt.
    geminiLogin_ = std::make_unique<ChildProcess>();
    std::weak_ptr<int> life = lifetime_;
    std::string error;
    if (!geminiLogin_->start(
            opt, {}, {},
            [this, life](unsigned long) {
                postToUi([this, life] {
                    if (!life.expired()) checkGemini();
                });
            },
            &error)) {
        toast("error", error);
        return;
    }
    geminiStatus_.state = "loggingIn";
    geminiStatus_.message = "請在 Gemini CLI 選擇 Login with Google；完成後可直接按重新檢查，無須先關閉登入視窗。";
    sendAuth();
}
