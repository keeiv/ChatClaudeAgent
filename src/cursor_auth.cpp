// Cursor account, plan and model catalogue come from the official CLI's read-only commands; no prompt is sent.
#include "app.hpp"

#include <thread>

#include "util.hpp"

namespace {

// The CLI prints one JSON document; anything around it (an update notice) is ignored.
Json parseDocument(const std::string& out) {
    size_t open = out.find('{'), close = out.rfind('}');
    return open == std::string::npos || close == std::string::npos || close < open
               ? Json()
               : Json::parse(out.substr(open, close - open + 1));
}

ProcessOptions cursorCommand(const CursorLaunch& launch, std::vector<std::wstring> args) {
    ProcessOptions opt;
    opt.exe = launch.node;
    opt.args = {launch.script};
    for (auto& a : args) opt.args.push_back(std::move(a));
    opt.cwd = util::appDataDir();
    opt.dropEnv = CursorSession::envToDrop();
    opt.setEnv = {{L"NO_COLOR", L"1"}, {L"CURSOR_INVOKED_AS", L"cursor-agent"}};
    return opt;
}

}  // namespace

CursorLaunch App::resolveCursor() const {
    if (cursorLaunchOverride_.ok()) return cursorLaunchOverride_;
    return CursorSession::resolve(util::widen(settings_["cursorPath"].str()));
}

void App::checkCursor() {
    if (cursorChecking_) return;
    const CursorLaunch launch = resolveCursor();
    if (!launch.ok()) {
        cursorStatus_ = AgentStatus();
        cursorStatus_.state = "missing";
        cursorStatus_.message = "找不到 Cursor CLI；請先安裝官方 Cursor Agent CLI，或在設定指定它的安裝資料夾。";
        cursorModels_ = Json::array();
        sendAuth();
        sendModels();
        return;
    }
    // A re-check keeps the last confirmed account, so a group that is running does not pass Cursor over meanwhile.
    if (cursorStatus_.state != "ok") cursorStatus_ = AgentStatus();
    cursorStatus_.exe = launch.script;
    if (cursorStatus_.version.empty()) cursorStatus_.version = util::narrow(launch.version);
    cursorChecking_ = true;
    const unsigned generation = ++cursorGeneration_;
    std::weak_ptr<int> life = lifetime_;
    sendAuth();
    std::thread([this, life, generation, status = cursorCommand(launch, {L"status", L"--format", L"json"}),
                 about = cursorCommand(launch, {L"about", L"--format", L"json"}),
                 models = cursorCommand(launch, {L"models"})] {
        std::string statusOut, aboutOut, modelsOut, ignored;
        int statusCode = runWait(status, &statusOut, &ignored, 30000);
        Json account = statusCode == 0 ? parseDocument(statusOut) : Json();
        int aboutCode = -1, modelsCode = -1;
        if (account["isAuthenticated"].boolean()) {
            aboutCode = runWait(about, &aboutOut, &ignored, 30000);
            modelsCode = runWait(models, &modelsOut, &ignored, 60000);
        }
        postToUi([this, life, generation, account, statusCode, aboutCode, aboutOut, modelsCode, modelsOut] {
            if (life.expired() || generation != cursorGeneration_) return;
            cursorChecking_ = false;
            Json info = aboutCode == 0 ? parseDocument(aboutOut) : Json();
            const std::wstring exe = cursorStatus_.exe;
            const std::string version = cursorStatus_.version;
            cursorStatus_ = AgentStatus();
            cursorStatus_.exe = exe;
            cursorStatus_.version = version;
            if (statusCode != 0 || !account.isObject()) {
                cursorStatus_.state = "error";
                cursorStatus_.message = "無法取得 Cursor 帳號狀態；請確認 Cursor CLI 可以執行，或重新安裝。";
            } else if (!account["isAuthenticated"].boolean()) {
                cursorStatus_.state = "loggedOut";
                cursorStatus_.message = "Cursor 尚未登入；請按登入，在瀏覽器完成 Cursor 帳號登入。";
            } else {
                cursorStatus_.state = "ok";
                cursorStatus_.email = account["userInfo"]["email"].str();
                cursorStatus_.plan = info["subscriptionTier"].str();
                if (!info["cliVersion"].str().empty()) cursorStatus_.version = info["cliVersion"].str();
            }
            cursorModels_ =
                cursorStatus_.state == "ok" && modelsCode == 0 ? CursorSession::parseModels(modelsOut) : Json::array();
            if (cursorStatus_.state == "ok" && cursorModels_.empty())
                cursorStatus_.message = "Cursor 沒有回報可用的模型清單；請稍後重新檢查。";
            // Cursor reports no remaining quota, so CCA shows it as unknown and never infers exhaustion from it.
            usage_["cursor"]["quota"] =
                J({{"ok", false}, {"checkedAt", util::nowMs()}, {"error", "Cursor 沒有提供額度資料"}});
            sendAuth();
            sendModels();
            sendUsage();
        });
    }).detach();
}

void App::loginCursor() {
    if (cursorLogin_ && cursorLogin_->running()) return;
    ++cursorGeneration_;
    cursorChecking_ = false;
    const CursorLaunch launch = resolveCursor();
    if (!launch.ok()) {
        checkCursor();
        return;
    }
    ProcessOptions opt = cursorCommand(launch, {L"login"});
    opt.newConsole = true;
    // The official login opens the browser itself; CCA never sees the account's credentials.
    cursorLogin_ = std::make_unique<ChildProcess>();
    std::weak_ptr<int> life = lifetime_;
    std::string error;
    if (!cursorLogin_->start(
            opt, {}, {},
            [this, life](unsigned long) {
                postToUi([this, life] {
                    if (!life.expired()) checkCursor();
                });
            },
            &error)) {
        toast("error", error);
        return;
    }
    cursorStatus_.state = "loggingIn";
    cursorStatus_.message = "請在開啟的瀏覽器完成 Cursor 登入；登入視窗結束後 CCA 會重新檢查。";
    sendAuth();
}
