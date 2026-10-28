// Version-control pane: status, history, selective commit, push and remote sync for a conversation's folder.
// Every Git command runs off the UI thread without prompts; commits and pulls wait until the agents are idle.
#include "app.hpp"

#include <windows.h>

#include <algorithm>
#include <thread>

#include "gitflow.hpp"
#include "process.hpp"
#include "util.hpp"

namespace {

constexpr long long kAutoCheckMs = 15000;

struct GitRun {
    std::wstring cwd;
    // Only an explicit sign-in lets Git Credential Manager open its own window or the browser.
    bool interactive = false;
    // Returns the exit code; output and error are kept separately so error text never mixes into data.
    int operator()(const std::vector<std::wstring>& args, std::string* out, std::string* err,
                   DWORD timeout = 30000) const {
        ProcessOptions opt;
        opt.exe = util::findOnPath(L"git");
        if (opt.exe.empty()) {
            *err = "找不到 Git；請先安裝 Git for Windows";
            return -1;
        }
        opt.args = {L"-c", L"core.quotepath=false", L"-c", L"i18n.logOutputEncoding=UTF-8"};
        opt.args.insert(opt.args.end(), args.begin(), args.end());
        opt.cwd = cwd;
        // No credential or editor prompt may block a hidden process; Git reports the failure instead.
        opt.setEnv = {{L"GIT_TERMINAL_PROMPT", L"0"},
                      {L"GCM_INTERACTIVE", interactive ? L"always" : L"never"},
                      {L"GIT_EDITOR", L"true"},
                      {L"GIT_MERGE_AUTOEDIT", L"no"},
                      {L"GIT_LITERAL_PATHSPECS", L"1"}};
        opt.dropEnv = {L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_INDEX_FILE", L"GIT_COMMON_DIR"};
        out->clear();
        err->clear();
        return runWait(opt, out, err, timeout);
    }
};

// Credentials embedded in a remote URL are never shown.
std::string redactUrl(const std::string& url) {
    const auto scheme = url.find("://"), at = url.find('@');
    if (scheme != std::string::npos && at != std::string::npos && at > scheme)
        return url.substr(0, scheme + 3) + url.substr(at + 1);
    return url;
}

std::string firstLine(const std::string& text) {
    const auto lines = util::splitLines(util::trim(text));
    return lines.empty() ? "" : lines.back();
}

// Git error text can quote the remote URL; any "user:token@" inside it is removed before it reaches the page.
std::string redactText(std::string text) {
    for (size_t at = text.find("://"); at != std::string::npos; at = text.find("://", at + 3)) {
        const auto end = text.find_first_of("/ '\"\n", at + 3), mark = text.find('@', at + 3);
        if (mark != std::string::npos && (end == std::string::npos || mark < end)) text.erase(at + 3, mark - at - 2);
    }
    return text;
}

// Asks the remote for its branch list: the cheapest call that needs the same sign-in as fetch and push.
Json checkRemote(const std::wstring& root, const Json& remote, bool interactive) {
    GitRun repo{root, interactive};
    std::string out, err;
    Json auth = J({{"state", "ok"}, {"remote", remote["name"]}, {"host", remote["host"]}, {"ssh", remote["ssh"]}});
    const int code =
        repo({L"ls-remote", L"--heads", util::widen(remote["name"].str())}, &out, &err, interactive ? 300000 : 20000);
    if (code == 0) return auth;
    const auto problem = gitflow::authProblem(err);
    auth["state"] = problem == "auth" ? "required" : problem == "network" || code == -2 ? "offline" : "unknown";
    auth["detail"] = code == -2 ? (interactive ? "等待登入逾時" : "連線逾時") : redactText(firstLine(err));
    if (auth["state"].str() == "required" && !remote["ssh"].boolean()) {
        std::string helper;
        repo({L"config", L"--get-all", L"credential.helper"}, &helper, &err);
        const auto helpers = util::splitLines(util::trim(helper));
        auth["helper"] = helpers.empty() ? "" : helpers.back();
        auth["gcm"] = repo({L"credential-manager", L"--version"}, &out, &err, 15000) == 0;
    }
    return auth;
}

long long fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return -1;
    return ((long long)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
}

// Collects everything the pane shows in one pass; called on a worker thread.
Json collect(const std::wstring& cwd, const Json& rules) {
    GitRun git{cwd};
    std::string out, err;
    if (git({L"rev-parse", L"--show-toplevel"}, &out, &err))
        return J({{"ok", false},
                  {"error", err.empty() ? "這個資料夾不是 Git 倉庫" : "這個資料夾不是 Git 倉庫：" + firstLine(err)}});
    const auto root = util::widen(util::trim(out));
    GitRun repo{root};
    Json state = J({{"ok", true}, {"root", util::narrow(root)}});
    if (repo({L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"}, &out, &err))
        return J({{"ok", false}, {"error", "無法讀取 Git 狀態：" + firstLine(err)}});
    Json files = gitflow::parseStatus(out), sizes = Json::object();
    for (const auto& f : files.items()) {
        const auto size = fileSize(util::joinPath(root, util::widen(util::replaceAll(f["path"].str(), "/", "\\"))));
        if (size >= 0) sizes[f["path"].str()] = size;
    }
    state["files"] = gitflow::classify(files, rules, sizes);
    std::string branch;
    if (!repo({L"symbolic-ref", L"--short", L"HEAD"}, &out, &err)) branch = util::trim(out);
    else if (!repo({L"rev-parse", L"--short", L"HEAD"}, &out, &err)) branch = "（分離的 HEAD " + util::trim(out) + "）";
    state["branch"] = branch;
    state["upstream"] = "";
    state["ahead"] = 0;
    state["behind"] = 0;
    if (!repo({L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{u}"}, &out, &err)) {
        state["upstream"] = util::trim(out);
        if (!repo({L"rev-list", L"--left-right", L"--count", L"HEAD...@{u}"}, &out, &err)) {
            const auto parts = util::trim(out);
            const auto tab = parts.find_first_of(" \t");
            if (tab != std::string::npos) {
                state["ahead"] = std::stoll(parts.substr(0, tab));
                state["behind"] = std::stoll(util::trim(parts.substr(tab)));
            }
        }
    }
    state["remote"] = Json();
    if (!repo({L"remote"}, &out, &err) && !util::trim(out).empty()) {
        const auto name = util::splitLines(util::trim(out)).front();
        std::string url;
        if (!repo({L"remote", L"get-url", util::widen(name)}, &url, &err)) {
            const Json host = gitflow::remoteHost(util::trim(url));
            state["remote"] =
                J({{"name", name}, {"url", redactUrl(util::trim(url))}, {"host", host["host"]}, {"ssh", host["ssh"]}});
        }
    }
    // Without a name and e-mail Git refuses every commit, so the pane asks for them before the first one.
    std::string name, email;
    if (!repo({L"config", L"user.name"}, &out, &err)) name = util::trim(out);
    if (!repo({L"config", L"user.email"}, &out, &err)) email = util::trim(out);
    state["identity"] = J({{"name", name}, {"email", email}});
    state["commits"] = Json::array();
    if (!repo({L"log", L"--all", L"--topo-order", L"-n", L"150",
               L"--format=%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1f%(trailers:key=CCA-Task,valueonly,separator=%x2c)%x1e"},
              &out, &err, 30000))
        state["commits"] = gitflow::parseLog(out);
    return state;
}

}  // namespace

Json App::gitSettings(const std::string& id) const {
    Json a = settings_["git"]["auto"][id];
    return J({{"fetch", a["fetch"].boolean(false)},
              {"pull", a["pull"].boolean(false)},
              {"minutes", std::clamp<long long>(a["minutes"].i64(5), 1, 240)}});
}

void App::gitRefresh(Conversation& c, const std::string& op, const Json& result, bool checkAuth) {
    std::wstring cwd = c.cwd;
    std::string id = c.id;
    if (cwd.empty() || !util::dirExists(cwd)) {
        send(J({{"type", "git.state"}, {"id", id}, {"ok", false}, {"error", "請先選擇專案資料夾"}}));
        return;
    }
    Json rules = settings_["git"]["neverCommit"];
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, cwd, id, rules, op, result, checkAuth] {
        // auth is stored for the conversation when given; otherwise the last known sign-in state is shown.
        auto deliver = [&](const Json& state, const Json& auth) {
            postToUi([this, life, id, state, auth, op, result] {
                if (life.expired()) return;
                Conversation* c = find(id);
                if (!c) return;
                if (!auth.isNull()) {
                    gitAuth_[id] = auth;
                    gitAuth_[id]["root"] = state["root"];
                }
                // A conversation moved to another folder must not inherit the previous repository's sign-in.
                if (gitAuth_.count(id) && gitAuth_[id]["root"].str() != state["root"].str()) gitAuth_.erase(id);
                Json out = state;
                out["type"] = "git.state";
                out["id"] = id;
                out["auto"] = gitSettings(id);
                out["rules"] = settings_["git"]["neverCommit"];
                out["task"] = c->task["task_id"].str();
                out["taskRequest"] = util::truncateUtf8(c->task["request"].str(), 300);
                out["busy"] = gitBusy_.count(id) > 0;
                out["auth"] = !state["remote"].isObject() ? J({{"state", "none"}})
                              : gitAuth_.count(id)        ? gitAuth_[id]
                                                          : J({{"state", "unknown"}});
                if (!op.empty()) out["last"] = result;
                send(out);
            });
        };
        Json state = collect(cwd, rules);
        if (!checkAuth || !state["ok"].boolean() || !state["remote"].isObject()) {
            deliver(state, Json());
            return;
        }
        // The pane opens on "checking" so it can show the sign-in page before any file is offered for push.
        const Json& remote = state["remote"];
        deliver(state, J({{"state", "checking"}, {"host", remote["host"]}, {"ssh", remote["ssh"]}}));
        deliver(state, checkRemote(util::widen(state["root"].str()), remote, false));
    }).detach();
}

void App::gitRemoteOutcome(Conversation& c, bool ok, const std::string& problem, const std::string& op,
                           const Json& result) {
    const bool wasRequired = gitAuth_.count(c.id) && gitAuth_[c.id]["state"].str() == "required";
    if (ok && gitAuth_.count(c.id)) gitAuth_[c.id]["state"] = "ok";
    if (!ok && problem == "auth") {
        if (!wasRequired && op == "auto") toast("error", "遠端倉庫需要登入，自動擷取先暫停；請開啟版本控制登入");
        // A full check fills in the host and the credential helper the sign-in page needs.
        gitRefresh(c, op, result, true);
        return;
    }
    gitRefresh(c, op, result);
}

bool App::handleGit(const Json& m) {
    const auto type = m["type"].str();
    if (!util::startsWith(type, "git.")) return false;
    if (type == "git.rules") {
        Json rules = Json::array();
        for (const auto& r : m["rules"].items())
            if (gitflow::validRule(r.str())) rules.push(util::trim(r.str()));
        Json previous = settings_;
        settings_["git"]["neverCommit"] = rules;
        if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
            settings_ = previous;
            toast("error", "永遠不提交規則未能保存，原規則已保留");
        }
        sendSettings();
        if (Conversation* c = find(m["id"].str())) gitRefresh(*c);
        return true;
    }
    Conversation* c = find(m["id"].str());
    if (!c) return true;
    const std::string id = c->id;
    if (type == "git.open" || type == "git.refresh") {
        gitRefresh(*c, "", Json(), type == "git.open" || m["checkAuth"].boolean());
        return true;
    }
    if (type == "git.identity") {
        const auto name = util::trim(m["name"].str()), email = util::trim(m["email"].str());
        bool clean = !name.empty() && name.size() <= 200 && email.size() <= 200 &&
                     email.find('@') != std::string::npos && email.find(' ') == std::string::npos;
        for (unsigned char ch : name + email) clean &= ch >= 32;
        if (!clean) {
            toast("error", "請輸入名稱和有效的 e-mail");
            return true;
        }
        if (c->cwd.empty() || !util::dirExists(c->cwd)) {
            toast("error", "請先選擇專案資料夾");
            return true;
        }
        const bool global = m["global"].boolean(true);
        std::wstring cwd = c->cwd;
        std::weak_ptr<int> life = lifetime_;
        std::thread([this, life, cwd, id, name, email, global] {
            GitRun git{cwd};
            std::string out, err;
            std::vector<std::wstring> scope = {L"config"};
            if (global) scope.push_back(L"--global");
            auto set = [&](const wchar_t* key, const std::string& value) {
                auto args = scope;
                args.push_back(key);
                args.push_back(util::widen(value));
                return git(args, &out, &err) == 0;
            };
            const bool ok = set(L"user.name", name) && set(L"user.email", email);
            const std::string error = ok ? "" : redactText(firstLine(err));
            postToUi([this, life, id, ok, error, global] {
                if (life.expired()) return;
                Conversation* c = find(id);
                if (!c) return;
                if (ok) toast("ok", global ? "已設定這台電腦的 Git 提交身分" : "已設定這個倉庫的 Git 提交身分");
                else toast("error", "提交身分未能保存：" + error);
                gitRefresh(*c, "identity", J({{"op", "identity"}, {"ok", ok}}));
            });
        }).detach();
        return true;
    }
    if (type == "git.login") {
        if (gitBusy_.count(id)) {
            toast("info", "上一個 Git 動作還在進行");
            return true;
        }
        if (c->cwd.empty() || !util::dirExists(c->cwd)) {
            toast("error", "請先選擇專案資料夾");
            return true;
        }
        const bool setupHelper = m["setupHelper"].boolean();
        std::wstring cwd = c->cwd;
        gitBusy_.insert(id);
        send(J({{"type", "git.busy"}, {"id", id}, {"op", "login"}}));
        // Git Credential Manager is a separate process; it may bring its sign-in window to the front.
        AllowSetForegroundWindow(ASFW_ANY);
        std::weak_ptr<int> life = lifetime_;
        std::thread([this, life, cwd, id, setupHelper] {
            GitRun git{cwd};
            std::string root, out, err, url;
            Json result = J({{"op", "login"}, {"ok", false}});
            Json auth;
            if (git({L"rev-parse", L"--show-toplevel"}, &root, &err)) {
                result["error"] = "這個資料夾不是 Git 倉庫";
            } else {
                GitRun repo{util::widen(util::trim(root))};
                std::string remotes;
                repo({L"remote"}, &remotes, &err);
                const auto names = util::splitLines(util::trim(remotes));
                if (names.empty() || repo({L"remote", L"get-url", util::widen(names.front())}, &url, &err)) {
                    result["error"] = "這個倉庫沒有遠端，不需要登入";
                } else {
                    Json remote = gitflow::remoteHost(util::trim(url));
                    remote["name"] = names.front();
                    // The helper is only switched on when the user asked for it on the sign-in page.
                    if (setupHelper && repo({L"credential-manager", L"--version"}, &out, &err, 15000) == 0)
                        repo({L"config", L"--global", L"credential.helper", L"manager"}, &out, &err);
                    auth = checkRemote(util::widen(util::trim(root)), remote, true);
                    result["ok"] = auth["state"].str() == "ok";
                    result["host"] = remote["host"];
                    if (!result["ok"].boolean())
                        result["error"] = auth["detail"].str(auth["state"].str() == "offline" ? "無法連線遠端" : "");
                }
            }
            const std::string top = util::trim(root);
            postToUi([this, life, id, result, auth, top] {
                if (life.expired()) return;
                gitBusy_.erase(id);
                Conversation* c = find(id);
                if (!c) return;
                if (!auth.isNull()) {
                    gitAuth_[id] = auth;
                    gitAuth_[id]["root"] = top;
                }
                if (result["ok"].boolean()) toast("ok", "已登入 " + result["host"].str("遠端倉庫"));
                else toast("error", "登入沒有完成：" + result["error"].str("請再試一次"));
                send(J({{"type", "git.result"}, {"id", id}, {"result", result}}));
                gitRefresh(*c, "login", result);
            });
        }).detach();
        return true;
    }
    if (type == "git.auto") {
        Json previous = settings_;
        settings_["git"]["auto"][id] = J({{"fetch", m["fetch"].boolean()},
                                          {"pull", m["pull"].boolean()},
                                          {"minutes", std::clamp<long long>(m["minutes"].i64(5), 1, 240)}});
        if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
            settings_ = previous;
            toast("error", "自動同步設定未能保存");
        }
        gitFetchAt_.erase(id);
        gitRefresh(*c);
        return true;
    }
    if (c->cwd.empty() || !util::dirExists(c->cwd)) {
        toast("error", "請先選擇專案資料夾");
        return true;
    }
    if (type == "git.diff") {
        const auto path = m["path"].str();
        const bool untracked = m["untracked"].boolean();
        std::wstring cwd = c->cwd;
        std::weak_ptr<int> life = lifetime_;
        std::thread([this, life, cwd, id, path, untracked] {
            GitRun git{cwd};
            std::string root, out, err;
            std::string diff;
            const bool repoFound = git({L"rev-parse", L"--show-toplevel"}, &root, &err) == 0;
            GitRun repo{util::widen(util::trim(root))};
            if (!repoFound) {
                diff.clear();
            } else if (untracked) {
                // A new file has no Git diff; its first 200 KB are shown as added lines.
                std::string content;
                const auto file =
                    util::joinPath(util::widen(util::trim(root)), util::widen(util::replaceAll(path, "/", "\\")));
                if (util::readFile(file, content)) {
                    content = util::truncateUtf8(content, 200 * 1024);
                    diff = "+++ b/" + path + "\n";
                    for (const auto& line : util::splitLines(content)) diff += "+" + line + "\n";
                }
            } else if (!repo({L"diff", L"--no-color", L"HEAD", L"--", util::widen(path)}, &out, &err) ||
                       !repo({L"diff", L"--no-color", L"--cached", L"--", util::widen(path)}, &out, &err)) {
                diff = util::truncateUtf8(out, 200 * 1024);
            }
            postToUi([this, life, id, path, diff] {
                if (!life.expired()) send(J({{"type", "git.diff"}, {"id", id}, {"path", path}, {"diff", diff}}));
            });
        }).detach();
        return true;
    }
    if (type != "git.commit" && type != "git.push" && type != "git.pull" && type != "git.fetch") return true;
    if (gitBusy_.count(id)) {
        toast("info", "上一個 Git 動作還在進行");
        return true;
    }
    // Committing or pulling under a working agent would mix its half-finished edits into history.
    if (type != "git.fetch" && (anyBusy(*c) || c->group.active)) {
        toast("error", "AI 正在工作，請等它完成或先停止，再提交、推送或拉取");
        return true;
    }
    std::vector<std::wstring> paths;
    std::string message = util::trim(m["message"].str());
    if (type == "git.commit") {
        if (message.empty()) {
            toast("error", "請輸入提交訊息");
            return true;
        }
        for (const auto& p : m["paths"].items()) paths.push_back(util::widen(p.str()));
        if (paths.empty()) {
            toast("error", "請至少選擇一個檔案");
            return true;
        }
        if (m["task"].boolean() && !c->task["task_id"].str().empty())
            message += "\n\nCCA-Task: " + c->task["task_id"].str();
    }
    Json allowRisk = m["allowRisk"], rules = settings_["git"]["neverCommit"];
    const bool allowSecrets = m["allowSecrets"].boolean(), alsoPush = m["push"].boolean();
    std::wstring cwd = c->cwd;
    gitBusy_.insert(id);
    send(J({{"type", "git.busy"}, {"id", id}, {"op", type.substr(4)}}));
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, cwd, id, type, paths, message, allowRisk, rules, allowSecrets, alsoPush] {
        GitRun git{cwd};
        std::string root, out, err;
        Json result = J({{"op", type.substr(4)}, {"ok", false}});
        auto push = [&](GitRun& repo) {
            std::string upstream, branch, remote;
            if (repo({L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{u}"}, &upstream, &err) == 0)
                return repo({L"push"}, &out, &err, 120000);
            // A branch without upstream is published to the first remote and tracked from then on.
            if (repo({L"symbolic-ref", L"--short", L"HEAD"}, &branch, &err) || repo({L"remote"}, &remote, &err) ||
                util::trim(remote).empty()) {
                err = "沒有可推送的遠端倉庫；請先設定 remote";
                return 1;
            }
            return repo({L"push", L"-u", util::widen(util::splitLines(util::trim(remote)).front()),
                         util::widen(util::trim(branch))},
                        &out, &err, 120000);
        };
        if (git({L"rev-parse", L"--show-toplevel"}, &root, &err)) {
            result["error"] = "這個資料夾不是 Git 倉庫";
        } else {
            GitRun repo{util::widen(util::trim(root))};
            if (type == "git.commit") {
                // The selection is re-checked against the live status: locked or unconfirmed risky files never enter.
                std::string status;
                repo({L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"}, &status, &err);
                Json live = gitflow::classify(gitflow::parseStatus(status), rules, Json::object());
                std::vector<std::wstring> chosen;
                std::string refused;
                for (const auto& p : paths) {
                    const auto path = util::narrow(p);
                    bool known = false, ok = true;
                    for (const auto& f : live.items())
                        if (f["path"].str() == path) {
                            known = true;
                            if (f["locked"].boolean()) ok = false;
                            bool allowed = false;
                            for (const auto& a : allowRisk.items()) allowed |= a.str() == path;
                            if (f["tone"].str() == "risk" && !f["locked"].boolean() && !allowed) ok = false;
                        }
                    if (known && ok) chosen.push_back(p);
                    else refused += (refused.empty() ? "" : "、") + path;
                }
                if (!refused.empty()) {
                    result["error"] = "這些檔案不能提交或需要先確認：" + refused;
                } else {
                    std::vector<std::wstring> add = {L"add", L"-A", L"--"};
                    add.insert(add.end(), chosen.begin(), chosen.end());
                    std::vector<std::wstring> diff = {L"diff", L"--cached", L"--no-color", L"-U0", L"--"};
                    diff.insert(diff.end(), chosen.begin(), chosen.end());
                    if (repo(add, &out, &err)) result["error"] = "暫存失敗：" + firstLine(err);
                    else if (repo(diff, &out, &err) == 0 && !allowSecrets &&
                             (result["secrets"] = gitflow::secretHits(out)).size() > 0) {
                        // Leave the index as it was: the user decides after seeing the findings.
                        std::vector<std::wstring> reset = {L"reset", L"-q", L"--"};
                        reset.insert(reset.end(), chosen.begin(), chosen.end());
                        if (repo(reset, &out, &err)) {
                            std::vector<std::wstring> unstage = {L"rm", L"--cached", L"-q", L"-r", L"--"};
                            unstage.insert(unstage.end(), chosen.begin(), chosen.end());
                            repo(unstage, &out, &err);
                        }
                        result["error"] = "提交前掃描發現疑似金鑰，已取消暫存";
                    } else {
                        std::vector<std::wstring> commit = {L"commit", L"-q", L"-m", util::widen(message), L"--"};
                        commit.insert(commit.end(), chosen.begin(), chosen.end());
                        if (repo(commit, &out, &err)) {
                            result["error"] = "提交失敗：" + firstLine(err.empty() ? out : err);
                        } else {
                            result["ok"] = true;
                            repo({L"rev-parse", L"--short", L"HEAD"}, &out, &err);
                            result["commit"] = util::trim(out);
                            result["files"] = (long long)chosen.size();
                            if (alsoPush) {
                                result["pushed"] = push(repo) == 0;
                                if (!result["pushed"].boolean()) {
                                    result["pushError"] = redactText(firstLine(err));
                                    result["authProblem"] = gitflow::authProblem(err);
                                }
                            }
                        }
                    }
                }
            } else {
                std::string prefix;
                if (type == "git.push") {
                    result["ok"] = push(repo) == 0;
                    prefix = "推送失敗：";
                } else if (type == "git.pull") {
                    result["ok"] = repo({L"pull", L"--ff-only"}, &out, &err, 120000) == 0;
                    prefix = "只能快轉拉取，Git 回報：";
                } else {
                    result["ok"] = repo({L"fetch", L"--all", L"--prune"}, &out, &err, 120000) == 0;
                    prefix = "擷取失敗：";
                }
                if (!result["ok"].boolean()) {
                    result["authProblem"] = gitflow::authProblem(err);
                    result["error"] = result["authProblem"].str() == "auth" ? "遠端倉庫需要登入"
                                                                            : prefix + redactText(firstLine(err));
                }
            }
        }
        postToUi([this, life, id, result] {
            if (life.expired()) return;
            gitBusy_.erase(id);
            Conversation* c = find(id);
            if (!c) return;
            if (result["ok"].boolean()) {
                const auto op = result["op"].str();
                toast("ok", op == "commit" ? "已提交 " + result["commit"].str() +
                                                 (result["pushed"].boolean() ? "，並推送到遠端" : "")
                            : op == "push" ? "已推送到遠端"
                            : op == "pull" ? "已快轉拉取遠端提交"
                                           : "已擷取遠端更新");
                if (result.has("pushed") && !result["pushed"].boolean())
                    toast("error", "提交成功，但推送失敗：" + result["pushError"].str());
            } else if (result["secrets"].size() == 0) {
                toast("error", result["error"].str("Git 動作失敗"));
            }
            send(J({{"type", "git.result"}, {"id", id}, {"result", result}}));
            const auto op = result["op"].str();
            const bool remote = op != "commit" || result.has("pushed");
            if (!remote) gitRefresh(*c, op, result);
            else
                gitRemoteOutcome(*c, op == "commit" ? result["pushed"].boolean() : result["ok"].boolean(),
                                 result["authProblem"].str(), op, result);
        });
    }).detach();
    return true;
}

void App::tickGit(long long now) {
    if (!uiReady_ || now - gitCheckAt_ < kAutoCheckMs) return;
    gitCheckAt_ = now;
    for (auto& cp : convs_) {
        Conversation& c = *cp;
        const Json a = gitSettings(c.id);
        if (!a["fetch"].boolean() || c.cwd.empty() || !util::dirExists(c.cwd) || gitBusy_.count(c.id)) continue;
        // A remote that needs signing in is left alone until the user signs in from the pane.
        if (gitAuth_.count(c.id) && gitAuth_[c.id]["state"].str() == "required") continue;
        if (now - gitFetchAt_[c.id] < a["minutes"].i64() * 60000) continue;
        gitFetchAt_[c.id] = now;
        // Pulling only fast-forwards, and never while an agent of this conversation is working.
        const bool pull = a["pull"].boolean() && !anyBusy(c) && !c.group.active;
        const std::wstring cwd = c.cwd;
        const std::string id = c.id;
        gitBusy_.insert(id);
        std::weak_ptr<int> life = lifetime_;
        std::thread([this, life, cwd, id, pull] {
            GitRun git{cwd};
            std::string out, err;
            Json result = J({{"op", "auto"}, {"ok", git({L"fetch", L"--all", L"--prune"}, &out, &err, 120000) == 0}});
            if (!result["ok"].boolean()) result["authProblem"] = gitflow::authProblem(err);
            long long behind = 0, ahead = 0;
            if (result["ok"].boolean() &&
                !git({L"rev-list", L"--left-right", L"--count", L"HEAD...@{u}"}, &out, &err)) {
                const auto parts = util::trim(out);
                const auto tab = parts.find_first_of(" \t");
                if (tab != std::string::npos) {
                    ahead = std::stoll(parts.substr(0, tab));
                    behind = std::stoll(util::trim(parts.substr(tab)));
                }
            }
            result["behind"] = behind;
            if (pull && behind > 0 && ahead == 0)
                result["pulled"] = git({L"pull", L"--ff-only"}, &out, &err, 120000) == 0;
            postToUi([this, life, id, result] {
                if (life.expired()) return;
                gitBusy_.erase(id);
                Conversation* c = find(id);
                if (!c) return;
                if (result["pulled"].boolean())
                    toast("ok", "已自動拉取 " + std::to_string(result["behind"].i64()) + " 個遠端提交（" +
                                    (c->kind == "group" ? c->name : util::narrow(c->cwd)) + "）");
                gitRemoteOutcome(*c, result["ok"].boolean(), result["authProblem"].str(), "auto", result);
            });
        }).detach();
    }
}
