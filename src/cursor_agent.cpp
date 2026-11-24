// Converts the Cursor Agent CLI stream into CCA events; never trusts a model's claims about edits or exit codes.
#include "cursor_agent.hpp"

#include <windows.h>

#include <cctype>
#include <tuple>

#include "util.hpp"

namespace {

std::wstring parentDir(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::wstring leafName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Folder names are YYYY.MM.DD-hash or YYYY.MM.DD-HH-MM-SS-hash, the same forms the official launcher accepts.
bool versionKey(const std::wstring& wide, std::tuple<int, int, int, int>* key) {
    std::string name = util::narrow(wide);
    int part[6] = {};
    int count = 0;
    size_t i = 0;
    auto digits = [&](int* out) {
        size_t start = i;
        while (i < name.size() && isdigit((unsigned char)name[i]) && i - start < 6)
            *out = *out * 10 + (name[i++] - '0');
        return i > start;
    };
    for (; count < 3; ++count) {
        if (!digits(&part[count])) return false;
        if (count < 2 && (i >= name.size() || name[i++] != '.')) return false;
    }
    if (i >= name.size() || name[i++] != '-') return false;
    size_t mark = i;
    int clock[3] = {};
    bool timed = true;
    for (int k = 0; k < 3 && timed; ++k) {
        size_t start = i;
        timed = digits(&clock[k]) && i - start == 2 && i < name.size() && name[i] == '-';
        if (timed) ++i;
    }
    if (!timed) i = mark;
    size_t hash = i;
    while (i < name.size() && isxdigit((unsigned char)name[i])) ++i;
    if (i == hash || i != name.size()) return false;
    *key = {part[0] * 10000 + part[1] * 100 + part[2], timed ? clock[0] : 0, timed ? clock[1] : 0,
            timed ? clock[2] : 0};
    return true;
}

CursorLaunch bundleIn(const std::wstring& dir) {
    CursorLaunch l;
    auto node = util::joinPath(dir, L"node.exe"), script = util::joinPath(dir, L"index.js");
    if (util::fileExists(node) && util::fileExists(script)) l = {node, script, leafName(dir)};
    return l;
}

CursorLaunch newestVersion(const std::wstring& install) {
    CursorLaunch best;
    std::tuple<int, int, int, int> bestKey{-1, 0, 0, 0};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(util::joinPath(util::joinPath(install, L"versions"), L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::tuple<int, int, int, int> key;
        if (!versionKey(fd.cFileName, &key) || key <= bestKey) continue;
        CursorLaunch l = bundleIn(util::joinPath(util::joinPath(install, L"versions"), fd.cFileName));
        if (!l.ok()) continue;
        best = l;
        bestKey = key;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// The display names carry zero-width characters and doubled spaces; ids are kept byte for byte.
std::string cleanName(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xE2 && i + 2 < in.size()) {
            unsigned char b = (unsigned char)in[i + 1], d = (unsigned char)in[i + 2];
            if ((b == 0x80 && (d == 0x8B || d == 0x8C || d == 0x8D || d == 0x8E || d == 0x8F)) ||
                (b == 0x81 && d == 0xA0)) {
                i += 2;
                continue;
            }
        }
        if (c == 0xEF && i + 2 < in.size() && (unsigned char)in[i + 1] == 0xBB && (unsigned char)in[i + 2] == 0xBF) {
            i += 2;
            continue;
        }
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += (char)c;
    }
    return util::trim(out);
}

bool safeArg(const std::string& value) {
    if (value.empty() || value.size() > 200 || value[0] == '-') return false;
    for (unsigned char c : value)
        if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != ':' && c != '[' && c != ']' && c != '=' && c != ',')
            return false;
    return true;
}

std::string toolKind(const std::string& name) {
    if (name == "shellToolCall" || name == "writeShellStdinToolCall") return "command";
    if (name == "editToolCall" || name == "writeToolCall" || name == "deleteToolCall" ||
        name == "applyAgentDiffToolCall")
        return "edit";
    if (name == "readToolCall" || name == "lsToolCall" || name == "globToolCall" || name == "grepToolCall" ||
        name == "semSearchToolCall" || name == "readLintsToolCall" || name == "readTodosToolCall")
        return "read";
    if (name == "mcpToolCall" || name == "listMcpResourcesToolCall" || name == "readMcpResourceToolCall") return "mcp";
    if (name == "webSearchToolCall" || name == "webFetchToolCall" || name == "fetchToolCall") return "web";
    return "other";
}

std::string toolTitle(const std::string& name, const Json& body) {
    if (name == "function") return body["name"].str("function");
    if (name == "mcpToolCall") return body["args"]["providerIdentifier"].str() + "/" + body["args"]["toolName"].str();
    return util::endsWith(name, "ToolCall") ? name.substr(0, name.size() - 8) : name;
}

const Json& firstField(const Json& object, std::string* key) {
    static const Json none;
    for (const auto& f : object.fields()) {
        *key = f.first;
        return f.second;
    }
    key->clear();
    return none;
}

std::wstring configDir() {
    auto dir = util::getEnv(L"CURSOR_CONFIG_DIR");
    if (!util::trim(util::narrow(dir)).empty()) return dir;
    auto xdg = util::getEnv(L"XDG_CONFIG_HOME");
    if (!util::trim(util::narrow(xdg)).empty()) return util::joinPath(xdg, L"cursor");
    return util::joinPath(util::homeDir(), L".cursor");
}

}  // namespace

std::vector<std::wstring> CursorSession::envToDrop() {
    // Subscription login only: an API key or token in the environment would bill a different account.
    return {L"CURSOR_API_KEY", L"CURSOR_AUTH_TOKEN", L"NODE_OPTIONS", L"NODE_EXTRA_CA_CERTS_OVERRIDE"};
}

std::string CursorSession::permissionProblem(const std::string& permission, const std::wstring& cwd) {
    if (permission == "plan")
        return "Cursor CLI 的無介面模式會直接寫入工作區檔案，CCA "
               "無法保證唯讀規劃；請改用「自動編輯」或「跳過所有確認」。";
    if (permission == "ask")
        return "Cursor CLI 的無介面模式不能逐項詢問，工作區內的修改會直接寫入；請改用「自動編輯」或「跳過所有確認」。";
    if (permission != "edit") return "";
    // Without --force the CLI still runs everything when the user's own approval mode says so.
    const std::wstring global = util::joinPath(configDir(), L"cli-config.json");
    std::string text;
    if (util::readFile(global, text)) {
        Json config = Json::parse(text);
        if (!config.isObject())
            return "無法解析 Cursor 設定檔 " + util::narrow(global) +
                   "，無法確認它的核准模式；請修正設定或改用「跳過所有確認」。";
        std::string mode = config["approvalMode"].str();
        if (!mode.empty() && mode != "allowlist")
            return "Cursor 設定的核准模式是 " + mode +
                   "，會不經詢問執行指令，和「自動編輯」不符；請在 Cursor 改回 allowlist，或在 CCA "
                   "改用「跳過所有確認」。";
    }
    // A project's .cursor\cli.json replaces the user's allow list (the CLI merges it over the global config, and
    // `--trust` silences its prompt), so a repository could let commands run unasked. Only Read(...) entries pass.
    for (std::wstring dir = cwd; !dir.empty();) {
        const std::wstring file = util::joinPath(util::joinPath(dir, L".cursor"), L"cli.json");
        if (util::readFile(file, text)) {
            Json project = Json::parse(text);
            if (!project.isObject())
                return "無法解析專案的 Cursor 設定 " + util::narrow(file) + "；請修正它或改用「跳過所有確認」。";
            for (const auto& entry : project["permissions"]["allow"].items())
                if (!util::startsWith(entry.str(), "Read("))
                    return "專案的 Cursor 設定 " + util::narrow(file) + " 允許 " + util::truncateUtf8(entry.str(), 80) +
                           " 不經詢問執行，和「自動編輯」不符；請移除它，或在 CCA 改用「跳過所有確認」。";
        }
        const std::wstring parent = parentDir(dir);
        if (parent == dir || parent.size() < 3) break;
        dir = parent;
    }
    return "";
}

Json CursorSession::parseModels(const std::string& text) {
    Json models = Json::array();
    for (const auto& raw : util::splitLines(text)) {
        std::string line = util::trim(raw);
        size_t dash = line.find(" - ");
        if (dash == std::string::npos) continue;
        std::string id = util::trim(line.substr(0, dash));
        std::string name = cleanName(line.substr(dash + 3));
        if (!safeArg(id) || name.empty()) continue;
        bool isDefault = false, isCurrent = false;
        for (bool again = true; again;) {
            again = false;
            for (auto [suffix, flag] :
                 {std::pair<const char*, bool*>{"(default)", &isDefault}, {"(current)", &isCurrent}}) {
                if (util::endsWith(name, suffix)) {
                    name = util::trim(name.substr(0, name.size() - strlen(suffix)));
                    *flag = again = true;
                }
            }
        }
        models.push(J({{"value", id},
                       {"displayName", name.empty() ? id : name},
                       {"isDefault", isDefault},
                       {"isCurrent", isCurrent},
                       {"supportedEffortLevels", Json::array()}}));
        if (models.size() >= 500) break;
    }
    return models;
}

CursorLaunch CursorSession::resolve(const std::wstring& configured) {
    std::wstring base = configured;
    if (base.empty()) {
        base = util::joinPath(util::getEnv(L"LOCALAPPDATA"), L"cursor-agent");
        if (!util::dirExists(base)) base = parentDir(util::findOnPath(L"cursor-agent"));
    }
    if (base.empty()) return {};
    std::string lower = util::toLower(util::narrow(base));
    if (util::fileExists(base) && util::endsWith(lower, ".js")) {
        // A bundle given directly runs with its own node.exe, or with Node.js from PATH (custom installs, tests).
        auto node = util::joinPath(parentDir(base), L"node.exe");
        if (!util::fileExists(node)) node = util::findOnPath(L"node");
        return node.empty() ? CursorLaunch{} : CursorLaunch{node, base, leafName(parentDir(base))};
    }
    if (util::fileExists(base) && (util::endsWith(lower, ".cmd") || util::endsWith(lower, ".ps1")))
        base = parentDir(base);
    if (!util::dirExists(base)) return {};
    if (auto here = bundleIn(base); here.ok()) return here;
    return newestVersion(base);
}

void CursorSession::emit(AgentEvent::Kind kind, const std::string& id, Json data) {
    if (events_) events_({kind, id, std::move(data)});
}

bool CursorSession::send(const CursorLaunch& launch, const AgentConfig& config, const std::string& text,
                         std::string* error) {
    if (busy_) {
        if (error) *error = "Cursor 正在工作";
        return false;
    }
    shutdown();
    if (!launch.ok()) {
        if (error) *error = "找不到 Cursor CLI；請先安裝官方 Cursor Agent CLI。";
        return false;
    }
    if (!safeArg(config.model)) {
        if (error) *error = "請先從 Cursor 回報的模型清單選一個模型（例如 Grok）。";
        return false;
    }
    if (std::string problem = permissionProblem(config.permission, config.cwd); !problem.empty()) {
        if (error) *error = problem;
        return false;
    }
    if (!config.resumeId.empty() && !safeArg(config.resumeId)) {
        if (error) *error = "Cursor 對話編號格式不正確";
        return false;
    }
    ProcessOptions opt;
    opt.exe = launch.node;
    opt.cwd = config.cwd;
    opt.dropEnv = envToDrop();
    opt.args = {launch.script,  L"-p",      L"--output-format", L"stream-json",           L"--trust",
                L"--workspace", config.cwd, L"--model",         util::widen(config.model)};
    if (config.permission == "bypass") opt.args.push_back(L"--force");
    if (!config.resumeId.empty()) {
        opt.args.push_back(L"--resume");
        opt.args.push_back(util::widen(config.resumeId));
    }
    opt.setEnv = {{L"NO_COLOR", L"1"}, {L"CURSOR_INVOKED_AS", L"cursor-agent"}};
    process_ = std::make_unique<ChildProcess>();
    busy_ = true;
    gotResult_ = resultOk_ = wroteText_ = thinking_ = false;
    stderr_.clear();
    usage_ = Json::object();
    auto weak = weak_from_this();
    unsigned generation = ++generation_;
    bool ok = process_->start(
        opt,
        [weak, generation](const std::string& line) {
            if (line.size() > 4 * 1024 * 1024) return;
            postToUi([weak, generation, line] {
                if (auto self = weak.lock(); self && self->generation_ == generation && self->busy_) self->line(line);
            });
        },
        [weak, generation](const std::string& line) {
            postToUi([weak, generation, line] {
                if (auto self = weak.lock(); self && self->generation_ == generation)
                    self->stderr_ = util::tailUtf8(self->stderr_ + line + "\n", 1500);
            });
        },
        [weak, generation](unsigned long code) {
            postToUi([weak, generation, code] {
                auto self = weak.lock();
                if (!self || self->generation_ != generation || !self->busy_) return;
                self->busy_ = false;
                bool success = code == 0 && self->gotResult_ && self->resultOk_;
                std::string why = "Cursor 未完成任務；請檢查登入、模型或 Cursor 的權限設定。";
                if (!success && !util::trim(self->stderr_).empty())
                    why += "\n" + cleanName(util::tailUtf8(self->stderr_, 400));
                self->emit(AgentEvent::Kind::TurnDone, "",
                           J({{"status", success ? "completed" : "failed"}, {"error", success ? "" : why}}));
            });
        },
        error);
    if (!ok) {
        busy_ = false;
        return false;
    }
    emit(AgentEvent::Kind::TurnStarted, "", Json::object());
    // With no prompt argument the CLI reads the whole prompt from stdin, so its length never meets argv limits.
    if (!process_->writeLine(config.instructions.empty() ? text : config.instructions + "\n\n" + text)) {
        shutdown();
        if (error) *error = "無法傳送 Cursor 任務";
        return false;
    }
    process_->closeStdin();
    return true;
}

void CursorSession::line(const std::string& text) {
    const Json event = Json::parse(text);
    if (!event.isObject()) return;
    const std::string type = event["type"].str(), sub = event["subtype"].str();
    using K = AgentEvent::Kind;
    if (type == "system" && sub == "init") {
        emit(K::SessionInfo, "", J({{"sessionId", event["session_id"]}, {"model", event["model"]}}));
        if (event["apiKeySource"].str("login") != "login")
            emit(K::Notice, "", J({{"text", "Cursor 回報這次使用 API 金鑰而不是登入的帳號。"}, {"level", "warn"}}));
    } else if (type == "assistant") {
        // Without --stream-partial-output every assistant event is one finished segment between tool calls.
        std::string out;
        for (const auto& part : event["message"]["content"].items())
            if (part["type"].str() == "text") out += part["text"].str();
        if (out.empty()) return;
        static unsigned counter = 0;
        emit(K::Text, "cursor-text-" + std::to_string(++counter), J({{"text", out}}));
        wroteText_ = true;
    } else if (type == "thinking") {
        if (sub == "delta" && !thinking_) emit(K::Thinking, "", Json::object());
        thinking_ = sub == "delta";
    } else if (type == "tool_call") {
        std::string name;
        const Json& body = firstField(event["tool_call"], &name);
        if (name.empty()) return;
        std::string id = event["call_id"].str();
        if (id.empty()) return;
        const Json& args = name == "function" ? body["arguments"] : body["args"];
        if (sub == "started") {
            emit(K::ToolStart, id,
                 J({{"tool", name},
                    {"kind", toolKind(name)},
                    {"title", toolTitle(name, body)},
                    {"command", args["command"]},
                    {"path", args["path"].str()},
                    {"detail", util::truncateUtf8(args.isString() ? args.str() : args.dump(), 3000)}}));
        } else if (sub == "completed") {
            std::string state;
            const Json& value = firstField(body["result"], &state);
            std::string output;
            if (name == "shellToolCall" && (state == "success" || state == "failure")) {
                output = value["stdout"].str();
                if (!value["stderr"].str().empty()) output += (output.empty() ? "" : "\n") + value["stderr"].str();
            } else if (state == "success") {
                output = value["content"].isString() ? value["content"].str()
                                                     : (value.isNull() ? "" : util::truncateUtf8(value.dump(), 3000));
            } else {
                output = value["error"].str(value["errorMessage"].str(value["message"].str()));
                if (state == "rejected" || state == "permissionDenied")
                    output = "Cursor 拒絕了這個動作：無介面執行時，需要確認的操作不會執行。" +
                             (output.empty() ? "" : "\n" + output);
            }
            bool fine = state == "success";
            Json end = J({{"status", fine ? "ok" : "error"}, {"output", util::truncateUtf8(output, 16000)}});
            // The CLI's own shell executor reports the exit code; when it is absent, CCA does not invent one.
            if (name == "shellToolCall" && value["exitCode"].isNumber()) {
                end["exitCode"] = value["exitCode"];
                if (value["exitCode"].i64() != 0) end["status"] = "error";
            }
            emit(K::ToolEnd, id, end);
        }
    } else if (type == "usage") {
        const Json& u = event["usage"];
        for (const char* key : {"inputTokens", "outputTokens", "cacheReadTokens", "cacheWriteTokens"})
            usage_[key] = usage_[key].i64() + u[key].i64();
    } else if (type == "interaction_query" && sub == "request") {
        emit(K::Notice, "",
             J({{"text", "Cursor 想要互動確認（" + event["query_type"].str("未知") + "），無介面執行時一律拒絕。"},
                {"level", "warn"}}));
    } else if (type == "error") {
        emit(K::Error, "", J({{"text", event["message"].str(event["error"].str("Cursor 回報錯誤"))}}));
    } else if (type == "result") {
        gotResult_ = true;
        resultOk_ = sub == "success" && !event["is_error"].boolean();
        const Json& u = event["usage"].isObject() ? event["usage"] : usage_;
        emit(K::Usage, "",
             J({{"inputTokens", u["inputTokens"].i64()},
                {"outputTokens", u["outputTokens"].i64()},
                {"cachedTokens", u["cacheReadTokens"].i64()},
                {"durationMs", event["duration_ms"]},
                {"billing", "cursor-account"}}));
        if (!resultOk_) emit(K::Error, "", J({{"text", event["result"].str("Cursor 執行失敗")}}));
    }
}

void CursorSession::shutdown() {
    ++generation_;
    busy_ = false;
    if (process_) process_->terminate();
    process_.reset();
}
