// Project rules editor: reads and saves the project's AGENTS.md / CLAUDE.md, and turns the compliance checks' most
// frequent send-backs in this project into rule lines the user can add. Agents read those files themselves.
#include "app.hpp"

#include <algorithm>
#include <map>

#include "util.hpp"

namespace {

const char* kRuleFiles[] = {"AGENTS.md", "CLAUDE.md"};

bool ruleFile(const std::string& name) {
    return std::any_of(std::begin(kRuleFiles), std::end(kRuleFiles), [&](const char* f) { return name == f; });
}

// The rule line each kind of send-back points to; codes without a project-level remedy are left out.
const char* suggestionFor(const std::string& code) {
    if (code == "language" || code == "simplified") return "說明一律用台灣繁體中文。";
    if (code == "verdict") return "審查與複審回覆的最後一行寫 VERDICT: APPROVE、CHANGED 或 ISSUES。";
    if (code == "approve-changed") return "這一輪改了檔案就寫 VERDICT: CHANGED，不能寫 APPROVE。";
    if (code == "no-evidence") return "給 APPROVE 之前要先讀檔、搜尋或執行測試，並在回覆裡說明證據。";
    if (code == "test-claim") return "說測試通過時，附上這一輪實際執行的指令與結束碼。";
    if (code.rfind("local-", 0) == 0) return "本地審查員的建議要先查證再採納，回覆裡說明查證了哪幾條。";
    return nullptr;
}

}  // namespace

bool App::handleRules(const Json& m) {
    const std::string type = m["type"].str();
    if (type != "rules.get" && type != "rules.save") return false;
    Conversation* c = find(m["id"].str());
    const Json reqId = m["reqId"];
    if (!c || c->cwd.empty() || !util::dirExists(c->cwd)) {
        send(J({{"type", "rules.result"}, {"reqId", reqId}, {"ok", false}, {"error", "請先選擇專案資料夾"}}));
        return true;
    }
    if (type == "rules.save") {
        const std::string name = m["name"].str(), text = m["text"].str();
        const std::wstring path = util::joinPath(c->cwd, util::widen(name));
        std::string current;
        const bool exists = util::readFile(path, current);
        std::string error;
        if (!ruleFile(name)) error = "只能編輯 AGENTS.md 或 CLAUDE.md";
        else if (anyBusy(*c) || c->group.active) error = "AI 正在工作，停止後再保存，避免規則混進這一輪的修改";
        else if (text.size() > 256 * 1024) error = "規則檔超過 256 KB";
        // Saved only over the version the user opened, so an edit made meanwhile elsewhere is not lost.
        else if ((exists ? current : "") != m["base"].str()) error = "檔案在你編輯時被改過了，請重新開啟後再改";
        else if (!util::writeFileAtomic(path, text)) error = "無法寫入 " + name;
        send(J({{"type", "rules.result"}, {"reqId", reqId}, {"ok", error.empty()}, {"error", error}}));
        if (error.empty()) toast("ok", name + " 已保存");
        return true;
    }
    Json files = Json::array();
    std::string all;
    for (const char* name : kRuleFiles) {
        std::string text;
        const bool exists = util::readFile(util::joinPath(c->cwd, util::widen(name)), text);
        all += text;
        files.push(J({{"name", name}, {"exists", exists}, {"text", text}}));
    }
    // Count this project's send-backs across every conversation that works in the same folder.
    std::map<std::string, int> counts;
    for (const auto& other : convs_) {
        if (_wcsicmp(other->cwd.c_str(), c->cwd.c_str()) != 0) continue;
        for (const auto& msg : other->messages)
            if (msg["kind"].str() == "guard")
                for (const auto& issue : msg["issues"].items())
                    if (const char* line = suggestionFor(issue["code"].str())) ++counts[line];
    }
    std::vector<std::pair<int, std::string>> ranked;
    for (const auto& [line, n] : counts)
        if (all.find(line) == std::string::npos) ranked.push_back({n, line});
    std::sort(ranked.rbegin(), ranked.rend());
    Json suggestions = Json::array();
    for (const auto& [n, line] : ranked)
        if (suggestions.size() < 6) suggestions.push(J({{"text", line}, {"count", n}}));
    send(J({{"type", "rules.result"},
            {"reqId", reqId},
            {"ok", true},
            {"folder", util::narrow(c->cwd)},
            {"files", files},
            {"suggestions", suggestions}}));
    return true;
}
