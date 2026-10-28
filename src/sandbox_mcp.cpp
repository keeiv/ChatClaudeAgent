// Launch description of the shared Docker sandbox MCP server, in the shapes Codex and Claude Code expect.
#include "sandbox_mcp.hpp"

#include "runtime.hpp"
#include "util.hpp"

namespace {

Json serverArgs(const AgentConfig& cfg) {
    const SandboxMcp& s = cfg.sandbox;
    Json args = Json::array();
    for (const std::string& a :
         {std::string("-I"), std::string("-u"), util::narrow(s.script), std::string("--project"), util::narrow(cfg.cwd),
          std::string("--data-dir"), util::narrow(s.dataDir), std::string("--docker"), util::narrow(s.docker)}) {
        args.push(a);
    }
    return args;
}

}  // namespace

SandboxMcp findSandboxMcp() {
    SandboxMcp s;
    std::wstring python = runtimePython();
    std::wstring docker = util::findOnPath(L"docker");
    if (python.empty() || docker.empty()) return s;
    // sandbox_mcp.py imports agent_runtime.py from its own folder, so both must be extracted.
    if (runtimeScript(L"RUNTIME_AGENT_PY", L"agent_runtime.py").empty()) return s;
    std::wstring script = runtimeScript(L"SANDBOX_MCP_PY", L"sandbox_mcp.py");
    if (script.empty() || !util::fileExists(script)) return s;
    s.python = python;
    s.script = script;
    s.docker = docker;
    s.dataDir = util::joinPath(util::appDataDir(), L"sandbox");
    return s;
}

Json codexSandboxConfig(const AgentConfig& cfg) {
    Json server = J({{"command", util::narrow(cfg.sandbox.python)},
                     {"args", serverArgs(cfg)},
                     {"default_tools_approval_mode", "approve"},
                     // A run may take the full 120 s; Codex's default tool timeout is 60 s.
                     {"tool_timeout_sec", 150},
                     {"startup_timeout_sec", 30}});
    return J({{"mcp_servers", J({{kSandboxServer, server}})}});
}

Json claudeSandboxConfig(const AgentConfig& cfg) {
    Json server = J({{"type", "stdio"}, {"command", util::narrow(cfg.sandbox.python)}, {"args", serverArgs(cfg)}});
    return J({{"mcpServers", J({{kSandboxServer, server}})}});
}

Json sandboxCompletion(const Json& content) {
    Json result = content;
    if (content.isString()) result = Json::parse(content.str());
    if (content.isArray()) {
        std::string text;
        for (const auto& block : content.items())
            if (block["type"].str() == "text") text += block["text"].str();
        result = Json::parse(text);
    }
    if (result["isolation"].str() != "docker" || !result["exit_code"].isNumber()) return Json();
    std::string output = result["output"].str();
    if (!result["error"].str().empty()) output += "\n" + result["error"].str();
    return J({{"status", result["exit_code"].i64() == 0 ? "ok" : "error"},
              {"exitCode", result["exit_code"]},
              {"output", util::tailUtf8(output, 16 * 1024)}});
}

std::string sandboxInstructions() {
    return "你可以用 MCP 工具 sandbox_status 與 sandbox_run，在 CCA 的 Docker 沙箱裡執行命令"
           "（Claude 端的完整名稱是 mcp__cca-sandbox__sandbox_run）。沙箱沒有網路，專案原始檔以唯讀方式放在 /input，"
           "只有 /tmp 與 /output 可以寫入，映像必須已經在本機，不會自動下載。跑測試、試執行不信任的程式碼，"
           "或你自己的指令環境無法啟動時，改用它。Windows 專用的建置（build.bat、MSVC）不能在沙箱裡跑，"
           "那類結果以主機實際執行為準。指令環境失敗時先試 sandbox_run，仍然不行再用一句話說明原因，不要反覆回報。";
}
