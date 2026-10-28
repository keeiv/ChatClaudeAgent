// Compliance checks on AI replies (see guard.hpp). Everything here is plain code over
// the reply text and the turn's tool blocks, so the same reply always gets the same result.
#include "guard.hpp"

#include <regex>
#include <set>

#include "diff.hpp"
#include "util.hpp"

namespace {

// Simplified-only characters in everyday technical writing. Characters that Taiwan
// also uses in their own right (里、后、面、干、只、台、范、着、准、于、云、杰…) are
// left out, so a correct Traditional reply never trips this list.
const char* kSimplified =
    "这们为发说对时会个来过还没么样现进问题应该实际认证设计开关写读错误检测试运转换处数据库务码输显页网络请"
    "结构类变组线选择录储节点链图标头边缘验户号让给吗听讲话语译简历记归档产东车马门见风飞鸟龙长书鱼气电钱"
    "银铁钟锁镜针钢阅闭闻间闪阶际陆阳阴队隐难颜额顺须预领频饭饮馆驱骤鸡麦黄齐齿龄刚则创别剧动劳势区华单卖"
    "卫厂压厅县参双叠叹吓员呜响哑团园围场坏块坚坛垫备复够夹夺奋奖妈娅孙学宁宝实宠审宪宫宽宾寻导将尔尘尝层"
    "属岁岂岗岛峡币帅师带帮广庄庆废异弃张弹归当彻径忆怀态总恋恶惊惯惩战扑执扩扫扬扰抚抛抢护报担拟拥挂挡挤"
    "挥损据掷摄摆摇撑敌断无旧显晋晒晓暂术机杀杂权条杨极枪柜栏树桥梦楼欢欧毁毕毙汇汉汤沟沪泪泽洁浅浓涌润涩"
    "渊渐湾湿满滚滞灭灯灵灾炼炉烂烦烧热爱爷犹状独狭猎献环玛画畅疗疯痒盖盘矿砖础硕确礼祸离种积称稳穷窃竞笔"
    "筑签粮紧纠红约级纪纯纲纳纵纷纸纹练细织终绍经绕绘绝统继绩续维绵综绿编缓缩罗罚罢职联聪肃胜胆脉脑脱脸舰"
    "艺芦苏荐药获莱营萧补袭装观规视览觉触订讨训议许论访评识诉词诗诚询详谁调谈谢贝负贡财责贤败账货质贩购贯"
    "费贴贵贸资赋赏赔赖赚赛赞赶趋跃践踪轨软轮轻载较辅辆辞达迁远违连迟适递逻遗邮邻郑酱释鉴钮销锐键闲闹阐陈"
    "险随隶雾韦顶项顾饥饰饱驶驻驾骗鲜鸣鹏从两与义乐习买乱争亏亚亲亿仅众优伞伟传伤伦体侧侦俭债倾偿儿兑兴养"
    "兽内册军农冻净减凤击删刘剑办励劲协卢厉吨启喷嘱圣奥妇寿尽届帐弯忧悬愿戏携涨温焕牵胁脏荣誉讯课谱专业丝"
    "丢严临丽举乡仓仪价";

const std::set<wchar_t>& simplifiedSet() {
    static const std::set<wchar_t> set = [] {
        std::set<wchar_t> s;
        for (wchar_t ch : util::widen(kSimplified)) s.insert(ch);
        return s;
    }();
    return set;
}

bool isHan(wchar_t ch) {
    return (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF);
}
bool isKana(wchar_t ch) {
    return ch >= 0x3040 && ch <= 0x30FF;
}
bool isLatin(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

bool isInspection(const std::string& kind) {
    return kind == "read" || kind == "search" || kind == "command" || kind == "mcp" || kind == "web";
}

const char* shortLabel(const std::string& code) {
    if (code == "language") return "說明不是繁體中文";
    if (code == "simplified") return "出現簡體字";
    if (code == "address") return "沒有使用角色稱呼";
    if (code == "persona") return "出戲（自稱 AI）";
    if (code == "verdict") return "缺少 VERDICT";
    if (code == "approve-changed") return "改了檔案卻寫 APPROVE";
    if (code == "no-evidence") return "沒有查證就同意";
    if (code == "test-claim") return "沒有執行卻說測試通過";
    if (code == "local-notes") return "沒有逐條查證本地建議";
    if (code == "local-unread") return "沒有讀檔就下結論";
    if (code == "local-cite") return "引用和檔案對不上";
    if (code == "local-format") return "建議格式不符";
    return "不符合規範";
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : "、") + n;
    return out;
}

}  // namespace

std::string guardProse(const std::string& reply) {
    std::string out;
    bool fence = false;
    for (const auto& raw : util::splitLines(reply)) {
        std::string t = util::trim(raw);
        if (util::startsWith(t, "```") || util::startsWith(t, "~~~")) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        std::string up = t;
        for (auto& c : up) {
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        }
        std::string bare;
        for (char c : up) {
            if (c != '*' && c != '`' && c != '_') bare += c;
        }
        if (util::startsWith(util::trim(bare), "VERDICT")) continue;
        out += raw + "\n";
    }
    static const std::regex inlineCode("`[^`\n]*`");
    static const std::regex url(R"(https?://\S+)");
    static const std::regex drivePath(R"([A-Za-z]:[\\/][^\s，。、）)]*)");
    static const std::regex relPath(R"((?:[\w.\-]+[\\/])+[\w.\-]*)");
    static const std::regex fileName(
        R"(\b[\w\-]+\.(?:cpp|hpp|h|c|cc|js|cjs|mjs|ts|tsx|jsx|json|md|py|bat|cmd|ps1|css|html|txt|yml|yaml|toml|rs|go|java|cs|rb|sh|exe|dll|lock|ini|xml|png|svg)\b)");
    out = std::regex_replace(out, inlineCode, " ");
    out = std::regex_replace(out, url, " ");
    out = std::regex_replace(out, drivePath, " ");
    out = std::regex_replace(out, relPath, " ");
    out = std::regex_replace(out, fileName, " ");
    return out;
}

std::vector<std::string> guardSimplified(const std::string& prose) {
    std::vector<std::string> found;
    std::set<wchar_t> seen;
    const auto& set = simplifiedSet();
    for (wchar_t ch : util::widen(prose)) {
        if (set.count(ch) && seen.insert(ch).second) found.push_back(util::narrow(std::wstring(1, ch)));
    }
    return found;
}

bool guardSaysTestsPassed(const std::string& prose) {
    static const std::wregex patterns[] = {
        std::wregex(L"(測試|單元測試|tests?|檢查)[^。\\n！!？?]{0,16}?(通過|全過|pass(ed)?|綠燈)", std::regex::icase),
        std::wregex(L"\\d+\\s*(項|個|tests?)?\\s*(測試)?\\s*(全部|全數)?\\s*(passed|通過)", std::regex::icase),
    };
    static const wchar_t* negations[] = {L"沒", L"未", L"無法", L"不能", L"不會", L"失敗", L"not", L"n't", L"fail"};
    std::wstring text = util::widen(prose);
    for (const auto& re : patterns) {
        for (std::wsregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            size_t pos = (size_t)it->position(0);
            size_t from = pos > 10 ? pos - 10 : 0;
            std::wstring around =
                util::widen(util::toLower(util::narrow(text.substr(from, pos - from + it->length(0)))));
            bool negated = false;
            for (const wchar_t* n : negations) {
                if (around.find(n) != std::wstring::npos) negated = true;
            }
            // "0 passed" is a failure report, not a claim of success.
            if (!negated && !(it->str(0).size() > 0 && (*it)[0].first != text.end() && *(*it)[0].first == L'0'))
                return true;
        }
    }
    return false;
}

std::vector<GuardIssue> guardCheck(const GuardRules& rules, const std::string& reply, const Json& blocks) {
    std::vector<GuardIssue> issues;
    std::string prose = guardProse(reply);
    std::wstring wide = util::widen(prose);
    int han = 0, latin = 0, kana = 0;
    for (wchar_t ch : wide) {
        if (isHan(ch)) ++han;
        else if (isLatin(ch)) ++latin;
        else if (isKana(ch)) ++kana;
    }

    // Language: Taiwan Traditional Chinese for everything that is not code.
    std::vector<std::string> simp = guardSimplified(prose);
    int simpCount = 0;
    for (wchar_t ch : wide) simpCount += simplifiedSet().count(ch) ? 1 : 0;
    if (simp.size() >= 2 || simpCount >= 3) {
        std::vector<std::string> sample(simp.begin(), simp.begin() + std::min<size_t>(simp.size(), 6));
        issues.push_back({"simplified", "出現簡體字（" + joinNames(sample) + "），必須改用台灣繁體中文。"});
    }
    if ((han == 0 && latin >= 20) || (latin >= 80 && han * 3 < latin) || kana >= 10) {
        issues.push_back(
            {"language", "說明主要不是中文。除了程式碼、指令、路徑和錯誤訊息之外，必須用台灣繁體中文回覆。"});
    }

    // Persona: the form of address in every reply, and no stepping out of character.
    bool hasText = util::trim(prose).size() > 0;
    if (!rules.address.empty() && hasText && reply.find(rules.address) == std::string::npos) {
        issues.push_back({"address", "沒有稱呼使用者為「" + rules.address + "」。" + rules.personaName +
                                         " 的角色設定要求每則回覆都要用這個稱呼，自然地放在回覆開頭，例如「" +
                                         rules.address + "，……」；不要塞進檔案位置或程式碼裡。"});
    }
    if (!rules.personaName.empty()) {
        static const std::wregex zh(
            L"(作為|身為|我是|我只是|我僅是)\\s*(一個|一名|一款|個)?\\s*(AI|ＡＩ|人工智慧|人工智能|"
            L"語言模型|大型語言模型|聊天機器人|AI\\s*助理|AI\\s*助手|虛擬助理)",
            std::regex::icase);
        static const std::wregex en(
            L"\\b(as an ai|as a (large )?language model|i am an ai|i'm an ai|i am a language model)\\b",
            std::regex::icase);
        if (std::regex_search(wide, zh) || std::regex_search(wide, en)) {
            issues.push_back({"persona", "回覆中自稱 AI 或語言模型，出戲了。請維持「" + rules.personaName +
                                             "」的口吻；角色只影響語氣，技術內容照常完整。"});
        }
    }

    // Group protocol.
    Verdict v = parseVerdict(reply, nullptr);
    if (rules.requireVerdict) {
        if (v == Verdict::None) {
            issues.push_back({"verdict",
                              "最後一行沒有 VERDICT。審查和複審必須以 VERDICT: APPROVE、VERDICT: CHANGED 或 "
                              "VERDICT: ISSUES 其中一行結尾。"});
        } else if (v == Verdict::Approve && rules.changedFiles > 0) {
            issues.push_back(
                {"approve-changed",
                 "這一輪修改了 " + std::to_string(rules.changedFiles) +
                     " 個檔案，卻寫 VERDICT: APPROVE。有修改就要寫 VERDICT: CHANGED，並說明改了什麼、為什麼。"});
        }
    }

    // Evidence: what the turn's tool calls show it actually did.
    int inspections = 0, commands = 0;
    for (const auto& b : blocks.items()) {
        if (b["type"].str() != "tool") continue;
        std::string kind = b["kind"].str();
        if (isInspection(kind)) ++inspections;
        if (kind == "command") ++commands;
    }
    if (rules.requireEvidence && v == Verdict::Approve && inspections == 0) {
        issues.push_back(
            {"no-evidence", "沒有開檔案、搜尋或執行任何指令就寫 APPROVE。同意之前必須實際查證目前的程式碼。"});
    }
    if (rules.checkTestClaims && commands == 0 && guardSaysTestsPassed(prose)) {
        issues.push_back({"test-claim",
                          "回覆說測試通過，但這一輪沒有執行任何指令。請實際執行測試並附上結果；"
                          "如果是引用別人的結果，要寫明不是你這一輪驗證的。"});
    }
    if (!rules.localNames.empty()) {
        std::vector<std::string> missing;
        for (const auto& n : rules.localNames) {
            if (reply.find(n) == std::string::npos) missing.push_back(n);
        }
        if (!missing.empty() || inspections == 0) {
            std::string why =
                !missing.empty() ? "沒有回應 " + joinNames(missing) + " 的建議" : "沒有任何讀檔、搜尋或指令紀錄";
            issues.push_back({"local-notes", why + "。本地審查員的每一條建議都要寫出查證方法（讀了哪個檔案、跑了什麼）"
                                                   "和結論（採納或不採納，以及理由）。"});
        }
    }
    return issues;
}

std::string guardRuleCard(const GuardRules& rules) {
    std::string card = "【CCA 規範｜程式會逐則檢查，不符合會被退回重寫】\n";
    card += "- 用台灣繁體中文回覆，不能有簡體字；程式碼、指令、路徑保留原文。\n";
    if (!rules.personaName.empty()) {
        card += "- 你是「" + rules.personaName + "」";
        if (!rules.address.empty()) card += "，每則回覆都要稱呼使用者「" + rules.address + "」";
        card += "；不要說自己是 AI 或語言模型。\n";
    }
    if (rules.requireVerdict)
        card += "- 最後一行寫 VERDICT: APPROVE／CHANGED／ISSUES；這一輪改了檔案就不能寫 APPROVE。\n";
    if (rules.requireEvidence) card += "- 寫 APPROVE 之前必須真的開檔案、搜尋或執行指令查證。\n";
    if (rules.checkTestClaims) card += "- 說測試通過，這一輪就必須真的執行過測試。\n";
    if (!rules.localNames.empty()) {
        card +=
            "- 本地審查員（" + joinNames(rules.localNames) + "）的建議要逐條寫出查證方法與結論，並提到他們的名字。\n";
    }
    return card;
}

std::string guardInstructions() {
    return "# CCA 強制規範\n"
           "CCA 會用程式逐則檢查你的回覆。不符合的回覆不會被採用，CCA 會附上違規清單退回給你重寫；"
           "連續兩次仍不符合，就暫停並交給使用者處理。\n"
           "1. 語言：說明一律使用台灣繁體中文，不能出現簡體字；程式碼、指令、路徑和錯誤訊息保留原文。\n"
           "2. 角色：有指定角色時，每則回覆都要用角色設定的稱呼稱呼使用者，不要說自己是 AI、語言模型或助理。\n"
           "3. 協作：審查與複審的最後一行必須是 VERDICT: APPROVE、VERDICT: CHANGED 或 VERDICT: ISSUES；"
           "這一輪修改了檔案就不能寫 APPROVE。\n"
           "4. 取證：寫 APPROVE 之前必須實際開檔案、搜尋或執行指令；聲稱測試通過時，同一輪必須真的執行過測試。\n"
           "5. 本地審查員的建議只能當線索，要逐條寫出你怎麼查證、結論是什麼，並提到審查員的名字。";
}

std::string guardCorrection(const GuardRules& rules, const std::vector<GuardIssue>& issues, int attempt,
                            int maxAttempts) {
    std::string p =
        "【CCA 規範審查未通過｜第 " + std::to_string(attempt) + "/" + std::to_string(maxAttempts) + " 次退回】\n";
    p += "你上一則回覆不符合 CCA 的規範，這個版本不會被採用：\n";
    int n = 0;
    bool needTools = false;
    for (const auto& i : issues) {
        p += std::to_string(++n) + ". " + i.text + "\n";
        if (i.code == "no-evidence" || i.code == "test-claim" || i.code == "local-notes") needTools = true;
    }
    p += "\n請修正以上問題後重新完整回覆，不要只回答「已修正」。";
    if (rules.requireVerdict) p += "回覆的最後一行仍然要寫 VERDICT。";
    if (needTools) p += "需要查證的部分請實際使用工具（開檔案、搜尋、執行測試），不能只改文字。";
    if (attempt >= maxAttempts) p += "這是最後一次，仍不符合就會暫停並交給使用者處理。";
    return p;
}

Json guardIssuesJson(const std::vector<GuardIssue>& issues) {
    Json arr = Json::array();
    for (const auto& i : issues) arr.push(J({{"code", i.code}, {"label", shortLabel(i.code)}, {"text", i.text}}));
    return arr;
}

std::string guardSummary(const std::vector<GuardIssue>& issues) {
    std::string out;
    std::set<std::string> seen;
    for (const auto& i : issues) {
        std::string label = shortLabel(i.code);
        if (seen.insert(label).second) out += (out.empty() ? "" : "、") + label;
    }
    return out;
}
