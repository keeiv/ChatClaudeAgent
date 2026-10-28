// Task profiles separate instructions from pasted evidence and expose routing factors.
#include "routing.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "util.hpp"

namespace {
bool word(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}
std::string match(const std::string& text, std::initializer_list<const char*> phrases, bool risk = false) {
    for (const std::string phrase : phrases) {
        size_t at = 0;
        while ((at = text.find(phrase, at)) != std::string::npos) {
            const size_t end = at + phrase.size();
            bool boundary = !(word(phrase.front()) && at && word(text[at - 1])) &&
                            !(word(phrase.back()) && end < text.size() && word(text[end]));
            const auto prefix = text.substr(at > 54 ? at - 54 : 0, std::min<size_t>(54, at));
            bool negative = false;
            for (const auto stop :
                 {"不要", "不必", "無須", "不需要", "禁止", "不做", "do not ", "don't ", "without "}) {
                auto pos = prefix.rfind(stop);
                if (pos == std::string::npos) continue;
                auto tail = prefix.substr(pos + std::string(stop).size());
                bool clause = false;
                for (const auto mark : {"，", "。", ",", ";", "\n", "但", "而是"})
                    clause |= tail.find(mark) != std::string::npos;
                negative |= tail.size() < 18 && !clause;
            }
            if (boundary && (risk || !negative)) return phrase;
            at = end;
        }
    }
    return "";
}
std::string narrative(const std::string& request, int& evidence) {
    bool fence = false;
    std::string text;
    for (const auto& line : util::splitLines(util::truncateUtf8(request, 65536))) {
        auto s = util::trim(line);
        if (util::startsWith(s, std::string(3, char(96))) || util::startsWith(s, "~~~")) {
            fence = !fence;
            ++evidence;
        } else if (fence || util::startsWith(s, ">") || util::startsWith(s, "Traceback") ||
                   util::startsWith(s, "File \"") || util::startsWith(s, "PS C:"))
            ++evidence;
        else text += s + "\n";
    }
    for (const auto& pair : {std::pair{"「", "」"}, {"『", "』"}, {"“", "”"}, {"\"", "\""}}) {
        size_t at = 0;
        while ((at = text.find(pair.first, at)) != std::string::npos) {
            const auto end = text.find(pair.second, at + std::string(pair.first).size());
            if (end == std::string::npos) break;
            text.replace(at, end + std::string(pair.second).size() - at, " ");
            ++at;
        }
    }
    bool code = false;
    for (auto& ch : text) {
        if (ch == 96) {
            code = !code;
            ch = ' ';
        } else if (ch == '\n') code = false;
        else if (code) ch = ' ';
    }
    return util::toLower(text);
}
bool followup(std::string value) {
    value = util::trim(value);
    for (const auto mark : {"。", "！", "!", ".", "，", ",", "？", "?"}) value = util::replaceAll(value, mark, "");
    const std::string choices =
        "|繼續|繼續工作|繼續修正|繼續處理|繼續被中斷的工作|繼續剛才的工作|接著做|照剛才的計畫|"
        "照這個做|照這個修|開始實作|開始更改|可以開始|修好它|再試一次|"
        "還是有問題|還是壞的|處理剩下的|continue|do it|go ahead|try again|still broken|";
    return !value.empty() && choices.find("|" + value + "|") != std::string::npos;
}
int fileCount(const std::string& text) {
    std::set<std::string> paths;
    std::string token;
    auto finish = [&] {
        const auto dot = token.find_last_of('.');
        if (dot != std::string::npos) {
            const auto ext = token.substr(dot);
            for (const auto known : {".cpp", ".hpp", ".js", ".ts", ".tsx", ".py", ".rs", ".sql", ".css", ".html"})
                if (ext == known) paths.insert(token);
        }
        token.clear();
    };
    for (const unsigned char ch : text) {
        if (word(ch) || ch == '.' || ch == '/' || ch == '\\' || ch == '-') token += ch;
        else finish();
    }
    finish();
    return (int)paths.size();
}
}  // namespace

Json analyzeTask(const std::string& request, const std::string& previousRequest) {
    int evidence = 0;
    const auto text = narrative(request, evidence);
    const auto copy =
        match(text, {"錯字", "標點", "潤飾", "措辭", "文案", "改字", "排版", "翻譯", "拼字", "語氣", "readme", "typo",
                     "spelling", "punctuation", "translate", "proofread", "wording", "copywriting"});
    const auto explain = match(text, {"什麼是", "是什麼", "如何運作", "怎麼做到", "解釋一下", "what is", "what are",
                                      "how does", "explain the meaning"});
    const auto build = match(text, {"實作", "新增功能", "開發功能", "串接", "整合", "設計", "implement",
                                    "add a feature", "build a", "integrate", "design"});
    const auto edit = match(text, {"修復", "修正", "修改", "更改", "新增", "調整", "重寫", "寫一個", "fix", "modify",
                                   "update", "write code", "patch"});
    const auto review = match(text, {"審查", "檢查", "掃描", "audit", "review", "inspect"});
    const auto check =
        match(text, {"回報設定", "顯示設定", "目前版本", "查詢版本", "現在幾點", "current version", "show settings"});
    const auto diagnose =
        match(text, {"根因", "排查", "重現", "間歇", "偶爾", "卡住", "崩潰", "無法啟動", "沒有回應", "仍然失敗",
                     "找不到原因", "debug", "diagnose", "root cause", "intermittent", "crash", "hang", "still fails"});
    const auto design = match(text, {"重構", "重新設計", "系統設計", "架構", "拆分模組", "重整分層", "refactor",
                                     "architecture", "redesign", "system design"});
    const auto broad = match(text, {"整個專案", "整個項目", "全專案", "整個系統", "跨模組", "跨服務", "端到端",
                                    "前後端", "所有模組", "全面", "entire project", "whole project", "across modules",
                                    "end-to-end", "frontend and backend", "all services"});
    const auto local = match(text, {"這一行", "一行", "單一函式", "一個函式", "單一檔案", "一個設定", "這句話",
                                    "one line", "single function", "one function", "single file", "one setting"});
    const auto data = match(
        text,
        {"資料遺失", "資料損毀", "資料被清零", "不可遺失", "不能遺失", "資料庫遷移", "資料搬移", "交易一致", "回滾",
         "備份還原", "data loss", "data corruption", "database migration", "rollback", "transaction consistency"},
        true);
    const auto security = match(
        text,
        {"資安", "越權", "洩漏密鑰", "密鑰洩漏", "權限繞過", "驗證漏洞", "安全審查", "漏洞", "注入", "security audit",
         "sql injection", "authentication", "authorization", "credential leak", "vulnerability", "path traversal"},
        true);
    const auto concurrency = match(text,
                                   {"併發", "並發", "競態", "死鎖", "執行緒", "鎖定", "重入", "冪等", "concurrency",
                                    "race condition", "deadlock", "thread safety", "idempotent", "reentrant"},
                                   true);
    const auto production = match(
        text,
        {"正式環境", "正式資料", "生產環境", "零停機", "服務中斷", "production", "zero downtime", "service outage"},
        true);
    const auto reasoning = match(text, {"證明",        "演算法",       "複雜度",     "效能瓶頸",
                                        "記憶體洩漏",  "比較方案",     "取捨",       "權衡",
                                        "推導",        "最佳化",       "深度思考",   "深入分析",
                                        "prove",       "algorithm",    "complexity", "performance bottleneck",
                                        "memory leak", "trade-off",    "tradeoff",   "derive",
                                        "optimize",    "deep analysis"});
    const auto verify =
        match(text, {"測試", "驗證", "邊界", "回歸", "覆蓋率", "重現", "無錯誤", "嚴絲合縫", "test", "tests", "testing",
                     "verify", "validate", "edge case", "regression", "coverage", "reproduce"});
    const auto preserve =
        match(text,
              {"保持相容", "向下相容", "不破壞", "保留原", "不可遺失", "不可以刪除", "不得刪除", "不改 api", "既有行為",
               "backward compatible", "preserve existing", "do not delete", "without breaking", "keep the api"},
              true);
    const auto limits =
        match(text,
              {"不能新增依賴", "不新增依賴", "跨平台", "多語言", "windows", "linux", "記憶體上限", "時限", "效能要求",
               "no dependencies", "cross-platform", "memory limit", "latency budget"},
              true);
    const auto extraReasoning =
        match(text, {"比較", "取捨", "權衡", "推導", "請證明", "並證明", "compare", "derive", "prove that"});
    const auto formal = match(text, {"證明", "不變量", "形式驗證", "prove", "invariant", "formal verification"});
    const bool definition = !explain.empty() && build.empty() && edit.empty() && diagnose.empty() &&
                            extraReasoning.empty() && request.size() < 700;
    const int kinds =
        (int)!data.empty() + (int)!security.empty() + (int)!concurrency.empty() + (int)!production.empty();
    const int risk = definition ? 0 : kinds ? std::min(40, 28 + (kinds - 1) * 6) : 0;
    int work = !design.empty()                      ? 28
               : !diagnose.empty()                  ? 24
               : !build.empty() || !review.empty()  ? 20
               : !edit.empty()                      ? 17
               : !explain.empty() || !check.empty() ? 6
                                                    : 0;
    const bool copyOnly = !copy.empty() && build.empty() && diagnose.empty() && design.empty() && !risk &&
                          broad.empty() && reasoning.empty();
    if (copyOnly) work = 2;
    int scope = broad.empty() ? local.empty() ? 4 : 2 : 25;
    const int thought = definition                             ? 0
                        : !formal.empty()                      ? 32
                        : !reasoning.empty()                   ? 24
                        : !design.empty() || !diagnose.empty() ? 12
                                                               : 0;
    const int validation = verify.empty() ? 0 : 8;
    const int constraints = (preserve.empty() ? 0 : 8) + (limits.empty() ? 0 : 6);
    int steps = 0;
    for (const auto& line : util::splitLines(text)) {
        auto s = util::trim(line);
        if (util::startsWith(s, "- ") || util::startsWith(s, "* ") ||
            (s.size() > 2 && std::isdigit((unsigned char)s[0]) && (s[1] == '.' || s[1] == ')')))
            ++steps;
    }
    const int files = fileCount(text);
    if (!copyOnly && !definition) {
        scope += std::min(10, std::max(0, steps - 1) * 2) + std::min(12, std::max(0, files - 1) * 4);
        if (text.size() > 3000) scope += 5;
    } else scope = std::min(scope, 4);
    const auto greeting = match(text, {"你好", "謝謝", "哈囉", "hello", "thank you", "thanks"});
    const bool unclear = work == 0 && !risk && !thought && broad.empty() && (greeting.empty() || text.size() > 80);
    if (unclear) work = 28;
    int score = std::clamp(work + scope + risk + thought + validation + constraints, 0, 100);
    int effortScore =
        std::clamp(work + scope + risk * 3 / 4 + thought * 5 / 4 + validation / 2 + constraints / 2, 0, 100);
    if (risk || (!formal.empty() && !definition) || (!broad.empty() && !review.empty())) {
        score = std::max(score, 60);
        effortScore = std::max(effortScore, 60);
    }
    Json factors = Json::array();
    auto factor = [&](const char* dimension, const char* label, const std::string& detail) {
        if (!detail.empty())
            factors.push(
                J({{"dimension", dimension}, {"label", label}, {"evidence", util::truncateUtf8(detail, 100)}}));
    };
    factor("work", "工作內容",
           copyOnly            ? copy
           : !design.empty()   ? design
           : !diagnose.empty() ? diagnose
           : !build.empty()    ? build
           : !edit.empty()     ? edit
           : !review.empty()   ? review
           : !explain.empty()  ? explain
                               : check);
    factor("scope", "影響範圍", !broad.empty() ? broad : !local.empty() ? local : steps > 1 ? "多項要求" : "");
    if (files > 1) factor("scope", "修改目標", std::to_string(files) + " 個檔案線索");
    if (!definition) {
        factor("risk", "資料完整性", data);
        factor("risk", "權限與安全", security);
        factor("risk", "併發與狀態", concurrency);
        factor("risk", "運行環境", production);
    }
    factor("reasoning", "推理需求", definition ? "" : reasoning);
    factor("verification", "驗證要求", verify);
    factor("constraints", "相容與資料限制", preserve);
    factor("constraints", "環境限制", limits);
    if (unclear) factor("uncertainty", "需求界線", "資訊不足，採一般設定");
    const bool continues = followup(text);
    const bool inherited = continues && !previousRequest.empty();
    bool inheritedLight = false;
    if (inherited) {
        const auto previous = analyzeTask(previousRequest, "");
        score = (int)previous["score"].i64();
        effortScore = (int)previous["effortScore"].i64();
        factors = previous["factors"];
        inheritedLight = previous["level"].i64() == 0;
        factor("context", "延續工作", "沿用前一個使用者需求的範圍與風險");
    }
    const bool simpleCheck = !check.empty() && edit.empty() && build.empty() && !risk && !thought && broad.empty();
    const bool light = copyOnly || definition || simpleCheck || inheritedLight || (!greeting.empty() && work == 0);
    const int level = score >= 60 ? 2 : light && score < 20 ? 0 : 1;
    const auto effort = effortScore >= 85 ? "xhigh" : effortScore >= 55 ? "high" : level == 0 ? "low" : "medium";
    std::string reason;
    for (const auto& f : factors.items()) {
        if (!reason.empty()) reason += "；";
        reason += f["label"].str() + "：" + f["evidence"].str();
        if (reason.size() > 450) break;
    }
    if (reason.empty()) reason = "簡短日常互動，使用輕量設定";
    return J({{"level", level},
              {"score", score},
              {"effortScore", effortScore},
              {"targetEffort", effort},
              {"category", level == 2              ? (effortScore >= 85 ? "複合高風險任務" : "深入分析")
                           : level == 0            ? "輕量任務"
                           : unclear && !inherited ? "需求待釐清"
                                                   : "一般任務"},
              {"reason", reason},
              {"factors", factors},
              {"followup", continues},
              {"inherited", inherited},
              {"evidenceLines", evidence},
              {"analysisVersion", 2}});
}
