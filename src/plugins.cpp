// Plugins and MCP servers for each agent. Claude Code is managed through its CLI,
// Codex through the app-server where it has methods and through its CLI otherwise.
#include <algorithm>
#include <cctype>

#include "app.hpp"
#include "util.hpp"

namespace {

bool validServerName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    }
    return true;
}

// cmd.exe would interpret these if a CLI is only reachable through a .cmd shim.
bool shellSafe(const std::string& s) { return s.find_first_of("&|<>^%!\"\r\n") == std::string::npos; }

bool isScript(const std::wstring& exe) {
    return exe.size() > 4 && (_wcsicmp(exe.substr(exe.size() - 4).c_str(), L".cmd") == 0 ||
                              _wcsicmp(exe.substr(exe.size() - 4).c_str(), L".bat") == 0);
}

std::string lastLines(const std::string& out, const std::string& err) {
    std::string t = util::trim(err.empty() ? out : err);
    return util::tailUtf8(t, 500);
}

}  // namespace

void App::setBusy(const std::string& key, bool busy) { send(J({{"type", "busy"}, {"key", key}, {"busy", busy}})); }

void App::restartIdleSessions() {
    // Running sessions keep their old plugin set; idle ones restart (and resume) on the next message.
    for (auto& c : convs_) {
        if (c->claude && !c->claude->busy()) {
            c->claude->shutdown();
            c->claude.reset();
        }
    }
    if (codexServer_ && codexServer_->running()) codexServer_->request("config/mcpServer/reload", Json::object());
}

void App::listPlugins(const std::string& agent, bool refresh) {
    std::string key = "plugins." + agent;
    if (agent == "claude") {
        if (claudeStatus_.exe.empty()) {
            send(J({{"type", "plugins"}, {"agent", "claude"}, {"error", "找不到 Claude Code CLI"}}));
            return;
        }
        setBusy(key, true);
        ProcessOptions opt;
        opt.exe = claudeStatus_.exe;
        opt.args = {L"plugin", L"list", L"--json", L"--available"};
        opt.cwd = util::homeDir();
        opt.dropEnv = ClaudeSession::envToDrop();
        runCapture(opt, [this, key](int code, std::string out, std::string err) {
            postToUi([this, key, code, out, err] {
                setBusy(key, false);
                Json j = Json::parse(out.substr(std::min(out.size(), out.find('{'))));
                if (!j.isObject()) {
                    send(J({{"type", "plugins"}, {"agent", "claude"},
                            {"error", "無法讀取 Claude 插件清單：" + lastLines(out, err)}}));
                    return;
                }
                Json installed = Json::array();
                for (const auto& p : j["installed"].items()) {
                    installed.push(J({{"id", p["id"]}, {"version", p["version"]}, {"enabled", p["enabled"]},
                                      {"scope", p["scope"]}}));
                }
                Json available = Json::array();
                for (const auto& p : j["available"].items()) {
                    available.push(J({{"id", p["pluginId"]}, {"name", p["name"]}, {"description", p["description"]},
                                      {"marketplace", p["marketplaceName"]}, {"installs", p["installCount"]}}));
                }
                (void)code;
                send(J({{"type", "plugins"}, {"agent", "claude"}, {"installed", installed}, {"available", available}}));
            });
        }, 180000);
        return;
    }

    std::string err;
    ensureCodexServer();
    if (!codexServer_->running() && !codexServer_->start(&err)) {
        send(J({{"type", "plugins"}, {"agent", "codex"}, {"error", err}}));
        return;
    }
    setBusy(key, true);
    codexServer_->request("plugin/list", J({{"forceRefetch", refresh}}), [this, key](const Json& r, const Json& e) {
        setBusy(key, false);
        if (!e.isNull()) {
            send(J({{"type", "plugins"}, {"agent", "codex"}, {"error", "無法讀取 GPT 插件清單：" + e["message"].str()}}));
            return;
        }
        Json installed = Json::array();
        Json available = Json::array();
        for (const auto& m : r["marketplaces"].items()) {
            std::string market = m["interface"]["displayName"].str(m["name"].str());
            for (const auto& p : m["plugins"].items()) {
                const Json& ui = p["interface"];
                Json item = J({{"id", p["id"]},
                               {"name", p["name"]},
                               {"displayName", ui["displayName"].str(p["name"].str())},
                               {"description", ui["shortDescription"].str(ui["longDescription"].str())},
                               {"category", ui["category"]},
                               {"developer", ui["developerName"]},
                               {"marketplace", market},
                               {"marketplaceName", m["name"]},
                               {"marketplacePath", m["path"]},
                               {"installed", p["installed"]},
                               {"enabled", p["enabled"]}});
                if (p["installed"].boolean()) {
                    installed.push(J({{"id", p["id"]}, {"enabled", p["enabled"]}, {"version", p["localVersion"]}}));
                }
                available.push(item);
            }
        }
        send(J({{"type", "plugins"}, {"agent", "codex"}, {"installed", installed}, {"available", available}}));
    });
}

void App::installPlugin(const Json& m) {
    std::string agent = m["agent"].str();
    std::string id = m["id"].str();
    std::string key = "install." + agent + "." + id;
    if (agent == "claude") {
        if (claudeStatus_.exe.empty() || id.empty()) return;
        if (isScript(claudeStatus_.exe) && !shellSafe(id)) {
            toast("error", "插件名稱含有不支援的字元");
            return;
        }
        setBusy(key, true);
        ProcessOptions opt;
        opt.exe = claudeStatus_.exe;
        opt.args = {L"plugin", L"install", util::widen(id), L"-s", L"user"};
        opt.cwd = util::homeDir();
        opt.dropEnv = ClaudeSession::envToDrop();
        runCapture(opt, [this, key, id](int code, std::string out, std::string err) {
            postToUi([this, key, id, code, out, err] {
                setBusy(key, false);
                if (code == 0) {
                    toast("ok", "已為 Claude 安裝 " + id);
                    restartIdleSessions();
                } else {
                    toast("error", "Claude 插件安裝失敗：" + lastLines(out, err));
                }
                listPlugins("claude", false);
            });
        }, 300000);
        return;
    }
    if (!codexServer_ || !codexServer_->running()) return;
    Json params = J({{"pluginName", m["name"].str()}});
    if (m["marketplacePath"].isString() && !m["marketplacePath"].str().empty()) {
        params["marketplacePath"] = m["marketplacePath"];
    } else {
        params["remoteMarketplaceName"] = m["marketplaceName"];
    }
    setBusy(key, true);
    std::string name = m["displayName"].str(m["name"].str());
    codexServer_->request("plugin/install", params, [this, key, name](const Json& r, const Json& e) {
        setBusy(key, false);
        if (!e.isNull()) {
            toast("error", "GPT 插件安裝失敗：" + e["message"].str());
        } else {
            toast("ok", "已為 GPT 安裝 " + name);
            if (r["appsNeedingAuth"].size() > 0) {
                toast("info", name + " 還需要在 Codex 裡完成授權才能使用");
            }
        }
        listPlugins("codex", false);
    });
}

void App::uninstallPlugin(const Json& m) {
    std::string agent = m["agent"].str();
    std::string id = m["id"].str();
    std::string key = "install." + agent + "." + id;
    if (agent == "claude") {
        if (claudeStatus_.exe.empty() || id.empty()) return;
        if (isScript(claudeStatus_.exe) && !shellSafe(id)) return;
        setBusy(key, true);
        ProcessOptions opt;
        opt.exe = claudeStatus_.exe;
        opt.args = {L"plugin", L"uninstall", util::widen(id), L"-s", L"user"};
        opt.cwd = util::homeDir();
        opt.dropEnv = ClaudeSession::envToDrop();
        runCapture(opt, [this, key, id](int code, std::string out, std::string err) {
            postToUi([this, key, id, code, out, err] {
                setBusy(key, false);
                if (code == 0) {
                    toast("ok", "已移除 Claude 插件 " + id);
                    restartIdleSessions();
                } else {
                    toast("error", "移除失敗：" + lastLines(out, err));
                }
                listPlugins("claude", false);
            });
        });
        return;
    }
    if (!codexServer_ || !codexServer_->running()) return;
    setBusy(key, true);
    codexServer_->request("plugin/uninstall", J({{"pluginId", id}}), [this, key, id](const Json&, const Json& e) {
        setBusy(key, false);
        if (!e.isNull()) toast("error", "移除失敗：" + e["message"].str());
        else toast("ok", "已移除 GPT 插件 " + id);
        listPlugins("codex", false);
    });
}

void App::listMcp() {
    // Claude keeps user-scope servers in ~/.claude.json.
    Json claude = Json::array();
    std::string text;
    if (util::readFile(util::joinPath(util::homeDir(), L".claude.json"), text)) {
        Json j = Json::parse(text);
        for (const auto& s : j["mcpServers"].fields()) {
            const Json& v = s.second;
            std::string type = v["type"].str(v["url"].isString() ? "http" : "stdio");
            claude.push(J({{"name", s.first}, {"transport", type}, {"command", v["command"]},
                           {"args", v["args"]}, {"url", v["url"]}}));
        }
    }
    send(J({{"type", "mcp"}, {"agent", "claude"}, {"items", claude}}));

    if (codexStatus_.exe.empty()) return;
    ProcessOptions opt;
    opt.exe = codexStatus_.exe;
    opt.args = {L"mcp", L"list", L"--json"};
    opt.cwd = util::homeDir();
    runCapture(opt, [this](int, std::string out, std::string err) {
        postToUi([this, out, err] {
            Json arr = Json::parse(out.substr(std::min(out.size(), out.find('['))));
            Json items = Json::array();
            for (const auto& s : arr.items()) {
                const Json& t = s["transport"];
                items.push(J({{"name", s["name"]}, {"transport", t["type"].str() == "stdio" ? "stdio" : "http"},
                              {"command", t["command"]}, {"args", t["args"]}, {"url", t["url"]},
                              {"enabled", s["enabled"]}}));
            }
            if (!arr.isArray() && !util::trim(err).empty()) {
                send(J({{"type", "mcp"}, {"agent", "codex"}, {"items", items}, {"error", lastLines(out, err)}}));
                return;
            }
            send(J({{"type", "mcp"}, {"agent", "codex"}, {"items", items}}));
        });
    });
}

void App::addMcp(const Json& m) {
    std::string name = m["name"].str();
    std::string transport = m["transport"].str("stdio");
    std::string command = util::trim(m["command"].str());
    std::string url = util::trim(m["url"].str());
    if (!validServerName(name)) {
        toast("error", "名稱只能用英文字母、數字、- 和 _");
        return;
    }
    if (transport == "stdio" ? command.empty() : !(util::startsWith(url, "https://") || util::startsWith(url, "http://"))) {
        toast("error", transport == "stdio" ? "請填寫啟動指令" : "請填寫 http(s) 網址");
        return;
    }
    std::vector<std::string> args;
    for (const auto& a : m["args"].items()) {
        if (!a.str().empty()) args.push_back(a.str());
    }
    std::vector<std::string> env;
    for (const auto& e : m["env"].fields()) {
        if (!e.first.empty()) env.push_back(e.first + "=" + e.second.str());
    }

    for (const auto& target : m["targets"].items()) {
        std::string agent = target.str();
        std::wstring exe = agent == "claude" ? claudeStatus_.exe : codexStatus_.exe;
        if (exe.empty()) continue;
        if (isScript(exe)) {
            bool safe = shellSafe(command) && shellSafe(url) && shellSafe(name);
            for (const auto& a : args) safe = safe && shellSafe(a);
            for (const auto& e : env) safe = safe && shellSafe(e);
            if (!safe) {
                toast("error", "參數含有 & | < > ^ % ! 或引號，透過 .cmd 啟動的 CLI 無法安全傳遞");
                continue;
            }
        }
        ProcessOptions opt;
        opt.exe = exe;
        opt.cwd = util::homeDir();
        if (agent == "claude") {
            opt.dropEnv = ClaudeSession::envToDrop();
            opt.args = {L"mcp", L"add", L"-s", L"user"};
            if (transport != "stdio") {
                opt.args.insert(opt.args.end(), {L"--transport", L"http", util::widen(name), util::widen(url)});
            } else {
                opt.args.push_back(util::widen(name));
                for (const auto& e : env) {
                    opt.args.push_back(L"-e");
                    opt.args.push_back(util::widen(e));
                }
                opt.args.push_back(L"--");
                opt.args.push_back(util::widen(command));
                for (const auto& a : args) opt.args.push_back(util::widen(a));
            }
        } else {
            opt.args = {L"mcp", L"add", util::widen(name)};
            if (transport != "stdio") {
                opt.args.insert(opt.args.end(), {L"--url", util::widen(url)});
            } else {
                for (const auto& e : env) {
                    opt.args.push_back(L"--env");
                    opt.args.push_back(util::widen(e));
                }
                opt.args.push_back(L"--");
                opt.args.push_back(util::widen(command));
                for (const auto& a : args) opt.args.push_back(util::widen(a));
            }
        }
        std::string key = "mcp." + agent;
        setBusy(key, true);
        std::string who = agent == "claude" ? "Claude" : "GPT";
        runCapture(opt, [this, key, who, name](int code, std::string out, std::string err) {
            postToUi([this, key, who, name, code, out, err] {
                setBusy(key, false);
                if (code == 0) toast("ok", "已為 " + who + " 加入 MCP 伺服器 " + name);
                else toast("error", who + " 加入 MCP 失敗：" + lastLines(out, err));
                restartIdleSessions();
                listMcp();
            });
        });
    }
}

void App::removeMcp(const Json& m) {
    std::string agent = m["agent"].str();
    std::string name = m["name"].str();
    if (!validServerName(name)) return;
    ProcessOptions opt;
    opt.exe = agent == "claude" ? claudeStatus_.exe : codexStatus_.exe;
    if (opt.exe.empty()) return;
    opt.cwd = util::homeDir();
    if (agent == "claude") {
        opt.dropEnv = ClaudeSession::envToDrop();
        opt.args = {L"mcp", L"remove", L"-s", L"user", util::widen(name)};
    } else {
        opt.args = {L"mcp", L"remove", util::widen(name)};
    }
    std::string who = agent == "claude" ? "Claude" : "GPT";
    runCapture(opt, [this, who, name](int code, std::string out, std::string err) {
        postToUi([this, who, name, code, out, err] {
            if (code == 0) toast("ok", "已從 " + who + " 移除 MCP 伺服器 " + name);
            else toast("error", "移除失敗：" + lastLines(out, err));
            restartIdleSessions();
            listMcp();
        });
    });
}
