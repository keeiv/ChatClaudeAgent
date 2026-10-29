#include "ollama.hpp"

#include <windows.h>
#include <winhttp.h>

#include <set>
#include <thread>

#include "local_tools.hpp"
#include "util.hpp"

#pragma comment(lib, "winhttp.lib")

namespace ollama {

int perform(const std::string& method, const std::string& path, const std::string& body,
            const std::function<void(const std::string&)>& onLine, std::atomic<bool>* cancel, std::string* error,
            unsigned long connectTimeoutMs) {
    int status = 0;
    HINTERNET session = WinHttpOpen(L"CCA/0.2", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, L"127.0.0.1", 11434, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, util::widen(method).c_str(), util::widen(path).c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                         : nullptr;
    if (req) {
        // Downloads and long reviews take a while; only connecting is time-limited.
        WinHttpSetTimeouts(req, (int)connectTimeoutMs, (int)connectTimeoutMs, 30000, 0);
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        BOOL sent = WinHttpSendRequest(req, headers, (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (sent && WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &len, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
            std::string pending;
            char buf[8192];
            while (!(cancel && *cancel)) {
                DWORD n = 0;
                if (!WinHttpReadData(req, buf, sizeof buf, &n) || n == 0) break;
                pending.append(buf, n);
                size_t start = 0, nl;
                while ((nl = pending.find('\n', start)) != std::string::npos) {
                    std::string line = pending.substr(start, nl - start);
                    if (!line.empty() && onLine) onLine(line);
                    start = nl + 1;
                }
                pending.erase(0, start);
            }
            if (!pending.empty() && onLine && !(cancel && *cancel)) onLine(pending);
        } else if (error) {
            *error = "連不到 Ollama（錯誤碼 " + std::to_string(GetLastError()) + "）";
        }
    } else if (error) {
        *error = "無法建立連線";
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return status;
}

void request(const std::string& method, const std::string& path, const std::string& body,
             std::function<void(const std::string&)> onLine, std::function<void(int, const std::string&)> onDone,
             std::shared_ptr<std::atomic<bool>> cancel) {
    std::thread([=] {
        std::string error;
        int status = perform(method, path, body, onLine, cancel.get(), &error);
        if (onDone) onDone(status, error);
    }).detach();
}

std::wstring findExe() {
    std::wstring p = util::findOnPath(L"ollama");
    if (!p.empty()) return p;
    p = util::joinPath(util::getEnv(L"LOCALAPPDATA"), L"Programs\\Ollama\\ollama.exe");
    if (util::fileExists(p)) return p;
    p = util::joinPath(util::getEnv(L"ProgramFiles"), L"Ollama\\ollama.exe");
    return util::fileExists(p) ? p : L"";
}

}  // namespace ollama

namespace {

// 16k tokens hold the diff plus a few files read through the tools; an 8B model and this
// window fit in 12 GB of video memory.
const int kContextWindow = 16384;
const size_t kToolBudget = 28 * 1024;  // tool output per turn, so the window is not overrun
const int kMaxRounds = 14;             // tool rounds plus send-backs

}  // namespace

LocalSession::LocalSession(AgentKind kind, AgentEventFn onEvent) : kind_(kind), onEvent_(std::move(onEvent)) {}

LocalSession::~LocalSession() { interrupt(); }

void LocalSession::emit(AgentEvent::Kind k, const std::string& id, Json data) {
    if (onEvent_) onEvent_(AgentEvent{k, id, std::move(data)});
}

void LocalSession::configure(const std::string& model, const std::string& system) {
    model_ = model;
    system_ = system;
}

bool LocalSession::send(const std::string& text, const Json& history, std::string* error) {
    if (busy_) {
        if (error) *error = "本地模型還在審查上一輪";
        return false;
    }
    if (model_.empty()) {
        if (error) *error = "還沒有選擇本地模型";
        return false;
    }
    Json messages = Json::array();
    if (!system_.empty()) messages.push(J({{"role", "system"}, {"content", system_}}));
    for (const auto& m : history.items()) messages.push(m);
    messages.push(J({{"role", "user"}, {"content", text}}));

    busy_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    int turn = ++seq_;
    emit(AgentEvent::Kind::TurnStarted, "", Json::object());
    emit(AgentEvent::Kind::Thinking, "", Json::object());
    std::weak_ptr<LocalSession> self = shared_from_this();
    std::thread(runTurn, self, model_, messages, review_, cancel_, turn).detach();
    return true;
}

// The whole review runs on one worker thread: chat, run the tool calls the model asks
// for, feed the results back, and check the final answer; events go to the UI thread.
void LocalSession::runTurn(std::weak_ptr<LocalSession> self, std::string model, Json messages, LocalReviewSetup setup,
                           std::shared_ptr<std::atomic<bool>> cancel, int turn) {
    using K = AgentEvent::Kind;
    auto post = [self](K kind, std::string id, Json data) {
        postToUi([self, kind, id, data] {
            if (auto s = self.lock()) s->emit(kind, id, data);
        });
    };
    std::set<std::string> readPaths;
    Json readList = Json::array();
    size_t toolBudget = kToolBudget;
    int corrections = 0, toolCalls = 0;
    long long inputTokens = 0, outputTokens = 0, durationMs = 0;
    std::string finalText, finalId, err;
    int status = 200;
    bool interrupted = false;

    for (int round = 0; round < kMaxRounds; ++round) {
        Json body = J({{"model", model},
                       {"messages", messages},
                       {"stream", true},
                       {"options", J({{"num_ctx", kContextWindow}, {"temperature", 0.2}})}});
        if (setup.review) body["tools"] = localToolSpecs();
        if (setup.think) body["think"] = true;
        std::string content, roundErr;
        Json calls = Json::array();
        Json stats = Json::object();
        bool thinking = false;
        std::string textId = "local-" + std::to_string(turn) + "-" + std::to_string(round);
        status = ollama::perform(
            "POST", "/api/chat", body.dump(),
            [&](const std::string& line) {
                const Json j = Json::parse(line);
                if (!j.isObject()) return;
                if (!j["error"].str().empty()) roundErr = j["error"].str();
                if (j["done"].boolean()) stats = j;
                const Json& msg = j["message"];
                if (!msg["thinking"].str().empty() && !thinking) {
                    thinking = true;
                    post(K::Thinking, "", Json::object());
                }
                for (const auto& c : msg["tool_calls"].items()) calls.push(c);
                std::string delta = msg["content"].str();
                if (delta.empty()) return;
                content += delta;
                post(K::TextDelta, textId, J({{"delta", delta}}));
            },
            cancel.get(), &roundErr, 3000);
        inputTokens = stats["prompt_eval_count"].i64(inputTokens);
        outputTokens += stats["eval_count"].i64();
        durationMs += stats["total_duration"].i64() / 1000000;
        if (*cancel) {
            interrupted = true;
            finalText = content;
            break;
        }
        if (!roundErr.empty() || status != 200) {
            err = roundErr;
            break;
        }
        // Some models still wrap their reasoning in <think> tags inside the answer.
        size_t open = content.find("<think>"), close = content.find("</think>");
        if (open != std::string::npos && close != std::string::npos && close > open) {
            content = util::trim(content.substr(0, open) + content.substr(close + 8));
        }
        // A call written into the text counts as a call; whatever followed it was written blind.
        std::string ahead;
        if (setup.review && calls.size() == 0 && extractTextToolCalls(content, &calls, &ahead)) content = ahead;
        post(K::Text, textId, J({{"text", content}}));

        if (calls.size() > 0 && setup.review) {
            Json assistant = J({{"role", "assistant"}, {"content", content}, {"tool_calls", calls}});
            messages.push(assistant);
            for (const auto& call : calls.items()) {
                std::string name = call["function"]["name"].str();
                Json args = call["function"]["arguments"];
                if (args.isString()) args = Json::parse(args.str());
                std::string toolId = textId + "-tool-" + std::to_string(++toolCalls);
                LocalToolResult r;
                if (toolBudget == 0) {
                    r.kind = "read";
                    r.title = "讀取檔案";
                    r.detail = args["path"].str(args["text"].str());
                    r.output = "錯誤：這一輪讀取的內容已達上限，請根據已經讀到的內容給建議。";
                } else {
                    r = runLocalTool(setup.root, name, args);
                    if (r.output.size() > toolBudget)
                        r.output = util::truncateUtf8(r.output, toolBudget) + "\n（已達讀取上限）";
                    toolBudget -= std::min(toolBudget, r.output.size());
                }
                if (r.ok && !r.path.empty() && readPaths.insert(r.path).second) readList.push(r.path);
                post(K::ToolStart, toolId,
                     J({{"kind", r.kind}, {"tool", name}, {"title", r.title}, {"detail", r.detail}, {"path", r.path}}));
                post(K::ToolEnd, toolId,
                     J({{"status", r.ok ? "ok" : "error"}, {"output", util::truncateUtf8(r.output, 4000)}}));
                messages.push(J({{"role", "tool"}, {"content", r.output}, {"tool_name", name}}));
            }
            continue;
        }

        finalText = content;
        finalId = textId;
        if (!setup.review) break;
        LocalReviewCheck check = checkLocalReview(setup.root, content, readPaths, setup.changedPaths, setup.rules);
        if (check.issues.empty() || corrections >= setup.maxCorrections) break;
        ++corrections;
        post(K::Notice, "",
             J({{"text", "CCA 規範審查：" + guardSummary(check.issues) + "，已退回重寫（第 " +
                             std::to_string(corrections) + "/" + std::to_string(setup.maxCorrections) + " 次）"},
                {"level", "warn"},
                {"guard", true}}));
        messages.push(J({{"role", "assistant"}, {"content", content}}));
        messages.push(J({{"role", "user"},
                         {"content", guardCorrection(setup.rules, check.issues, corrections, setup.maxCorrections)}}));
    }

    Json usage = J({{"inputTokens", inputTokens},
                    {"outputTokens", outputTokens},
                    {"contextUsed", inputTokens + outputTokens},
                    {"contextWindow", kContextWindow},
                    {"durationMs", durationMs}});
    if (err.find("more system memory") != std::string::npos) {
        err = "電腦目前可用的記憶體不夠跑這個模型（Ollama：" + err +
              "）。可以關掉一些程式，或在 設定 › 本地模型 換成較小的模型。";
    }
    if (err.find("does not support tools") != std::string::npos) {
        err =
            "這個模型不支援工具呼叫，沒辦法自己讀檔。請在 設定 › 本地模型 換成支援工具的模型（例如 "
            "qwen3、qwen2.5-coder、llama3.1）。";
    }
    Json done;
    if (interrupted) done = J({{"status", "interrupted"}, {"text", finalText}});
    else if (!err.empty() || status != 200) {
        done = J({{"status", "failed"}, {"error", err.empty() ? "Ollama 回應 HTTP " + std::to_string(status) : err}});
    } else {
        done = J({{"status", "completed"},
                  {"text", finalText},
                  {"textId", finalId},
                  {"readPaths", readList},
                  {"corrections", corrections}});
    }
    postToUi([self, usage, done] {
        auto s = self.lock();
        if (!s) return;
        s->busy_ = false;
        if (usage["outputTokens"].i64() > 0) s->emit(AgentEvent::Kind::Usage, "", usage);
        s->emit(AgentEvent::Kind::TurnDone, "", done);
    });
}

void LocalSession::interrupt() {
    if (cancel_) *cancel_ = true;
}
