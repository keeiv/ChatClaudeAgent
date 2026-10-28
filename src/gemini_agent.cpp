// Converts the official Gemini stream into CCA events without trusting a model's exit claims.
#include "gemini_agent.hpp"

#include "util.hpp"

std::vector<std::wstring> GeminiSession::envToDrop() {
    return {L"GEMINI_API_KEY",
            L"GOOGLE_API_KEY",
            L"GOOGLE_APPLICATION_CREDENTIALS",
            L"GOOGLE_CLOUD_ACCESS_TOKEN",
            L"GOOGLE_GENAI_USE_VERTEXAI",
            L"GOOGLE_GENAI_USE_GCA",
            L"NODE_OPTIONS",
            L"GEMINI_SYSTEM_MD"};
}

void GeminiSession::emit(AgentEvent::Kind kind, const std::string& id, Json data) {
    if (events_) events_({kind, id, std::move(data)});
}

bool GeminiSession::send(const std::wstring& exe, const AgentConfig& config, const std::string& text,
                         std::string* error) {
    if (busy_) {
        if (error) *error = "Gemini 正在工作";
        return false;
    }
    shutdown();
    if (config.permission == "plan") {
        if (error) *error = "Gemini CLI 的無介面計畫模式可能自動轉為寫入，CCA 不會在唯讀規劃權限啟動它。";
        return false;
    }
    ProcessOptions opt;
    opt.exe = exe;
    opt.cwd = config.cwd;
    opt.dropEnv = envToDrop();
    opt.args = {L"--output-format",
                L"stream-json",
                L"--approval-mode",
                config.permission == "bypass" ? L"yolo"
                : config.permission == "edit" ? L"auto_edit"
                : config.permission == "plan" ? L"plan"
                                              : L"default",
                L"-p",
                L"請依照標準輸入中的 CCA 任務與規範執行。"};
    if (!config.model.empty()) {
        opt.args.push_back(L"--model");
        opt.args.push_back(util::widen(config.model));
    }
    if (!config.resumeId.empty()) {
        opt.args.push_back(L"--resume");
        opt.args.push_back(util::widen(config.resumeId));
    }
    // Never inherit API billing. The selected Google OAuth method is checked before dispatch.
    std::wstring accountSettings = util::joinPath(util::appDataDir(), L"gemini-account-settings.json");
    if (!util::writeFileAtomic(
            accountSettings,
            J({{"security",
                J({{"auth", J({{"selectedType", "oauth-personal"}, {"enforcedType", "oauth-personal"}})}})}})
                .dump())) {
        if (error) *error = "無法保存 Gemini 帳號登入限制";
        return false;
    }
    opt.setEnv = {{L"NO_COLOR", L"1"}, {L"NO_BROWSER", L"true"}, {L"GEMINI_CLI_SYSTEM_SETTINGS_PATH", accountSettings}};
    process_ = std::make_unique<ChildProcess>();
    busy_ = true;
    gotResult_ = resultOk_ = false;
    stderr_.clear();
    auto weak = weak_from_this();
    unsigned generation = ++generation_;
    bool ok = process_->start(
        opt,
        [weak, generation](const std::string& text) {
            if (text.size() > 1024 * 1024) return;
            postToUi([weak, generation, text] {
                if (auto self = weak.lock(); self && self->generation_ == generation && self->busy_) self->line(text);
            });
        },
        [weak, generation](const std::string& text) {
            postToUi([weak, generation, text] {
                if (auto self = weak.lock(); self && self->generation_ == generation)
                    self->stderr_ = util::tailUtf8(self->stderr_ + text + "\n", 1500);
            });
        },
        [weak, generation](unsigned long code) {
            postToUi([weak, generation, code] {
                auto self = weak.lock();
                if (!self || self->generation_ != generation || !self->busy_) return;
                self->busy_ = false;
                bool success = code == 0 && self->gotResult_ && self->resultOk_;
                self->emit(AgentEvent::Kind::TurnDone, "",
                           J({{"status", success ? "completed" : "failed"},
                              {"error", success ? "" : "Gemini 未完成任務；請檢查登入、工具權限或 CLI 相容性。"}}));
            });
        },
        error);
    if (!ok) {
        busy_ = false;
        return false;
    }
    emit(AgentEvent::Kind::TurnStarted, "", Json::object());
    if (!process_->writeLine(config.instructions + "\n\n" + text)) {
        shutdown();
        if (error) *error = "無法傳送 Gemini 任務";
        return false;
    }
    process_->closeStdin();
    return true;
}

void GeminiSession::line(const std::string& text) {
    Json event = Json::parse(text);
    std::string type = event["type"].str();
    using K = AgentEvent::Kind;
    if (type == "init") {
        emit(K::SessionInfo, "", J({{"sessionId", event["session_id"]}, {"model", event["model"]}}));
    } else if (type == "message" && event["role"].str() == "assistant") {
        emit(event["delta"].boolean() ? K::TextDelta : K::Text, "gemini-response",
             J({{event["delta"].boolean() ? "delta" : "text", event["content"]}}));
    } else if (type == "tool_use") {
        std::string name = event["tool_name"].str();
        const auto& params = event["parameters"];
        bool command = name == "run_shell_command";
        bool edit = name == "replace" || name == "write_file";
        emit(K::ToolStart, event["tool_id"].str(),
             J({{"tool", name},
                {"kind", command ? "command"
                         : edit  ? "edit"
                                 : "read"},
                {"title", name},
                {"command", params["command"]},
                {"path", params["file_path"].str(params["path"].str())},
                {"detail", util::truncateUtf8(params.dump(), 3000)}}));
    } else if (type == "tool_result") {
        // Headless JSON contains no independently verifiable shell exit code: never invent one.
        emit(K::ToolEnd, event["tool_id"].str(),
             J({{"status", event["status"].str() == "success" ? "ok" : "error"},
                {"output", util::truncateUtf8(event["output"].str(event["error"]["message"].str()), 16000)}}));
    } else if (type == "error") {
        emit(event["severity"].str() == "warning" ? K::Notice : K::Error, "", J({{"text", event["message"]}}));
    } else if (type == "result") {
        gotResult_ = true;
        resultOk_ = event["status"].str() == "success";
        const Json& s = event["stats"];
        emit(K::Usage, "",
             J({{"inputTokens", s["input_tokens"]},
                {"outputTokens", s["output_tokens"]},
                {"cachedTokens", s["cached"]},
                {"durationMs", s["duration_ms"]},
                {"billing", "google-account"}}));
        if (!resultOk_) emit(K::Error, "", J({{"text", event["error"]["message"].str("Gemini 執行失敗")}}));
    }
}

void GeminiSession::shutdown() {
    ++generation_;
    busy_ = false;
    if (process_) process_->terminate();
    process_.reset();
}
