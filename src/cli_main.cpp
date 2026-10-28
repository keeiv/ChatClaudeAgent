// ChatClaudeAgent CLI: the desktop version's App core, drawn in the terminal instead
// of a WebView, so it needs no browser engine. Finished display lines are written
// into the terminal's own scrollback once; only the live region is redrawn. App
// still keeps the conversation history for persistence, handoffs and /show.
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "cli_render.hpp"
#include "term.hpp"
#include "themes.hpp"
#include "updater.hpp"
#include "util.hpp"
#include "version.hpp"

using tui::Line;
using tui::Span;
using tui::Style;
namespace C = tui::color;

namespace {

std::mutex g_mu;
std::deque<std::function<void()>> g_queue;
HANDLE g_wake = nullptr;
std::atomic<bool> g_closing{false};
HANDLE g_closed = nullptr;

BOOL WINAPI onConsoleClose(DWORD type) {
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process when this returns, so wait for the conversations to be saved.
        g_closing = true;
        if (g_wake) SetEvent(g_wake);
        if (g_closed) WaitForSingleObject(g_closed, 4000);
        return TRUE;
    }
    return TRUE;  // Ctrl+C and Ctrl+Break arrive as keys; nothing to do here
}

Line operator+(Line a, const Line& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
Line L(const std::string& text, Style st = Style()) { return tui::line(text, st); }
std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}
std::string toLowerAscii(std::string s) { return util::toLower(std::move(s)); }

// ---- the input line

struct Editor {
    std::u32string text;
    size_t caret = 0;

    bool empty() const { return text.empty(); }
    std::string utf8() const { return tui::encode(text); }
    void set(const std::string& s) { text = tui::decode(s); caret = text.size(); }
    void clear() { text.clear(); caret = 0; }
    void insert(char32_t c) { text.insert(caret++, 1, c); }
    void backspace() { if (caret > 0) text.erase(--caret, 1); }
    void del() { if (caret < text.size()) text.erase(caret, 1); }
    void left() { if (caret > 0) --caret; }
    void right() { if (caret < text.size()) ++caret; }
    size_t lineStart(size_t at) const { while (at > 0 && text[at - 1] != '\n') --at; return at; }
    size_t lineEnd(size_t at) const { while (at < text.size() && text[at] != '\n') ++at; return at; }
    void home() { caret = lineStart(caret); }
    void end() { caret = lineEnd(caret); }
    bool onFirstLine() const { return lineStart(caret) == 0; }
    bool onLastLine() const { return lineEnd(caret) == text.size(); }
    void up() {
        size_t s = lineStart(caret);
        if (s == 0) return;
        size_t col = caret - s, ps = lineStart(s - 1);
        caret = std::min(ps + col, s - 1);
    }
    void down() {
        size_t e = lineEnd(caret);
        if (e == text.size()) return;
        size_t col = caret - lineStart(caret), ns = e + 1;
        caret = std::min(ns + col, lineEnd(ns));
    }
    void killToStart() { size_t s = lineStart(caret); text.erase(s, caret - s); caret = s; }
    void killToEnd() { text.erase(caret, lineEnd(caret) - caret); }
    void killWord() {
        size_t s = caret;
        while (s > 0 && text[s - 1] == ' ') --s;
        while (s > 0 && text[s - 1] != ' ' && text[s - 1] != '\n') --s;
        text.erase(s, caret - s);
        caret = s;
    }
};

struct Item {
    std::string label, hint, value;
    uint32_t color = 0;
    bool checked = false;
    bool checkable = false;
};

struct Picker {
    std::string title;
    std::vector<Item> items;
    int sel = 0;
    std::u32string filter;
    std::string footer = "↑↓ 選擇 · Enter 確定 · Esc 取消";
    // Returns true to keep the list open (for toggles).
    std::function<bool(Item&)> onPick;
    std::function<void()> onCancel;

    std::vector<int> visible() const {
        std::vector<int> out;
        std::string f = toLowerAscii(tui::encode(filter));
        for (int i = 0; i < (int)items.size(); ++i) {
            if (f.empty() || toLowerAscii(items[i].label + " " + items[i].hint).find(f) != std::string::npos) out.push_back(i);
        }
        return out;
    }
};

struct Ask {
    std::string title, placeholder;
    bool allowEmpty = false;
    std::function<void(const std::string&)> done;
    std::function<void()> cancel;
};

struct Command {
    const char* name;
    const char* args;
    const char* desc;
};

const Command kCommands[] = {
    {"/help", "", "所有指令和快捷鍵"},
    {"/chat", "", "切換對話（私訊或群組）"},
    {"/groups", "[up|down]", "列出固定順序的群組，或調整目前群組的位置"},
    {"/new", "[名稱]", "建立新的協作群組"},
    {"/cd", "[路徑]", "設定這個對話的專案資料夾（. 是目前資料夾）"},
    {"/model", "", "選模型"},
    {"/effort", "", "選思考程度"},
    {"/permission", "", "權限：規劃、每次詢問、自動編輯、跳過所有確認"},
    {"/lead", "[claude|gpt]", "選擇主刀手，接續對話上下文"},
    {"/takeover", "", "停止 Claude 並交接給 GPT"},
    {"/fallback", "[on|off]", "額度耗盡的成員自動跳過；私訊由 GPT 接手 Claude"},
    {"/gemini", "[on|off]", "邀請 Gemini 擔任群組的第三位審查者"},
    {"/stop", "", "停止 AI（也可以按 Esc）"},
    {"/continue", "", "協作暫停時繼續（規範審查暫停時是再退回一次）"},
    {"/accept", "", "規範審查暫停時接受這則回覆並繼續"},
    {"/review", "[turn|task|all]", "列出從檢查點以來改了哪些檔案"},
    {"/revert", "檔案路徑", "把一個檔案放回 /review 時的檢查點版本"},
    {"/undo-revert", "", "復原上一次 /revert"},
    {"/end", "", "協作暫停時結束"},
    {"/show", "", "顯示上一則回覆的完整指令輸出和 diff"},
    {"/name", "", "改你和 AI 的名稱"},
    {"/persona", "", "替 AI 選性格，或建立、刪除自訂性格"},
    {"/humanize", "[on|off]", "去 AI 化開關"},
    {"/flow", "[auto|compact|off]", "協作流程圖：完整、自動收合或隱藏"},
    {"/motion", "[on|off]", "呼吸燈、思考轉圈與交接圓點"},
    {"/daily", "", "私訊的日常模式（最省額度的模型）"},
    {"/rounds", "<次數>", "群組的審查輪數上限"},
    {"/plugins", "[enable|disable ID|scan|open]", "CCA 插件：主題、對話模式與指令"},
    {"/plugin-mode", "[模式ID|off]", "套用或清除這個對話的插件模式"},
    {"/test-guard", "<指令|on|off>", "審查者改檔後跑測試，轉紅就還原它的修改"},
    {"/locals", "", "邀請或移除本地審查員"},
    {"/rename", "<名稱>", "改群組名稱"},
    {"/clear", "", "清除這個對話（AI 也會忘記）"},
    {"/status", "", "登入狀態、用量和記憶體"},
    {"/gemini-login", "", "啟動官方 Gemini CLI 的 Google 帳號登入"},
    {"/resume", "", "接續 CCA 關閉時中斷的群組任務"},
    {"/claude-usage", "[on|off]", "即時查詢 Claude 額度（會讀取 Claude Code 的登入權杖）"},
    {"/theme", "[ID]", "列出或切換已內建的介面主題"},
    {"/auto", "[on|off|preview 需求]", "依需求選擇模型及思考程度，或只預覽"},
    {"/context", "", "查看 CCA 實際交付的需求、規則與上下文來源"},
    {"/queue", "[add|edit|move|remove|retry|pause|resume]", "保存與管理待送指令"},
    {"/update", "[check|install|cancel|open|auto on|off|switch 版本|return]",
     "掃描本機更新資料夾並重新開啟；切換特殊版本或退回普通版本"},
    {"/git",
     "[login|identity 名稱 e-mail|select|commit 訊息|commit-push 訊息|push|pull|fetch|never 規則|allow 規則|auto "
     "fetch|pull on|off]",
     "版本控制：選檔提交、推送、提交紀錄與永遠不提交規則"},
    {"/resume-auto", "[on|off]", "全部額度用完後，額度恢復時自動接續任務"},
    {"/activity", "[daily|weekly|total]", "查看 Token 活動：每日、近 7 日或紀錄累計"},
    {"/about", "", "查看 CCA 的開發目的與設計原則"},
    {"/badges", "[hide|show|first ID]", "查看本機徽章，或調整個人檔案的展示"},
    {"/pipeline", "<JSON檔|off>", "載入目前群組的多代理流程"},
    {"/inbox", "", "列出所有對話的待辦與完成通知"},
    {"/evidence", "", "檢視需求的主機驗證證據"},
    {"/verify", "JSON檔", "依設定執行驗收命令並記錄真實結果"},
    {"/reproduce", "JSON檔", "對修正前後執行相同的重現案例"},
    {"/quality-check", "", "重新核對來源與證據有效性"},
    {"/browser", "網址", "開啟獨立網站預覽工作階段"},
    {"/browser-run", "JSON檔", "執行瀏覽器操作驗收並保存圖片"},
    {"/plan", "[import JSON檔]", "查看或匯入含步驟與驗收條件的計畫"},
    {"/plan-comment", "文字", "留言並退回待核准"},
    {"/plan-approve", "", "核准剛檢查的計畫版本"},
    {"/plan-run", "", "執行已核准的計畫"},
    {"/plan-accept", "步驟識別碼", "由指揮官驗收一項步驟"},
    {"/opinions", "[claude,codex 問題]", "蒐集獨立唯讀意見或查看結果"},
    {"/opinions-stop", "", "停止意見模式並保留結果"},
    {"/opinion-plan", "", "將意見提案轉為待核准計畫"},
    {"/focus", "[成員 關注點識別碼]", "查看或指定審查關注點"},
    {"/focus-import", "JSON檔", "匯入 Markdown 審查關注點清單"},
    {"/workspace", "[create|check|merge]", "建立隔離工作區、預演或合併已提交變更"},
    {"/provider-import", "<JSON檔>", "從本機檔案匯入供應商與金鑰，金鑰不進對話"},
    {"/task-history", "[序號]", "按需讀取本任務的交接索引"},
    {"/sandbox", "", "檢查 Docker 隔離環境是否可用"},
    {"/provider-models", "<id>", "檢查 API 供應商連線與模型清單"},
    {"/provider-remove", "<id>", "移除 API 供應商及加密金鑰"},
    {"/toolserver-import", "<JSON檔>", "檢查並核准 MCP 宿主工具設定"},
    {"/toolserver-test", "<id>", "連線並發現已核准的 MCP 工具，列出釘住狀態"},
    {"/toolserver-pin", "<id> <工具…>", "釘住剛檢視過的工具描述；之後被改動的工具會被阻擋"},
    {"/toolserver-remove", "<id>", "移除 MCP 工具設定"},
    {"/pause", "", "暫停多代理任務並保存目前步驟"},
    {"/rereview", "", "從審查代理重新檢查目前任務"},
    {"/exit", "", "離開"},
};

const char* kSpinner[] = {"·", "✢", "✳", "✶", "✻", "✽", "✻", "✶", "✳", "✢"};

struct PermInfo {
    const char* id;
    const char* label;
    const char* desc;
};
const PermInfo kPerms[] = {
    {"plan", "規劃模式", "只閱讀和分析，不修改檔案，先提出計畫給你核准"},
    {"ask", "每次詢問", "修改檔案、執行指令之前都會先問你"},
    {"edit", "自動編輯", "專案資料夾內的修改自動允許，其他指令和網路仍會先問你"},
    {"bypass", "跳過所有確認", "完全不詢問：AI 可以直接改任何檔案、執行任何指令"},
};

const char* kEfforts[][2] = {
    {"minimal", "Minimal"}, {"low", "Low"},   {"medium", "Medium"}, {"high", "High"},
    {"xhigh", "XHigh"},     {"max", "Max"},   {"ultra", "Ultra"},   {"ultracode", "Ultracode"},
};

std::string effortLabel(const std::string& e) {
    for (auto& x : kEfforts) {
        if (e == x[0]) return x[1];
    }
    return e;
}

std::string shortPath(const std::string& p) {
    std::string home = util::narrow(util::homeDir());
    if (!home.empty() && util::startsWith(toLowerAscii(p), toLowerAscii(home))) return "~" + p.substr(home.size());
    return p;
}

// ---- the CLI

class Cli {
public:
    int run(int argc, wchar_t** argv);

private:
    // host messages
    void onHost(const std::string& json);
    void drain();
    void ui(Json m) {
        app_->onUiMessage(m.dump());
        drain();
    }

    // conversations
    const Json* conv(const std::string& id) const;
    const Json* conv() const { return conv(current_); }
    const Json& state() const;
    std::string name(const std::string& who) const;
    std::string convTitle(const Json& c) const;
    const Json* persona(const std::string& who) const;
    std::map<std::string, std::string> queuedDrafts_;
    // Tool hashes the user saw in the latest /toolserver-test, per server; /toolserver-pin can only pin these.
    std::map<std::string, Json> toolHashes_;
    Json updateChoices_ = Json::array();  // edition switches from the latest update.state
    Json extensions_ = Json::object();    // CCA plugins as the host validated them
    bool running() const { return state()["running"].boolean(); }
    bool motionOn() const { return systemMotion_ && settings_["visual"]["motion"].boolean(true); }
    void open(const std::string& id);

    // output
    void commit(const std::vector<Line>& lines);
    void commit(const Line& l) { commit(std::vector<Line>{l}); }
    void note(const std::string& text, uint32_t c = C::kMuted, const std::string& mark = "·");
    void printHeader();
    void printHistory();
    void frame();
    std::vector<Line> buildLive(int cols, int rows, int* caretRow, int* caretCol);
    std::vector<Line> box(const std::string& title, uint32_t c, const std::vector<Line>& body, const std::string& footer, int cols) const;
    std::vector<Line> inputBox(int cols, int* caretRow, int* caretCol);
    std::vector<Line> pickerBox(int cols);
    std::vector<Line> widgetBox(int cols);
    std::vector<Line> statusLines(int cols);
    Line footerLine(int cols);

    // input
    void handleKey(const term::Key& k);
    bool widgetKey(const term::Key& k);
    void pickerKey(const term::Key& k);
    void submit();
    void runCommand(const std::string& cmd, const std::string& args);
    std::vector<const Command*> matches() const;

    // widgets
    const Json* pendingBlock(std::string* msgId) const;
    void choose(const Json& block, const std::string& msgId, const std::string& choice);
    void answerQuestion(const Json& block, const std::string& msgId, int option);
    void openPicker(Picker p) { picker_ = std::make_unique<Picker>(std::move(p)); dirty_ = true; }
    void ask(Ask a, const std::string& initial = "");
    void toast(const std::string& text, const std::string& level = "info");

    // commands with several steps
    void pickAgent(const std::string& title, std::function<void(const std::string&)> then, bool withLocals = false,
                   bool withUser = false, bool withGemini = false);
    void pickModel(const std::string& agent);
    void pickEffort(const std::string& agent);
    void pickPermission();
    void pickPersona(const std::string& who);
    void newPersona(const std::string& who);
    void showStatus();
    void showHelp();
    void arrangeConversations();

    std::unique_ptr<App> app_;
    std::vector<std::string> inbox_;
    bool booted_ = false;
    std::string version_, winUser_;
    Json convs_ = Json::array(), states_ = Json::object(), settings_ = Json::object(), personas_ = Json::array();
    Json auth_ = Json::object(), models_ = Json::object(), usage_ = Json::object();
    Json badges_ = Json::object();
    Json groupOrder_ = Json::array();
    std::string current_;
    std::string reviewBase_;  // checkpoint of the last /review, so /revert puts files back to it
    std::string reviewHead_;  // refuse to discard edits made after /review
    std::string reviewConv_;
    long long planRevision_ = -1;
    std::string planConv_;
    Json mergePreview_;
    std::string mergeConv_;
    bool showInbox_ = false;
    std::string revertBackup_;  // checkpoint taken before the last /revert, for /undo-revert
    std::vector<Json> msgs_;
    bool historyPending_ = false;
    std::set<std::string> unread_;
    std::string authShown_;
    std::wstring launchDir_;

    // what is already in the scrollback
    size_t doneMsgs_ = 0, partial_ = 0;
    int partialWidth_ = 0;
    std::string pendingOut_;
    // the live region drawn last time
    std::vector<int> liveWidths_;
    int caretRow_ = 0, caretCol_ = 0, lastCols_ = 0;
    bool dirty_ = true;
    long long lastFrame_ = 0;

    Editor ed_;
    std::vector<std::string> history_;
    int histPos_ = -1;
    std::string histDraft_;
    int popupSel_ = 0;
    std::unique_ptr<Picker> picker_;
    Picker* picking_ = nullptr;  // the list whose onPick is running (it is off screen meanwhile)
    std::unique_ptr<Ask> ask_;
    std::string askDraft_;
    // the approval or question being answered
    std::string widgetId_;
    int widgetSel_ = 0, questionAt_ = 0;
    Json answers_ = Json::object();
    std::string toast_, toastLevel_;
    long long toastUntil_ = 0;
    long long lastCtrlC_ = 0;
    bool quit_ = false;
    bool updateAsked_ = false;  // routine folder scans stay quiet unless /update asked for the result
    // Version control: the last reported state and the files chosen for the next commit.
    Json gitState_;
    std::set<std::string> gitSel_;
    bool gitSeeded_ = false, gitAsked_ = false;
    std::string gitAuthSeen_;  // "<conversation>:<sign-in state>" last printed
    void showGit();
    void showGitAuth();
    void gitCommand(const std::string& args);
    bool systemMotion_ = true;
};

// ------------------------------------------------------------ host messages

void Cli::drain() {
    while (!inbox_.empty()) {
        std::vector<std::string> batch;
        batch.swap(inbox_);
        for (const auto& s : batch) onHost(s);
    }
}

void Cli::arrangeConversations() {
    Json ordered = Json::array();
    std::set<std::string> added;
    for (const auto& c : convs_.items())
        if (c["kind"].str() != "group") {
            ordered.push(c);
            added.insert(c["id"].str());
        }
    for (const auto& id : groupOrder_.items())
        for (const auto& c : convs_.items())
            if (c["kind"].str() == "group" && c["id"] == id && added.insert(id.str()).second) ordered.push(c);
    for (const auto& c : convs_.items())
        if (added.insert(c["id"].str()).second) ordered.push(c);
    convs_ = ordered;
}

void Cli::onHost(const std::string& json) {
    Json m = Json::parse(json);
    std::string type = m["type"].str();
    dirty_ = true;
    if (type == "boot") {
        booted_ = true;
        version_ = m["version"].str();
        winUser_ = m["user"]["name"].str();
        personas_ = m["personas"];
        convs_ = m["convs"];
        groupOrder_ = m["groupOrder"];
        arrangeConversations();
        for (const auto& s : m["states"].items()) states_[s["id"].str()] = s;
        settings_ = m["settings"];
        tui::setTheme(settings_["theme"].str("cca"));
        badges_ = m["badges"];
        models_ = m["models"];
        usage_ = m["usage"];
    } else if (type == "auth") {
        auth_ = m;
        std::string key;
        for (const char* a : {"claude", "codex", "gemini"}) key += std::string(a) + "=" + m[a]["state"].str() + ";";
        if (key != authShown_ && m["claude"]["state"].str() != "checking" && m["codex"]["state"].str() != "checking") {
            authShown_ = key;
            for (const char* a : {"claude", "codex", "gemini"}) {
                const Json& s = m[a];
                std::string cli = std::string(a) == "claude"   ? "Claude Code"
                                  : std::string(a) == "gemini" ? "Gemini CLI"
                                                               : "Codex";
                if (s["state"].str() == "ok" || s["state"].str() == "checking" || s["state"].str() == "loggingIn" ||
                    s.isNull())
                    continue;
                note(cli + "：" + s["message"].str("還不能使用"), C::kWarn, "!");
                if (std::string(a) == "gemini") {
                    note(s["state"].str() == "missing" ? "安裝：先裝 Node.js，再執行 npm install -g @google/gemini-cli"
                         : s["state"].str() == "retired" ? "說明：https://geminicli.com/docs/resources/"
                                                           "quota-and-pricing/ ；改用受支援帳號可執行 /gemini-login"
                                                         : "登入：/gemini-login，完成後輸入 /status 重新檢查");
                } else if (s["state"].str() == "missing") {
                    note(std::string(a) == "claude" ? "安裝：在 PowerShell 執行 irm https://claude.ai/install.ps1 | iex"
                                                   : "安裝：先裝 Node.js，再執行 npm install -g @openai/codex");
                } else if (s["state"].str() == "loggedOut") {
                    note(std::string(a) == "claude" ? "登入：在另一個終端機執行 claude auth login，完成後輸入 /status 重新檢查"
                                                   : "登入：在另一個終端機執行 codex login，完成後輸入 /status 重新檢查");
                }
            }
        }
    } else if (type == "convs") {
        convs_ = m["items"];
        groupOrder_ = m["groupOrder"];
        arrangeConversations();
        if (!conv() && convs_.size() > 0) open(convs_.at(0)["id"].str());
    } else if (type == "conv.created") {
        open(m["id"].str());
    } else if (type == "history") {
        if (m["id"].str() == current_) {
            msgs_ = m["messages"].items();
            if (historyPending_) {
                historyPending_ = false;
                printHistory();
            } else {
                // Rolled back or cleared: start the view over below what is already printed.
                note("對話內容已更新", C::kBrand, "↺");
                doneMsgs_ = msgs_.size();
                partial_ = 0;
                for (size_t i = 0; i < msgs_.size(); ++i) {
                    if (msgs_[i]["streaming"].boolean()) { doneMsgs_ = i; break; }
                }
            }
        }
    } else if (type == "msg") {
        std::string id = m["convId"].str();
        const Json& msg = m["msg"];
        if (id != current_ || historyPending_) {
            if (!msg["streaming"].boolean() && msg["author"].str() != "user") unread_.insert(id);
            return;
        }
        bool found = false;
        for (auto& x : msgs_) {
            if (x["id"].str() == msg["id"].str()) { x = msg; found = true; break; }
        }
        if (!found) msgs_.push_back(msg);
    } else if (type == "convState") {
        states_[m["state"]["id"].str()] = m["state"];
    } else if (type == "inbox") {
        if (showInbox_) {
            note("收件匣 · " + std::to_string(m["total"].i64()) + " 項");
            for (const auto& item : m["items"].items())
                note(item["convId"].str() + " · " + item["kind"].str() + " · " + item["text"].str());
            note("用 /chat 選擇對話後處理；核准與提問不會因已讀而略過。");
            showInbox_ = false;
        }
    } else if (type == "quality.state") {
        if (m["id"].str() == current_) {
            note(m["error"].str(m["result"]["note"].str("驗證操作已完成")));
            if (!m["result"]["output"].str().empty()) note(m["result"]["output"].str());
            if (!m["result"]["url"].str().empty()) note(m["result"]["url"].str());
        }
    } else if (type == "worktree.result") {
        if (m["id"].str() == current_ && m["result"]["ok"].boolean()) {
            note(m["result"].dump());
            if (m["action"].str() == "worktree.check") {
                mergePreview_ = m["result"];
                mergeConv_ = current_;
            }
        }
    } else if (type == "settings") {
        settings_ = m["settings"];
        tui::setTheme(settings_["theme"].str("cca"));
        if (m.has("groupOrder")) {
            groupOrder_ = m["groupOrder"];
            arrangeConversations();
        }
        if (m.has("badges")) badges_ = m["badges"];
        if (m.has("personas")) personas_ = m["personas"];
    } else if (type == "theme.result") {
        toast(m["ok"].boolean() ? "介面主題已保存" : m["error"].str(), m["ok"].boolean() ? "ok" : "error");
    } else if (type == "routing.result") {
        toast(m["ok"].boolean() ? "自動選擇設定已保存" : m["error"].str(), m["ok"].boolean() ? "ok" : "error");
    } else if (type == "routing.preview") {
        const auto& r = m["result"];
        note(r["category"].str() + " · " + r["modelName"].str() + " · " + r["effort"].str("後端預設"));
        note(r["reason"].str());
    } else if (type == "badges.result") {
        toast(m["ok"].boolean() ? "徽章顯示設定已儲存" : m["error"].str(), m["ok"].boolean() ? "ok" : "error");
    } else if (type == "groups.order.result") {
        toast(m["ok"].boolean() ? "群組順序已儲存" : m["error"].str(), m["ok"].boolean() ? "ok" : "error");
    } else if (type == "extensions") {
        extensions_ = m;
    } else if (type == "models") {
        models_ = m["models"];
    } else if (type == "usage") {
        usage_ = m["usage"];
    } else if (type == "platform.result") {
        const Json& result = m["result"];
        toast(result["ok"].boolean() ? "工具設定操作完成" : result["error"].str("工具設定操作失敗"),
              result["ok"].boolean() ? "ok" : "error");
        if (m["action"].str() == "sandbox.status")
            note(result["ok"].boolean() ? "Docker 隔離環境可用" : "Docker 隔離環境無法使用");
        for (const auto& model : result["models"].items()) note(model["id"].str());
        const std::string action = m["action"].str();
        if (action == "toolserver.test" && result["ok"].boolean()) toolHashes_[m["id"].str()] = result["hashes"];
        static const std::map<std::string, std::string> pinLabels = {{"ok", "與釘住時相同"},
                                                                     {"changed", "描述已變更，已阻擋"},
                                                                     {"unpinned", "尚未釘住"},
                                                                     {"missing", "伺服器已不提供"},
                                                                     {"unlisted", "不在核准清單"}};
        for (const auto& tool : result["tools"].items()) {
            const std::string name = tool["name"].str();
            const std::string state = result["pinState"][name].str("unlisted");
            note(name + " · " + pinLabels.at(pinLabels.count(state) ? state : "unlisted") + " · SHA-256 " +
                 result["hashes"][name].str().substr(0, 16));
            if (action == "toolserver.test")
                note("  " + util::truncateUtf8(tool["description"].str("（沒有描述）"), 300));
        }
        if (action == "toolserver.pin") {
            for (const auto& name : result["rejected"].items())
                note("沒有釘住 " + name.str() + "：檢視後描述又變了，請重新 /toolserver-test");
        }
    } else if (type == "queue.result") {
        const auto token = m["requestToken"].str();
        auto pending = queuedDrafts_.find(token);
        if (!m["ok"].boolean()) {
            if (pending != queuedDrafts_.end() && m["id"].str() == current_ && ed_.utf8().empty())
                ed_.set(pending->second);
            else if (pending != queuedDrafts_.end()) note("未送出的指令：" + pending->second);
            toast(m["error"].str("佇列操作失敗"), "error");
        } else {
            note(m["armed"].boolean() ? "待送佇列已啟用" : "待送佇列已暫停");
            for (const auto& item : m["queue"].items())
                note(item["id"].str() + " · " + item["status"].str() + " · " +
                     util::truncateUtf8(item["text"].str(), 200));
        }
        if (pending != queuedDrafts_.end()) queuedDrafts_.erase(pending);
    } else if (type == "context") {
        note("CCA 實際交付紀錄；外部 CLI 記憶與內部截斷未知。");
        for (const auto& item : m["items"].items()) {
            note(item["agent"].str() + " · " + item["status"].str() + " · 檢查點 " + item["snapshot"].str("未知"));
            note("規則：" + item["instructions"]["text"].str());
            note("交付文字：" + item["input"]["text"].str());
            if (item["input"]["truncated"].boolean())
                note("顯示紀錄已截短，原始 bytes：" + std::to_string(item["input"]["bytes"].i64()));
        }
    } else if (type == "git.state") {
        if (m["id"].str() != current_) return;
        const bool fresh = gitState_["id"].str() != m["id"].str() || !gitState_["ok"].boolean();
        gitState_ = m;
        if (fresh || (m["last"]["op"].str() == "commit" && m["last"]["ok"].boolean())) gitSeeded_ = false;
        if (!gitSeeded_) {
            gitSel_.clear();
            for (const auto& f : m["files"].items())
                if (f["selected"].boolean()) gitSel_.insert(f["path"].str());
            gitSeeded_ = true;
        }
        if (gitAsked_) showGit();
        gitAsked_ = false;
        // The sign-in state is printed when it changes, so the second answer of /git (after the remote check) shows.
        const auto seen = m["id"].str() + ":" + m["auth"]["state"].str();
        if (seen != gitAuthSeen_) {
            gitAuthSeen_ = seen;
            showGitAuth();
        }
    } else if (type == "git.result") {
        if (m["id"].str() != current_) return;
        for (const auto& s : m["result"]["secrets"].items())
            note("疑似金鑰：" + s["path"].str() + " 第 " + std::to_string(s["line"].i64()) + " 行 · " + s["kind"].str(),
                 C::kErr, "!");
        if (m["result"]["secrets"].size())
            note("已取消暫存。確認不是金鑰時用 /git commit! 訊息 仍要提交。", C::kWarn, "!");
    } else if (type == "update.state") {
        const auto status = m["status"].str();
        const bool notable = status == "available" || status == "scheduled" || status == "waiting" || status == "error";
        if (!notable && (!updateAsked_ || status == "checking")) return;
        updateAsked_ = false;
        const std::map<std::string, std::string> label = {{"unchecked", "尚未掃描"},
                                                          {"empty", "資料夾中沒有安裝包"},
                                                          {"current", "安裝包都不比目前版本新"},
                                                          {"available", "有較新的安裝包"},
                                                          {"waiting", "等待工作結束後自動更新"},
                                                          {"scheduled", "10 秒後自動更新並重新開啟"},
                                                          {"downloading", "核對並準備安裝包"},
                                                          {"error", "更新未完成"}};
        note("更新 · 本機 " + m["current"].str() + " · " + (label.count(status) ? label.at(status) : status));
        note("本機更新資料夾：" + m["folder"].str() + "（遠端發行站未啟用）");
        if (m["available"].boolean())
            note("安裝包版本 " + m["version"].str() + "；/update install 立即安裝" +
                 (status == "scheduled" ? "，/update cancel 取消這次自動更新。" : "。"));
        // Special editions and the way back are offered, never installed automatically.
        for (const auto& choice : m["choices"].items())
            note((choice["kind"].str() == "return" ? "可退回普通版本 "
                                                   : "可切換到 " + choice["editionName"].str() + " ") +
                 choice["version"].str() + "：" +
                 (choice["kind"].str() == "return" ? "/update return" : "/update switch " + choice["edition"].str()));
        updateChoices_ = m["choices"];
        for (const auto& apk : m["android"].items())
            note(apk["editionName"].str() + " " + apk["version"].str() + " 只能在 Android 安裝：把 " +
                 apk["path"].str() + " 傳到手機開啟");
        if (!m["error"].str().empty()) toast(m["error"].str(), "error");
    } else if (type == "task.history") {
        for (const auto& item : m["items"].items()) {
            note("#" + std::to_string(item["seq"].i64()) + " " + item["agent_id"].str() + " · " +
                 item["verdict"].str());
            note(item["summary"].str());
        }
    } else if (type == "review") {
        // The desktop review pane in text form: files and hunk counts; line comments and single-hunk
        // reverts need the diff viewer, so the CLI offers whole-file /revert only.
        if (m["id"].str() != current_) return;
        if (!m["error"].str().empty()) {
            toast(m["error"].str(), "error");
            return;
        }
        reviewBase_ = m["base"].str();
        reviewHead_ = m["head"].str();
        reviewConv_ = current_;
        static const std::map<std::string, std::string> scopes = {
            {"turn", "這一輪"}, {"task", "這次任務"}, {"all", "整個對話"}};
        auto scope = scopes.find(m["scope"].str());
        long long added = 0, removed = 0;
        for (const auto& f : m["files"].items()) {
            added += f["added"].i64();
            removed += f["removed"].i64();
        }
        note("審查變更（" + (scope == scopes.end() ? std::string("這次任務") : scope->second) + "）：" +
                 std::to_string(m["files"].size()) + " 個檔案 +" + std::to_string(added) + " −" +
                 std::to_string(removed),
             C::kBrand, "⇄");
        static const std::map<std::string, std::string> kinds = {
            {"add", "新增"}, {"delete", "刪除"}, {"update", "修改"}, {"binary", "二進位"}};
        for (const auto& f : m["files"].items()) {
            auto kind = kinds.find(f["change"].str());
            note((kind == kinds.end() ? std::string("修改") : kind->second) + " " + f["path"].str() + "  +" +
                 std::to_string(f["added"].i64()) + " −" + std::to_string(f["removed"].i64()) + " · " +
                 std::to_string(f["hunks"].size()) + " 段");
        }
        if (m["files"].size() > 0)
            note("用 /revert 檔案路徑 把單一檔案放回檢查點的版本；逐段還原與逐行意見請用桌面版。");
    } else if (type == "review.reverted") {
        if (m["id"].str() != current_) return;
        if (m["ok"].boolean()) {
            revertBackup_ = m["backup"].str();
            toast("已還原 " + m["path"].str() + "。還原前已存檢查點，用 /undo-revert 可以復原", "ok");
        } else toast(m["error"].str("無法還原"), "error");
    } else if (type == "toast") {
        toast(m["text"].str(), m["level"].str());
    }
}

// ------------------------------------------------------------ conversations

const Json* Cli::conv(const std::string& id) const {
    for (const auto& c : convs_.items()) {
        if (c["id"].str() == id) return &c;
    }
    return nullptr;
}

const Json& Cli::state() const { return states_[current_]; }

std::string Cli::name(const std::string& who) const {
    std::string n = util::trim(settings_["profiles"][who]["name"].str());
    if (!n.empty()) return n;
    if (who == "user") return winUser_.empty() ? "你" : winUser_;
    if (who == "local1" || who == "local2") {
        if (const Json* p = persona(who)) return (*p)["name"].str();
        return who == "local1" ? "本地模型 1" : "本地模型 2";
    }
    return who == "claude" ? "Claude" : who == "codex" ? "GPT" : who == "gemini" ? "Gemini" : who;
}

std::string Cli::convTitle(const Json& c) const {
    return c["kind"].str() == "group" ? c["name"].str() : name(c["kind"].str());
}

const Json* Cli::persona(const std::string& who) const {
    std::string id = settings_["persona"][who].str();
    for (const auto& p : personas_.items()) {
        if (p["id"].str() == id) return &p;
    }
    return nullptr;
}

void Cli::open(const std::string& id) {
    if (id.empty()) return;
    current_ = id;
    msgs_.clear();
    historyPending_ = true;
    doneMsgs_ = partial_ = 0;
    unread_.erase(id);
    widgetId_.clear();
    ui(J({{"type", "conv.open"}, {"id", id}}));
    if (const Json* c = conv()) SetConsoleTitleW(util::widen(std::string(CCA_PRODUCT) + " — " + convTitle(*c)).c_str());
}

// ------------------------------------------------------------ output

void Cli::commit(const std::vector<Line>& lines) {
    int cols = std::max(term::cols() - 1, 20);
    for (const auto& l : lines) pendingOut_ += tui::ansi(tui::clip(l, cols)) + "\r\n";
    dirty_ = true;
}

void Cli::note(const std::string& text, uint32_t c, const std::string& mark) {
    int cols = std::max(term::cols() - 1, 20);
    commit(tui::wrap({{c == C::kMuted ? tui::muted() : tui::fg(c), text}}, cols, {{tui::fg(c), "  " + mark + " "}}, L("    ")));
}

void Cli::printHeader() {
    int cols = std::max(term::cols() - 1, 20);
    std::vector<Line> art = tui::logo();
    const int artWidth = art.empty() ? 0 : tui::width(art.front());
    std::vector<Line> text;
    text.push_back(
        {{tui::fg(C::kBrand, true), std::string(CCA_PRODUCT) + (*CCA_EDITION_NAME ? " " CCA_EDITION_NAME : "")},
         {tui::muted(), "  CLI " + version_}});
    text.push_back({});
    auto who = [&](const std::string& a, const std::string& cli, const std::string& role) {
        Line l = {{tui::fg(tui::agentColor(a)), "● "}, {tui::fg(tui::agentColor(a), true), name(a)}, {tui::muted(), "  " + cli + " · " + role}};
        if (const Json* p = persona(a)) {
            if ((*p)["name"].str() != name(a)) l.push_back({tui::faint(), " · 性格 " + (*p)["name"].str()});
        }
        return l;
    };
    text.push_back(who("claude", "Claude Code", "可切換主刀"));
    text.push_back(who("codex", "Codex", "可接手工作"));
    text.push_back(who("gemini", "Gemini CLI", "多代理審查"));
    text.push_back({{tui::muted(), "你是 "}, {Style(), name("user")}, {tui::muted(), " · 資料在 " + shortPath(util::narrow(util::appDataDir()))}});
    text.push_back({{tui::faint(), "/help 看指令 · Esc 中斷 · Ctrl+C 離開"}});
    std::vector<Line> out;
    out.push_back({});
    for (size_t i = 0; i < std::max(art.size(), text.size()); ++i) {
        Line l = L("  ");
        l = l + (i < art.size() ? art[i] : L(std::string(artWidth, ' ')));
        l = l + L("   ");
        if (i < text.size()) l = l + text[i];
        out.push_back(tui::clip(l, cols));
    }
    commit(out);
}

void Cli::printHistory() {
    int cols = std::max(term::cols() - 1, 20);
    const Json* c = conv();
    if (!c) return;
    bool group = (*c)["kind"].str() == "group";
    std::string title = (group ? "# " : "@ ") + convTitle(*c);
    int side = std::max(2, (cols - tui::width(title) - 2) / 2);
    std::vector<Line> out = {{}, {{tui::faint(), repeat("─", side) + " "}, {tui::fg(C::kBrand, true), title}, {tui::faint(), " " + repeat("─", side)}}};
    std::string cwd = (*c)["cwd"].str();
    Line where = {{tui::muted(), "  資料夾 "}, {Style(), cwd.empty() ? "還沒選" : shortPath(cwd)}};
    if (cwd.empty()) where.push_back({tui::muted(), "（/cd 路徑，或 /cd . 用目前資料夾）"});
    out.push_back(where);
    commit(out);

    const size_t kShow = 12;
    size_t first = msgs_.size() > kShow ? msgs_.size() - kShow : 0;
    if (first > 0) note("前面還有 " + std::to_string(first) + " 則訊息（桌面版可以看完整紀錄）");
    tui::Names names{[this](const std::string& w) { return name(w); }};
    doneMsgs_ = first;
    partial_ = 0;
    for (; doneMsgs_ < msgs_.size(); ++doneMsgs_) {
        if (msgs_[doneMsgs_]["streaming"].boolean()) break;
        commit(tui::renderMessage(msgs_[doneMsgs_], names, cols).lines);
    }
    if (msgs_.empty()) {
        note(group ? "在下面輸入指令。/lead 選擇主刀，/takeover 由 GPT 接續工作"
                   : "在下面輸入訊息，開始和 " + name((*c)["kind"].str()) + " 對話");
    }
    if (state()["task"]["status"].str() == "interrupted") {
        note("上次的協作任務在進行中被關閉了（" + std::to_string(state()["task"]["count"].i64()) +
                 " 次交接已保存）。輸入 /resume 接續，下一位會先檢查實際檔案再繼續。",
             C::kWarn, "!");
    }
    // A conversation started from a folder works in that folder, unless it already has one.
    std::string here = util::narrow(launchDir_);
    if (cwd.empty() && !here.empty() && util::projectFolderProblem(launchDir_).empty()) {
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"cwd", here}})}}));
        note("專案資料夾設為目前的資料夾 " + shortPath(here), C::kBrand, "✓");
    } else if (!cwd.empty() && toLowerAscii(cwd) != toLowerAscii(here) && util::projectFolderProblem(launchDir_).empty()) {
        note("你在 " + shortPath(here) + " 開啟，這個對話用的是 " + shortPath(cwd) + "。要換成目前資料夾請輸入 /cd .");
    }
}

std::vector<Line> Cli::box(const std::string& title, uint32_t c, const std::vector<Line>& body, const std::string& footer, int cols) const {
    Style b = tui::fg(c);
    std::vector<Line> out;
    auto edge = [&](const char* l, const char* r, const std::string& label, bool muted) {
        if (label.empty()) return Line{{b, l + repeat("─", cols - 2) + r}};
        std::string lab = tui::clipText(label, cols - 6);
        int rest = cols - 4 - tui::width(lab) - 1;
        return Line{{b, std::string(l) + "─ "}, {muted ? tui::muted() : tui::fg(c, true), lab}, {b, " " + repeat("─", std::max(rest, 0)) + r}};
    };
    out.push_back(edge("╭", "╮", title, false));
    for (const auto& l : body) out.push_back(Line{{b, "│ "}} + tui::padTo(l, cols - 4) + Line{{b, " │"}});
    out.push_back(edge("╰", "╯", footer, true));
    return out;
}

std::vector<Line> Cli::inputBox(int cols, int* caretRow, int* caretCol) {
    int inner = cols - 4, textW = std::max(inner - 2, 8);
    std::vector<Line> body;
    std::vector<std::u32string> rows(1);
    int row = 0, col = 0, cr = 0, cc = 0;
    for (size_t i = 0; i <= ed_.text.size(); ++i) {
        if (i == ed_.caret) { cr = row; cc = col; }
        if (i == ed_.text.size()) break;
        char32_t ch = ed_.text[i];
        if (ch == '\n') { rows.emplace_back(); ++row; col = 0; continue; }
        int w = tui::charWidth(ch);
        if (col + w > textW) { rows.emplace_back(); ++row; col = 0; if (i == ed_.caret) { cr = row; cc = 0; } }
        rows[row] += ch;
        col += w;
    }
    if (cc >= textW) { cc = 0; ++cr; if (cr >= (int)rows.size()) rows.emplace_back(); }
    std::string placeholder;
    if (ask_) placeholder = ask_->placeholder;
    else if (const Json* c = conv()) {
        bool group = (*c)["kind"].str() == "group";
        if (state()["group"]["active"].boolean()) placeholder = "協作進行中，可以先打字，或 /stop 停止";
        else if (running()) placeholder = "等 " + convTitle(*c) + " 回覆中，可以先打字";
        else placeholder = group ? "給 " + name("claude") + " 和 " + name("codex") + " 的指令" : "傳訊息給 " + name((*c)["kind"].str());
    }
    for (size_t r = 0; r < rows.size(); ++r) {
        Line l = r == 0 ? Line{{tui::fg(C::kBrand, true), "› "}} : L("  ");
        if (ed_.empty() && r == 0) l.push_back({tui::faint(), placeholder});
        else l.push_back({Style(), tui::encode(rows[r])});
        body.push_back(l);
    }
    // Long input scrolls inside the box instead of pushing the screen around.
    const int kMaxRows = 8;
    int top = 0;
    if ((int)body.size() > kMaxRows) {
        top = std::clamp(cr - kMaxRows + 1, 0, (int)body.size() - kMaxRows);
        body = std::vector<Line>(body.begin() + top, body.begin() + top + kMaxRows);
    }
    std::string title = ask_ ? ask_->title : "";
    std::string footer = ask_ ? (ask_->allowEmpty ? "Enter 確定（可以留空） · Shift+Enter 換行 · Esc 取消" : "Enter 確定 · Shift+Enter 換行 · Esc 取消") : "";
    std::vector<Line> out = box(title, ask_ ? C::kWarn : C::kBrand, body, footer, cols);
    *caretRow = 1 + (cr - top);
    *caretCol = 2 + 2 + cc;
    return out;
}

std::vector<Line> Cli::pickerBox(int cols) {
    Picker& p = *picker_;
    std::vector<int> vis = p.visible();
    if (vis.empty()) p.sel = 0;
    else p.sel = std::clamp(p.sel, 0, (int)vis.size() - 1);
    std::vector<Line> body;
    if (!p.filter.empty()) body.push_back({{tui::muted(), "篩選："}, {Style(), tui::encode(p.filter)}});
    const int kMax = 10;
    int top = std::clamp(p.sel - kMax / 2, 0, std::max(0, (int)vis.size() - kMax));
    if (top > 0) body.push_back(L("  ↑ 還有 " + std::to_string(top) + " 項", tui::faint()));
    int labelW = 0;
    for (int i : vis) labelW = std::max(labelW, tui::width(p.items[i].label));
    labelW = std::min(labelW, (cols - 8) / 2);
    for (int k = top; k < (int)vis.size() && k < top + kMax; ++k) {
        const Item& it = p.items[vis[k]];
        bool sel = k == p.sel;
        Line l = {{sel ? tui::fg(C::kBrand, true) : Style(), sel ? "❯ " : "  "}};
        if (it.checkable) l.push_back({it.checked ? tui::fg(C::kOk) : tui::faint(), it.checked ? "■ " : "□ "});
        Style ls = it.color ? tui::fg(it.color, sel) : sel ? tui::fg(C::kBrand, true) : Style();
        Line lab = tui::padTo({{ls, it.label}}, labelW);
        l = l + lab;
        if (!it.hint.empty()) l.push_back({tui::muted(), "  " + it.hint});
        body.push_back(l);
    }
    int below = (int)vis.size() - (top + kMax);
    if (below > 0) body.push_back(L("  ↓ 還有 " + std::to_string(below) + " 項", tui::faint()));
    if (vis.empty()) body.push_back(L("沒有符合的項目", tui::muted()));
    return box(p.title, C::kBrand, body, p.footer + (p.items.size() > 6 ? " · 打字篩選" : ""), cols);
}

const Json* Cli::pendingBlock(std::string* msgId) const {
    for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
        for (const auto& b : (*it)["blocks"].items()) {
            std::string t = b["type"].str();
            if ((t == "approval" || t == "question") && b["state"].str() == "pending") {
                if (msgId) *msgId = (*it)["id"].str();
                return &b;
            }
        }
    }
    return nullptr;
}

std::vector<Line> Cli::widgetBox(int cols) {
    std::string msgId;
    const Json* b = pendingBlock(&msgId);
    if (!b) {
        widgetId_.clear();
        return {};
    }
    if ((*b)["id"].str() != widgetId_) {
        widgetId_ = (*b)["id"].str();
        widgetSel_ = 0;
        questionAt_ = 0;
        answers_ = Json::object();
    }
    tui::Names names{[this](const std::string& w) { return name(w); }};
    int inner = cols - 4;
    std::vector<Line> body;
    auto add = [&](const Line& content, const Line& prefix = {}) {
        for (auto& l : tui::wrap(content, inner, prefix, prefix)) body.push_back(l);
    };
    std::string agent = (*b)["agent"].str() == "codex" ? "codex" : "claude";
    if ((*b)["type"].str() == "approval") {
        if (!(*b)["command"].str().empty()) {
            auto lines = util::splitLines((*b)["command"].str());
            for (size_t i = 0; i < lines.size() && i < 6; ++i) add({{tui::fg(C::kCode), lines[i]}}, {{tui::faint(), i ? "  " : "$ "}});
            if (lines.size() > 6) body.push_back(L("  …（共 " + std::to_string(lines.size()) + " 行）", tui::faint()));
            if (!(*b)["cwd"].str().empty()) add({{tui::muted(), "在 " + (*b)["cwd"].str()}});
        }
        if (!(*b)["path"].str().empty()) {
            Line l = {{tui::fg(C::kCode), (*b)["path"].str()}};
            if ((*b).has("added")) l = l + Line{{tui::fg(C::kOk), "  +" + std::to_string((*b)["added"].i64())}, {tui::fg(C::kErr), " −" + std::to_string((*b)["removed"].i64())}};
            add(l);
        }
        for (const auto& f : (*b)["files"].items()) {
            add({{tui::fg(C::kCode), f["path"].str()}, {tui::fg(C::kOk), "  +" + std::to_string(f["added"].i64())}, {tui::fg(C::kErr), " −" + std::to_string(f["removed"].i64())}});
        }
        if (!(*b)["diff"].str().empty()) {
            auto lines = util::splitLines((*b)["diff"].str());
            int shown = 0;
            for (const auto& l : lines) {
                if (util::startsWith(l, "@@")) continue;
                if (++shown > 8) break;
                Style st = util::startsWith(l, "+") ? tui::fg(C::kOk) : util::startsWith(l, "-") ? tui::fg(C::kErr) : tui::muted();
                body.push_back({{st, tui::clipText(l, inner)}});
            }
            if ((int)lines.size() > shown + 1) body.push_back(L("…（/show 看完整的 diff）", tui::faint()));
        }
        if (!(*b)["plan"].str().empty()) {
            auto md = tui::markdown((*b)["plan"].str(), inner, {});
            for (size_t i = 0; i < md.size() && i < 14; ++i) body.push_back(md[i].line);
            if (md.size() > 14) body.push_back(L("…（/show 看完整的計畫）", tui::faint()));
        }
        if (!(*b)["detail"].str().empty() && (*b)["command"].str().empty()) add({{tui::fg(C::kCode), (*b)["detail"].str()}});
        if (!(*b)["reason"].str().empty()) add({{tui::muted(), "原因：" + (*b)["reason"].str()}});
        if (!(*b)["note"].str().empty()) add({{tui::muted(), (*b)["note"].str()}});
        if (!(*b)["blockedPath"].str().empty()) add({{tui::fg(C::kWarn), "涉及專案外的路徑：" + (*b)["blockedPath"].str()}});
        body.push_back({});
        const auto& opts = (*b)["options"].items();
        widgetSel_ = std::clamp(widgetSel_, 0, std::max(0, (int)opts.size() - 1));
        for (int i = 0; i < (int)opts.size(); ++i) {
            bool sel = i == widgetSel_;
            std::string style = opts[i]["style"].str();
            uint32_t c = style == "danger" ? C::kErr : style == "primary" || style == "success" ? C::kOk : 0;
            body.push_back({{sel ? tui::fg(C::kBrand, true) : Style(), sel ? "❯ " : "  "},
                            {sel ? tui::fg(c ? c : C::kBrand, true) : Style(), std::to_string(i + 1) + ". " + opts[i]["label"].str()}});
        }
        return box(tui::withAgentName((*b)["title"].str("權限請求"), names), C::kWarn, body,
                   "↑↓ 或數字選擇 · Enter 確定 · Esc 拒絕", cols);
    }
    // A question with options; "其他" lets the user type an answer.
    const auto& qs = (*b)["questions"].items();
    if (qs.empty()) return {};
    questionAt_ = std::clamp(questionAt_, 0, (int)qs.size() - 1);
    const Json& q = qs[questionAt_];
    if (!q["header"].str().empty()) body.push_back({{tui::fg(C::kBrand, true), q["header"].str()}});
    add({{Style(), q["question"].str()}});
    body.push_back({});
    std::string key = q["id"].str().empty() ? q["question"].str() : q["id"].str();
    const auto& opts = q["options"].items();
    int total = (int)opts.size() + 1 + (q["multiSelect"].boolean() ? 1 : 0);
    widgetSel_ = std::clamp(widgetSel_, 0, total - 1);
    for (int i = 0; i < total; ++i) {
        bool sel = i == widgetSel_;
        Line l = {{sel ? tui::fg(C::kBrand, true) : Style(), sel ? "❯ " : "  "}};
        if (i < (int)opts.size()) {
            std::string label = opts[i]["label"].str();
            if (q["multiSelect"].boolean()) {
                bool on = false;
                for (const auto& v : answers_[key].items()) on |= v.str() == label;
                l.push_back({on ? tui::fg(C::kOk) : tui::faint(), on ? "■ " : "□ "});
            }
            l.push_back({sel ? tui::fg(C::kBrand, true) : Style(), std::to_string(i + 1) + ". " + label});
            if (!opts[i]["description"].str().empty()) l.push_back({tui::muted(), "  " + opts[i]["description"].str()});
        } else if (i == (int)opts.size()) {
            l.push_back({sel ? tui::fg(C::kBrand, true) : tui::muted(), "✎ 其他（自己輸入）"});
        } else {
            l.push_back({sel ? tui::fg(C::kOk, true) : tui::fg(C::kOk), "✓ 完成選擇"});
        }
        for (auto& w : tui::wrap(l, inner, {}, L("     "))) body.push_back(w);
    }
    std::string title = name(agent) + " 想問你" + (qs.size() > 1 ? "（" + std::to_string(questionAt_ + 1) + "/" + std::to_string(qs.size()) + "）" : "");
    return box(title, C::kBrand, body, q["multiSelect"].boolean() ? "Enter 勾選 · 選「完成選擇」送出" : "↑↓ 或數字選擇 · Enter 確定", cols);
}

std::vector<Line> Cli::statusLines(int cols) {
    std::vector<Line> out;
    const Json& st = state();
    long long now = util::nowMs();
    std::string spin = motionOn() ? kSpinner[(now / 110) % 10] : "·";
    if (st["preparing"].str() == "guard") {
        out.push_back({{tui::fg(C::kBrand), spin + " "}, {tui::muted(), "正在執行測試，檢查審查者的修改"}});
    } else if (!st["preparing"].str().empty()) {
        out.push_back({{tui::fg(C::kBrand), spin + " "}, {tui::muted(), "正在建立檢查點，完成後 " + name(st["preparing"].str()) + " 就會開始"}});
    }
    for (const auto& t : st["typing"].items()) {
        std::string who = t.str();
        const Json* msg = nullptr;
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
            if ((*it)["author"].str() == who && (*it)["streaming"].boolean()) { msg = &*it; break; }
        }
        std::string action = who == "local1" || who == "local2" ? "唯讀審查中" : "回覆中";
        if (st["thinking"][who].boolean()) action = "思考中";
        if (msg) {
            for (const auto& b : (*msg)["blocks"].items()) {
                if (b["type"].str() == "tool" && b["status"].str() == "running") action = b["title"].str("使用工具") + "中";
                if ((b["type"].str() == "approval" || b["type"].str() == "question") && b["state"].str() == "pending") action = "等你決定";
            }
        }
        uint32_t c = tui::agentColor(who);
        Line l = {{tui::fg(c), spin + " "}, {tui::fg(c, true), name(who)}, {Style(), " " + action + "…"}};
        long long elapsed = msg ? now - (*msg)["ts"].i64() : 0;
        if (msg && (*msg)["ts"].i64() > 0 && elapsed >= 1000) {
            l.push_back({tui::muted(), "  " + (elapsed < 60000 ? std::to_string(elapsed / 1000) + " 秒" : tui::duration(elapsed))});
        }
        if (msg && (*msg)["retry"].isObject()) l.push_back({tui::fg(C::kWarn), "  連線不穩，重試中"});
        l.push_back({tui::faint(), " · Esc 中斷"});
        out.push_back(tui::clip(l, cols));
    }
    const Json& g = st["group"];
    if (g["active"].boolean()) {
        if (g["paused"].boolean()) {
            out.push_back(
                tui::clip({{tui::fg(C::kWarn), "‖ 協作暫停"},
                           {tui::muted(), g["pauseReason"].str().empty() ? "" : "：" + g["pauseReason"].str()},
                           {tui::faint(), g["pauseKind"].str() == "guard"
                                              ? " · /continue 再退回一次 · /accept 接受並繼續 · /end 結束"
                                              : " · /continue 繼續 · /end 結束"}},
                          cols));
        } else if (settings_["visual"]["mode"].str("auto") == "off") {
            int files = (int)g["files"].fields().size();
            Line l = {{tui::fg(C::kBrand), "⇄ 協作"}, {tui::muted(), " 第 " + std::to_string(g["round"].i64()) + "/" + std::to_string(g["maxRounds"].i64()) + " 輪"}};
            if (files) l.push_back({tui::muted(), " · " + std::to_string(files) + " 個檔案有變更"});
            if (!g["localActive"].str().empty()) l.push_back({tui::muted(), " · " + name(g["localActive"].str()) + " 唯讀審查中"});
            out.push_back(tui::clip(l, cols));
        }
    }
    // The latest to-do list of a running reply.
    for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
        if (!(*it)["streaming"].boolean()) continue;
        for (const auto& b : (*it)["blocks"].items()) {
            if (b["type"].str() != "todo") continue;
            int done = 0;
            for (const auto& x : b["items"].items()) done += x["status"].str() == "completed";
            out.push_back({{tui::faint(), "  ⎿ "}, {Style(), "待辦 "}, {tui::muted(), std::to_string(done) + "/" + std::to_string(b["items"].size())}});
            int shown = 0;
            for (const auto& x : b["items"].items()) {
                if (++shown > 6) break;
                std::string s = x["status"].str();
                Style st2 = s == "completed" ? tui::muted() : s == "inProgress" ? tui::fg(C::kBrand, true) : Style();
                if (s == "completed") st2.strike = true;
                out.push_back(tui::clip({{tui::faint(), "    "}, {s == "inProgress" ? tui::fg(C::kBrand) : tui::muted(), s == "completed" ? "☒ " : s == "inProgress" ? "◐ " : "☐ "},
                                         {st2, x["text"].str()}}, cols));
            }
        }
        break;
    }
    return out;
}

Line Cli::footerLine(int cols) {
    Line l = L("  ");
    Line right = {{tui::faint(), "/help"}};
    if (const Json* c = conv()) {
        bool group = (*c)["kind"].str() == "group";
        l.push_back({tui::fg(group ? C::kBrand : tui::agentColor((*c)["kind"].str()), true), (group ? "# " : "@ ") + convTitle(*c)});
        l.push_back({tui::fg(C::kBrand), " · 主刀 " + name((*c)["kind"].str() == "codex" ? "codex" : (*c)["lead"].str("claude"))});
        std::string cwd = (*c)["cwd"].str();
        if (!(*c)["daily"].boolean() || group) l.push_back({tui::muted(), " · " + (cwd.empty() ? std::string("沒有專案資料夾") : shortPath(cwd))});
        std::string perm = (*c)["permission"].str();
        for (const auto& p : kPerms) {
            if (perm == p.id) right = {{perm == "bypass" ? tui::fg(C::kErr, true) : tui::muted(), p.label}, {tui::faint(), " · /help"}};
        }
        if ((*c)["daily"].boolean() && !group) l.push_back({tui::fg(C::kBrand), " · 日常模式"});
    }
    if (!unread_.empty()) l.push_back({tui::fg(C::kWarn), " · 其他對話有新回覆（/chat）"});
    l = tui::clip(l, std::max(6, cols - tui::width(right) - 2));
    int gap = cols - tui::width(l) - tui::width(right);
    if (gap < 2) return tui::clip(l, cols);
    return l + L(std::string(gap, ' ')) + right;
}

std::vector<Line> Cli::buildLive(int cols, int rows, int* caretRow, int* caretCol) {
    std::vector<Line> bottom;
    std::vector<Line> widget = widgetBox(cols);
    for (auto& l : widget) bottom.push_back(l);
    for (auto& l : statusLines(cols)) bottom.push_back(l);
    if (!toast_.empty() && util::nowMs() < toastUntil_) {
        uint32_t c = toastLevel_ == "error" ? C::kErr : toastLevel_ == "ok" ? C::kOk : C::kBrand;
        for (auto& l : tui::wrap({{tui::fg(c), toast_}}, cols, {{tui::fg(c), toastLevel_ == "error" ? "✗ " : toastLevel_ == "ok" ? "✓ " : "· "}}, L("  "))) bottom.push_back(l);
    }
    int cr = -1, cc = 0;
    std::vector<Line> input;
    if (picker_) {
        input = pickerBox(cols);
    } else {
        input = inputBox(cols, &cr, &cc);
        std::vector<const Command*> ms = matches();
        if (!ms.empty() && !ask_) {
            popupSel_ = std::clamp(popupSel_, 0, (int)ms.size() - 1);
            int top = std::clamp(popupSel_ - 3, 0, std::max(0, (int)ms.size() - 7));
            for (int i = top; i < (int)ms.size() && i < top + 7; ++i) {
                bool sel = i == popupSel_;
                std::string head = std::string(ms[i]->name) + (*ms[i]->args ? std::string(" ") + ms[i]->args : "");
                Line l = {{sel ? tui::fg(C::kBrand, true) : tui::muted(), sel ? "  ❯ " : "    "}, {sel ? tui::fg(C::kBrand, true) : Style(), head}};
                l.push_back({Style(), std::string(std::max(1, 22 - tui::width(head)), ' ')});
                l.push_back({tui::muted(), ms[i]->desc});
                input.push_back(tui::clip(l, cols));
            }
        }
    }
    // The diagram yields its space to approvals, questions and the input editor.
    if (!picker_ && !ask_ && settings_["visual"]["mode"].str("auto") != "off") {
        int budget = rows - 1 - (int)bottom.size() - (int)input.size() - 4;
        bool compact = budget < 9 || cols < 68 || settings_["visual"]["mode"].str() == "compact";
        tui::Names names{[this](const std::string& w) { return name(w); }};
        auto flow = tui::renderWorkflow(state()["workflow"], names, cols, util::nowMs(), compact, motionOn());
        if ((int)flow.size() > budget && state()["workflow"]["layout"].str() == "pipeline" && budget >= 2) {
            flow.resize(1);
            for (const auto& node : state()["workflow"]["nodes"].items()) {
                if (node["id"] != state()["workflow"]["owner"]) continue;
                std::string label = node["profile"].str(name(node["agent"].str()));
                flow.push_back(tui::clip(L("  " + node["name"].str() + " · " + label + " · " + node["label"].str(),
                                           tui::fg(tui::agentColor(node["agent"].str()))),
                                         cols));
                break;
            }
        }
        if ((int)flow.size() <= budget) bottom.insert(bottom.begin(), flow.begin(), flow.end());
    }
    int inputTop = (int)bottom.size() + (bottom.empty() ? 1 : 0);
    if (bottom.empty()) bottom.push_back({});  // a quiet line above the box
    for (auto& l : input) bottom.push_back(l);
    bottom.push_back(footerLine(cols));

    // The part of the running reply that is not final yet, cut from the top if the screen is short.
    std::vector<Line> live;
    tui::Names names{[this](const std::string& w) { return name(w); }};
    std::vector<Line> pending;
    for (size_t i = doneMsgs_; i < msgs_.size(); ++i) {
        tui::Rendered r = tui::renderMessage(msgs_[i], names, i == doneMsgs_ && partial_ > 0 ? partialWidth_ : cols);
        for (size_t k = i == doneMsgs_ ? partial_ : 0; k < r.lines.size(); ++k) pending.push_back(r.lines[k]);
    }
    int room = std::max(0, rows - 1 - (int)bottom.size());
    if ((int)pending.size() > room) {
        size_t hidden = pending.size() - room + (room > 0 ? 1 : 0);
        std::vector<Line> cut;
        if (room > 0) cut.push_back(L("  ⋮ 上面還有 " + std::to_string(hidden) + " 行，完成後會整段印出", tui::faint()));
        for (size_t k = hidden; k < pending.size(); ++k) cut.push_back(pending[k]);
        pending.swap(cut);
    }
    for (auto& l : pending) live.push_back(tui::clip(l, cols));
    int offset = (int)live.size();
    for (auto& l : bottom) live.push_back(l);
    if ((int)live.size() > rows - 1) {
        // Even the controls do not fit: keep the bottom.
        int drop = (int)live.size() - (rows - 1);
        live.erase(live.begin(), live.begin() + drop);
        offset -= drop;
    }
    *caretRow = cr >= 0 ? offset + inputTop + cr : -1;
    *caretCol = cc;
    return live;
}

void Cli::frame() {
    int termCols = term::cols(), termRows = term::rows();
    int cols = std::max(termCols - 1, 20);
    tui::Names names{[this](const std::string& w) { return name(w); }};

    // 1. Lines that can no longer change go into the scrollback.
    if (!historyPending_) {
        while (doneMsgs_ < msgs_.size()) {
            int w = partial_ > 0 ? partialWidth_ : cols;
            tui::Rendered r = tui::renderMessage(msgs_[doneMsgs_], names, w);
            std::vector<Line> fresh;
            for (size_t i = partial_; i < r.stable && i < r.lines.size(); ++i) fresh.push_back(r.lines[i]);
            if (!fresh.empty()) {
                if (partial_ == 0) partialWidth_ = w;
                commit(fresh);
                partial_ = std::max(partial_, r.stable);
            }
            if (r.complete && partial_ >= r.lines.size()) {
                ++doneMsgs_;
                partial_ = 0;
                continue;
            }
            break;
        }
    }

    // 2. Erase what was drawn last time. A narrower window wraps those lines into more rows.
    std::string out = "\x1b[?2026h\x1b[?25l\r";
    if (!liveWidths_.empty()) {
        auto rowsOf = [&](int w) { return std::max(1, (w + termCols - 1) / std::max(termCols, 1)); };
        int up = 0;
        for (int i = 0; i < caretRow_ && i < (int)liveWidths_.size(); ++i) up += rowsOf(liveWidths_[i]);
        up += caretCol_ / std::max(termCols, 1);
        if (up > 0) out += "\x1b[" + std::to_string(up) + "A";
        out += "\x1b[J";
    }
    out += pendingOut_;
    pendingOut_.clear();

    // 3. Draw the live region and put the cursor where the user types.
    int caretRow = -1, caretCol = 0;
    std::vector<Line> live = buildLive(cols, termRows, &caretRow, &caretCol);
    liveWidths_.clear();
    for (size_t i = 0; i < live.size(); ++i) {
        out += tui::ansi(live[i]);
        liveWidths_.push_back(tui::width(live[i]));
        if (i + 1 < live.size()) out += "\r\n";
    }
    // The cursor is at the end of the last line now; without a caret (a list is open) it
    // goes to the start of that line and stays hidden.
    int last = (int)live.size() - 1;
    bool hasCaret = caretRow >= 0;
    if (!hasCaret) {
        caretRow = last;
        caretCol = 0;
    }
    if (last - caretRow > 0) out += "\x1b[" + std::to_string(last - caretRow) + "A";
    out += "\r";
    if (caretCol > 0) out += "\x1b[" + std::to_string(caretCol) + "C";
    caretRow_ = caretRow;
    caretCol_ = caretCol;
    bool showCursor = hasCaret && (ask_ || widgetId_.empty());
    out += showCursor ? "\x1b[?25h" : "";
    out += "\x1b[?2026l";
    term::write(out);
    lastCols_ = termCols;
    lastFrame_ = util::nowMs();
    dirty_ = false;
}

// ------------------------------------------------------------ input

std::vector<const Command*> Cli::matches() const {
    std::vector<const Command*> out;
    std::string t = ed_.utf8();
    if (t.empty() || t[0] != '/' || t.find(' ') != std::string::npos || t.find('\n') != std::string::npos) return out;
    for (const auto& c : kCommands) {
        if (util::startsWith(c.name, t)) out.push_back(&c);
    }
    return out;
}

void Cli::toast(const std::string& text, const std::string& level) {
    toast_ = text;
    toastLevel_ = level;
    toastUntil_ = util::nowMs() + (level == "error" ? 6000 : 4000);
    dirty_ = true;
}

void Cli::ask(Ask a, const std::string& initial) {
    askDraft_ = ed_.utf8();
    ed_.set(initial);
    ask_ = std::make_unique<Ask>(std::move(a));
    dirty_ = true;
}

void Cli::handleKey(const term::Key& k) {
    using K = term::Key;
    dirty_ = true;
    if (k.code == K::Resize) return;
    if (k.code == K::Char && k.ctrl && k.ch == 'c') {
        if (picker_) { auto cancel = picker_->onCancel; picker_.reset(); if (cancel) cancel(); return; }
        if (ask_) { auto cancel = ask_->cancel; ask_.reset(); ed_.set(askDraft_); if (cancel) cancel(); return; }
        if (!ed_.empty()) { ed_.clear(); return; }
        if (running()) { ui(J({{"type", "conv.stop"}, {"id", current_}})); toast("已要求停止"); return; }
        long long now = util::nowMs();
        if (now - lastCtrlC_ < 2000) { quit_ = true; return; }
        lastCtrlC_ = now;
        toast("再按一次 Ctrl+C 離開");
        return;
    }
    if (k.code == K::Char && k.ctrl && k.ch == 'd' && ed_.empty() && !picker_ && !ask_) { quit_ = true; return; }
    if (k.code == K::Char && k.ctrl && k.ch == 'l') { liveWidths_.clear(); term::write("\x1b[2J\x1b[H"); return; }
    if (picker_) { pickerKey(k); return; }
    if (!ask_ && ed_.empty() && widgetKey(k)) return;

    std::vector<const Command*> ms = ask_ ? std::vector<const Command*>{} : matches();
    switch (k.code) {
    case K::Enter:
        if (k.shift || k.ctrl || k.alt || k.pasted) { ed_.insert('\n'); return; }
        if (!ms.empty() && ed_.utf8() != ms[std::clamp(popupSel_, 0, (int)ms.size() - 1)]->name) {
            ed_.set(ms[std::clamp(popupSel_, 0, (int)ms.size() - 1)]->name);
        }
        submit();
        return;
    case K::Tab:
        if (!ms.empty()) { ed_.set(std::string(ms[std::clamp(popupSel_, 0, (int)ms.size() - 1)]->name) + " "); popupSel_ = 0; }
        return;
    case K::Esc:
        if (ask_) { auto cancel = ask_->cancel; ask_.reset(); ed_.set(askDraft_); if (cancel) cancel(); return; }
        if (running()) { ui(J({{"type", "conv.stop"}, {"id", current_}})); toast("已要求停止"); return; }
        ed_.clear();
        return;
    case K::Backspace: if (k.ctrl) ed_.killWord(); else ed_.backspace(); popupSel_ = 0; return;
    case K::Delete: ed_.del(); return;
    case K::Left: ed_.left(); return;
    case K::Right: ed_.right(); return;
    case K::Home: ed_.home(); return;
    case K::End: ed_.end(); return;
    case K::Up:
        if (!ms.empty()) { popupSel_ = std::max(0, popupSel_ - 1); return; }
        if (!ed_.onFirstLine()) { ed_.up(); return; }
        if (!ask_ && !history_.empty()) {
            if (histPos_ < 0) { histDraft_ = ed_.utf8(); histPos_ = (int)history_.size(); }
            if (histPos_ > 0) ed_.set(history_[--histPos_]);
        }
        return;
    case K::Down:
        if (!ms.empty()) { popupSel_ = std::min((int)ms.size() - 1, popupSel_ + 1); return; }
        if (!ed_.onLastLine()) { ed_.down(); return; }
        if (!ask_ && histPos_ >= 0) {
            if (++histPos_ >= (int)history_.size()) { histPos_ = -1; ed_.set(histDraft_); }
            else ed_.set(history_[histPos_]);
        }
        return;
    case K::Char:
        if (k.ctrl) {
            if (k.ch == 'a') ed_.home();
            else if (k.ch == 'e') ed_.end();
            else if (k.ch == 'u') ed_.killToStart();
            else if (k.ch == 'k') ed_.killToEnd();
            else if (k.ch == 'w') ed_.killWord();
            else if (k.ch == 'j') ed_.insert('\n');
            return;
        }
        ed_.insert(k.ch);
        popupSel_ = 0;
        return;
    default:
        return;
    }
}

bool Cli::widgetKey(const term::Key& k) {
    using K = term::Key;
    std::string msgId;
    const Json* b = pendingBlock(&msgId);
    if (!b || (*b)["id"].str() != widgetId_) return false;
    Json block = *b;  // a copy: answering may change msgs_
    bool approval = block["type"].str() == "approval";
    int count = 0;
    const Json* q = nullptr;
    if (approval) {
        count = (int)block["options"].size();
    } else {
        if (block["questions"].size() == 0) return false;
        q = &block["questions"].at(std::clamp(questionAt_, 0, (int)block["questions"].size() - 1));
        count = (int)(*q)["options"].size() + 1 + ((*q)["multiSelect"].boolean() ? 1 : 0);
    }
    if (k.code == K::Up) { widgetSel_ = (widgetSel_ + count - 1) % std::max(count, 1); return true; }
    if (k.code == K::Down || k.code == K::Tab) { widgetSel_ = (widgetSel_ + 1) % std::max(count, 1); return true; }
    int pick = -1;
    if (k.code == K::Enter) pick = widgetSel_;
    else if (k.code == K::Char && !k.ctrl && k.ch >= '1' && k.ch <= '9' && (int)(k.ch - '1') < count) pick = (int)(k.ch - '1');
    else if (k.code == K::Esc && approval) {
        for (int i = 0; i < count; ++i) {
            if (block["options"].at(i)["id"].str() == "deny") pick = i;
        }
        if (pick < 0) return true;
    } else {
        return false;
    }
    if (approval) {
        widgetSel_ = pick;
        choose(block, msgId, block["options"].at(pick)["id"].str());
    } else {
        answerQuestion(block, msgId, pick);
    }
    return true;
}

void Cli::choose(const Json& block, const std::string& msgId, const std::string& choice) {
    std::string blockId = block["id"].str();
    std::string agent = block["agent"].str() == "codex" ? "codex" : "claude";
    auto send = [this, msgId, blockId, choice](const std::string& feedback) {
        ui(J({{"type", "approval"}, {"convId", current_}, {"msgId", msgId}, {"blockId", blockId}, {"choice", choice}, {"feedback", feedback}}));
    };
    if (choice == "deny" || choice == "abort") {
        Ask a;
        a.title = "告訴 " + name(agent) + " 為什麼拒絕";
        a.placeholder = "（選填）原因或該怎麼做，直接按 Enter 略過";
        a.allowEmpty = true;
        a.done = send;
        ask(std::move(a));
        return;
    }
    send("");
}

void Cli::answerQuestion(const Json& block, const std::string& msgId, int option) {
    const auto& qs = block["questions"].items();
    const Json& q = qs[std::clamp(questionAt_, 0, (int)qs.size() - 1)];
    std::string key = q["id"].str().empty() ? q["question"].str() : q["id"].str();
    const auto& opts = q["options"].items();
    bool multi = q["multiSelect"].boolean();
    std::string blockId = block["id"].str();
    auto advance = [this, blockId, msgId, n = qs.size()] {
        if (questionAt_ + 1 < (int)n) {
            ++questionAt_;
            widgetSel_ = 0;
        } else {
            ui(J({{"type", "question"}, {"convId", current_}, {"msgId", msgId}, {"blockId", blockId}, {"answers", answers_}}));
        }
        dirty_ = true;
    };
    if (option < (int)opts.size()) {
        std::string label = opts[option]["label"].str();
        if (multi) {
            Json next = Json::array();
            bool had = false;
            for (const auto& v : answers_[key].items()) {
                if (v.str() == label) had = true;
                else next.push(v);
            }
            if (!had) next.push(label);
            answers_[key] = next;
            return;
        }
        answers_[key] = label;
        advance();
    } else if (option == (int)opts.size()) {
        Ask a;
        a.title = q["question"].str();
        a.placeholder = "輸入你的回答";
        a.done = [this, key, multi, advance](const std::string& text) {
            if (multi) {
                Json arr = Json::array();
                arr.push(text);
                answers_[key] = arr;
            } else {
                answers_[key] = text;
            }
            advance();
        };
        ask(std::move(a));
    } else {
        if (answers_[key].size() == 0) { toast("至少選一項", "error"); return; }
        advance();
    }
}

void Cli::pickerKey(const term::Key& k) {
    using K = term::Key;
    Picker& p = *picker_;
    std::vector<int> vis = p.visible();
    switch (k.code) {
    case K::Up: p.sel = vis.empty() ? 0 : (p.sel + (int)vis.size() - 1) % (int)vis.size(); return;
    case K::Down: case K::Tab: p.sel = vis.empty() ? 0 : (p.sel + 1) % (int)vis.size(); return;
    case K::PageUp: p.sel = std::max(0, p.sel - 10); return;
    case K::PageDown: p.sel = std::min((int)vis.size() - 1, p.sel + 10); return;
    case K::Esc: {
        auto cancel = p.onCancel;
        picker_.reset();
        if (cancel) cancel();
        return;
    }
    case K::Backspace: if (!p.filter.empty()) p.filter.pop_back(); p.sel = 0; return;
    case K::Enter: {
        if (vis.empty()) return;
        // The callback may open the next list, so take this one off first.
        std::unique_ptr<Picker> mine = std::move(picker_);
        Item& it = mine->items[vis[std::clamp(mine->sel, 0, (int)vis.size() - 1)]];
        picking_ = mine.get();
        bool keep = mine->onPick ? mine->onPick(it) : false;
        picking_ = nullptr;
        if (keep && !picker_) picker_ = std::move(mine);
        return;
    }
    case K::Char:
        if (!k.ctrl) { p.filter += k.ch; p.sel = 0; }
        return;
    default:
        return;
    }
}

void Cli::submit() {
    std::string text = util::trim(ed_.utf8());
    if (ask_) {
        if (text.empty() && !ask_->allowEmpty) { toast("請輸入內容", "error"); return; }
        std::unique_ptr<Ask> a = std::move(ask_);
        ed_.set(askDraft_);
        if (a->done) a->done(text);
        return;
    }
    if (text.empty()) return;
    if (history_.empty() || history_.back() != text) history_.push_back(text);
    if (history_.size() > 200) history_.erase(history_.begin());
    histPos_ = -1;
    ed_.clear();
    popupSel_ = 0;
    if (text[0] == '/' && text.find('\n') == std::string::npos) {
        size_t sp = text.find(' ');
        runCommand(toLowerAscii(text.substr(0, sp)), sp == std::string::npos ? "" : util::trim(text.substr(sp + 1)));
        return;
    }
    if (!conv()) { toast("還沒有對話", "error"); return; }
    if (running()) {
        auto token = util::newId();
        queuedDrafts_[token] = text;
        ui(J({{"type", "queue.add"}, {"id", current_}, {"text", text}, {"requestToken", token}}));
    } else ui(J({{"type", "conv.send"}, {"id", current_}, {"text", text}}));
}

// ------------------------------------------------------------ commands

void Cli::pickAgent(const std::string& title, std::function<void(const std::string&)> then, bool withLocals,
                    bool withUser, bool withGemini) {
    Picker p;
    p.title = title;
    if (withUser) p.items.push_back({name("user"), "你", "user", C::kBrand});
    p.items.push_back({name("claude"), "Claude Code", "claude", C::kClaude});
    p.items.push_back({name("codex"), "Codex", "codex", C::kGpt});
    if (withGemini) p.items.push_back({name("gemini"), "Gemini CLI", "gemini", tui::agentColor("gemini")});
    if (withLocals) {
        p.items.push_back({name("local1"), "本地審查員 1", "local1", C::kLocal1});
        p.items.push_back({name("local2"), "本地審查員 2", "local2", C::kLocal2});
    }
    p.onPick = [then](Item& it) { then(it.value); return false; };
    openPicker(std::move(p));
}

void Cli::pickModel(const std::string& agent) {
    const Json* c = conv();
    if (!c) return;
    Json list = models_[agent];
    if (agent == "claude" && list.size() == 0) {
        list = Json::array();
        list.push(J({{"value", ""}, {"displayName", "預設"}, {"description", "Claude Code 的預設模型"}}));
        list.push(J({{"value", "opus"}, {"displayName", "Opus"}, {"description", "最強的程式能力"}}));
        list.push(J({{"value", "sonnet"}, {"displayName", "Sonnet"}, {"description", "速度與能力平衡"}}));
        list.push(J({{"value", "haiku"}, {"displayName", "Haiku"}, {"description", "最快、最省額度"}}));
    }
    std::string key = agent == "claude" ? "claudeModel" : "codexModel";
    std::string cur = (*c)[key].str();
    Picker p;
    p.title = name(agent) + " 的模型";
    bool hasDefault = false;
    for (const auto& m : list.items()) hasDefault |= m["value"].str().empty() || m["value"].str() == "default";
    if (!hasDefault) p.items.push_back({"預設", "使用 CLI 的預設模型", ""});
    for (const auto& m : list.items()) {
        std::string v = m["value"].str() == "default" ? "" : m["value"].str();
        Item it{m["displayName"].str(v.empty() ? "預設" : v), m["description"].str(), v};
        it.checkable = true;
        it.checked = v == cur;
        p.items.push_back(it);
    }
    for (auto& it : p.items) it.checkable = true, it.checked = it.value == cur;
    p.onPick = [this, key, agent](Item& it) {
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{key, it.value}})}}));
        toast(name(agent) + " 改用 " + it.label, "ok");
        return false;
    };
    openPicker(std::move(p));
}

void Cli::pickEffort(const std::string& agent) {
    const Json* c = conv();
    if (!c) return;
    std::string model = (*c)[agent == "claude" ? "claudeModel" : "codexModel"].str();
    std::vector<std::string> levels;
    for (const auto& m : models_[agent].items()) {
        std::string v = m["value"].str() == "default" ? "" : m["value"].str();
        if (v != model) continue;
        for (const auto& e : m["supportedEffortLevels"].items()) levels.push_back(e.str());
    }
    if (levels.empty()) levels = agent == "claude" ? std::vector<std::string>{"low", "medium", "high", "xhigh", "max"}
                                                   : std::vector<std::string>{"low", "medium", "high", "xhigh"};
    if (agent == "claude" && std::find(levels.begin(), levels.end(), "ultracode") == levels.end() &&
        (std::find(levels.begin(), levels.end(), "xhigh") != levels.end() || std::find(levels.begin(), levels.end(), "max") != levels.end())) {
        levels.push_back("ultracode");
    }
    std::string key = agent == "claude" ? "claudeEffort" : "codexEffort";
    std::string cur = (*c)[key].str();
    Picker p;
    p.title = name(agent) + " 的思考程度";
    p.items.push_back({"預設", "由模型決定", ""});
    static const std::map<std::string, std::string> hints = {
        {"minimal", "幾乎不思考，最快"}, {"low", "快速回應，思考較少"}, {"medium", "速度和思考深度平衡"}, {"high", "想得更深，適合複雜問題"},
        {"xhigh", "非常深入的思考"}, {"max", "最大思考深度"}, {"ultra", "最大思考深度，並自動分派子代理（用量最高）"},
        {"ultracode", "在 XHigh 之上開啟動態工作流程，分派子代理（用量最高）"}};
    for (const auto& e : levels) p.items.push_back({effortLabel(e), hints.count(e) ? hints.at(e) : "", e});
    for (auto& it : p.items) it.checkable = true, it.checked = it.value == cur;
    p.onPick = [this, key, agent](Item& it) {
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{key, it.value}})}}));
        toast(name(agent) + " 的思考程度改為 " + it.label, "ok");
        return false;
    };
    openPicker(std::move(p));
}

void Cli::pickPermission() {
    const Json* c = conv();
    if (!c) return;
    Picker p;
    p.title = "權限";
    for (const auto& x : kPerms) {
        Item it{x.label, x.desc, x.id, std::string(x.id) == "bypass" ? C::kErr : 0};
        it.checkable = true;
        it.checked = (*c)["permission"].str() == x.id;
        p.items.push_back(it);
    }
    p.onPick = [this](Item& it) {
        if (it.value != "bypass") {
            ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"permission", it.value}})}}));
            toast("權限改為「" + it.label + "」", "ok");
            return false;
        }
        Picker confirm;
        confirm.title = "確定要跳過所有確認？";
        confirm.items.push_back({"取消", "維持目前的權限", "no"});
        confirm.items.push_back({"確定", "AI 可以不經詢問改任何檔案、執行任何指令、連上網路", "yes", C::kErr});
        confirm.onPick = [this](Item& c2) {
            if (c2.value == "yes") {
                ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"permission", "bypass"}, {"confirmBypass", true}})}}));
                toast("已切換到「跳過所有確認」", "error");
            }
            return false;
        };
        openPicker(std::move(confirm));
        return false;
    };
    openPicker(std::move(p));
}

void Cli::pickPersona(const std::string& who) {
    bool local = who == "local1" || who == "local2";
    std::string cur = settings_["persona"][who].str();
    Picker p;
    p.title = name(who) + " 的性格";
    Item none{"不使用性格", "用 AI 原本的語氣", "none"};
    none.checkable = true;
    none.checked = cur == "none" || cur.empty();
    p.items.push_back(none);
    for (const auto& x : personas_.items()) {
        std::string a = x["agent"].str();
        bool fits = a == "any" || a.empty() || a == who || (local && a == "local");
        if (!fits) continue;
        Item it{x["name"].str(), tui::clipText(x["summary"].str(), 60) + (x["builtin"].boolean() ? "" : "（自訂）"), x["id"].str()};
        it.checkable = true;
        it.checked = it.value == cur;
        p.items.push_back(it);
    }
    p.items.push_back({"＋ 建立新的性格", "寫下身分、性格和說話方式", "+new", C::kBrand});
    bool hasCustom = false;
    for (const auto& x : personas_.items()) hasCustom |= !x["builtin"].boolean();
    if (hasCustom) p.items.push_back({"－ 刪除自訂性格", "", "+delete", C::kErr});
    p.onPick = [this, who](Item& it) {
        if (it.value == "+new") { newPersona(who); return false; }
        if (it.value == "+delete") {
            Picker del;
            del.title = "刪除哪個自訂性格？";
            for (const auto& x : personas_.items()) {
                if (!x["builtin"].boolean()) del.items.push_back({x["name"].str(), tui::clipText(x["summary"].str(), 60), x["id"].str(), C::kErr});
            }
            del.onPick = [this](Item& d) {
                ui(J({{"type", "persona.delete"}, {"id", d.value}}));
                return false;
            };
            openPicker(std::move(del));
            return false;
        }
        ui(J({{"type", "persona.assign"}, {"agent", who}, {"id", it.value}, {"rename", it.value != "none"}}));
        return false;
    };
    openPicker(std::move(p));
}

void Cli::newPersona(const std::string& who) {
    auto draft = std::make_shared<Json>(Json::object());
    Ask a;
    a.title = "新性格：名稱";
    a.placeholder = "例如：耐心的老師";
    a.done = [this, who, draft](const std::string& nm) {
        (*draft)["name"] = nm;
        Ask b;
        b.title = "新性格：一句話簡介（選填）";
        b.placeholder = "例如：說明很仔細，會先講為什麼";
        b.allowEmpty = true;
        b.done = [this, who, draft](const std::string& summary) {
            (*draft)["summary"] = summary;
            Ask c;
            c.title = "新性格：身分、性格和說話方式";
            c.placeholder = "Shift+Enter 換行。CCA 會自動加上規則：角色只影響語氣，不影響程式品質";
            c.done = [this, who, draft](const std::string& prompt) {
                (*draft)["prompt"] = prompt;
                (*draft)["agent"] = who == "local1" || who == "local2" ? "local" : who;
                ui(J({{"type", "persona.save"}, {"persona", *draft}, {"assignTo", who}}));
            };
            ask(std::move(c));
        };
        ask(std::move(b));
    };
    ask(std::move(a));
}

void Cli::showHelp() {
    int cols = std::max(term::cols() - 1, 20);
    std::vector<Line> out = {{}, {{tui::fg(C::kBrand, true), "  指令"}}};
    for (const auto& c : kCommands) {
        std::string head = std::string(c.name) + (*c.args ? std::string(" ") + c.args : "");
        Line l = {{Style(), "    " + head}, {Style(), std::string(std::max(1, 22 - tui::width(head)), ' ')}, {tui::muted(), c.desc}};
        for (auto& w : tui::wrap(l, cols, {}, L(std::string(26, ' ')))) out.push_back(w);
    }
    out.push_back({});
    out.push_back({{tui::fg(C::kBrand, true), "  快捷鍵"}});
    const char* keys[][2] = {
        {"Enter", "送出"}, {"Shift+Enter／Ctrl+J", "換行"}, {"Esc", "停止 AI；沒有在工作時清空輸入"}, {"↑ ↓", "叫出之前輸入過的內容"},
        {"Tab", "補完指令"}, {"Ctrl+U／Ctrl+K", "刪到行首／行尾"}, {"Ctrl+W", "刪掉前一個詞"}, {"Ctrl+L", "清除畫面"}, {"Ctrl+C 兩次", "離開"},
    };
    for (auto& k : keys) out.push_back({{Style(), std::string("    ") + k[0]}, {Style(), std::string(std::max(1, 22 - tui::width(k[0])), ' ')}, {tui::muted(), k[1]}});
    out.push_back({});
    for (auto& w : tui::wrap({{tui::muted(), "權限請求出現時，用 ↑↓ 或數字選擇、Enter 確定，Esc 是拒絕。私訊和群組與桌面版共用，兩邊不能同時開著。"}}, cols, L("  "), L("  "))) out.push_back(w);
    commit(out);
}

// Version control in the terminal: the same colors as the desktop tiles, one line per file.
void Cli::showGit() {
    const Json& g = gitState_;
    if (!g["ok"].boolean()) {
        note(g["error"].str("無法讀取 Git 狀態"), C::kErr, "!");
        return;
    }
    std::string head = "分支 " + g["branch"].str("（無）");
    if (!g["upstream"].str().empty())
        head += " · 追蹤 " + g["upstream"].str() + " ↑" + std::to_string(g["ahead"].i64()) + " ↓" +
                std::to_string(g["behind"].i64());
    if (g["remote"].isObject()) head += " · 遠端 " + g["remote"]["name"].str();
    note(head, C::kBrand, "⎇");
    if (g["identity"]["name"].str().empty() || g["identity"]["email"].str().empty())
        note("尚未設定提交身分，Git 會拒絕提交：/git identity 名稱 e-mail", C::kWarn, "!");
    const std::map<std::string, std::string> kinds = {{"modified", "修改"}, {"added", "新增"},   {"untracked", "新增"},
                                                      {"deleted", "刪除"},  {"renamed", "改名"}, {"conflict", "衝突"}};
    for (const auto& f : g["files"].items()) {
        const auto tone = f["tone"].str(), path = f["path"].str();
        const uint32_t color = tone == "risk" ? C::kErr : tone == "new" ? C::kOk : C::kWarn;
        std::string label = (kinds.count(f["kind"].str()) ? kinds.at(f["kind"].str()) : "修改") + " " + path;
        if (f["locked"].boolean()) label += "  · 永遠不提交（" + f["rule"].str() + "）";
        else if (f["reasons"].size()) label += "  · 不建議：" + f["reasons"].at(0).str();
        note(label, color, gitSel_.count(path) ? "■" : "□");
    }
    if (!g["files"].size()) note("工作區沒有變更");
    note("■ 已選 " + std::to_string(gitSel_.size()) +
         " 個：/git select 調整，/git commit 訊息 提交，/git commit-push 訊息 提交並推送");
    int shown = 0;
    for (const auto& c : g["commits"].items()) {
        if (++shown > 10) break;
        std::string refs;
        for (const auto& r : c["refs"].items()) refs += (refs.empty() ? " (" : ", ") + r.str();
        if (!refs.empty()) refs += ")";
        note(
            c["hash"].str().substr(0, 7) + " " + c["subject"].str() + refs + (c["task"].str().empty() ? "" : "  [CCA]"),
            c["parents"].size() > 1 ? C::kBrand : C::kMuted, c["parents"].size() > 1 ? "◆" : "●");
    }
}

// Same sign-in page as the desktop, as lines: the password is only typed into Git Credential Manager or the browser.
void Cli::showGitAuth() {
    const Json& a = gitState_["auth"];
    const auto state = a["state"].str(), host = a["host"].str(gitState_["remote"]["name"].str("遠端"));
    if (state == "checking") {
        note("確認 " + host + " 的登入狀態…", C::kMuted, "…");
    } else if (state == "ok") {
        note(host + " 已登入", C::kOk, "✓");
    } else if (state == "offline") {
        note("無法連線 " + host + "：" + a["detail"].str("請稍後再試"), C::kWarn, "!");
    } else if (state == "required") {
        note("需要登入 " + host + " 才能推送、拉取與擷取", C::kErr, "!");
        if (!a["detail"].str().empty()) note("Git 回報：" + a["detail"].str(), C::kMuted);
        if (a["ssh"].boolean()) {
            note("SSH 遠端用金鑰登入：ssh-keygen -t ed25519，把 .pub 內容加到 " + host +
                 " 的 SSH 金鑰設定，再輸入 /git 重新檢查");
        } else if (a["helper"].str().empty() && a.has("helper") && !a["gcm"].boolean()) {
            note("這台電腦沒有 Git 認證管理員，請安裝或更新 Git for Windows 後輸入 /git 重新檢查", C::kWarn);
        } else {
            note("輸入 /git login 開啟 Git 認證管理員的登入視窗；帳號密碼只在該視窗或瀏覽器輸入，CCA 看不到也不保存");
        }
    }
}

void Cli::gitCommand(const std::string& args) {
    auto send = [&](const std::string& type, Json extra = Json::object()) {
        extra["type"] = type;
        extra["id"] = current_;
        ui(extra);
    };
    const auto word = args.substr(0, args.find(' '));
    const auto rest = args.find(' ') == std::string::npos ? "" : util::trim(args.substr(args.find(' ') + 1));
    if (args.empty() || word == "status" || word == "log") {
        gitAsked_ = true;
        send("git.open");
        return;
    }
    if (word == "push" || word == "pull" || word == "fetch") {
        send("git." + word);
        return;
    }
    if (word == "login") {
        const Json& a = gitState_["auth"];
        const bool setup = a.has("helper") && a["helper"].str().empty() && a["gcm"].boolean();
        if (setup) note("登入時會一併啟用 Git 認證管理員（credential.helper=manager）");
        note("請在跳出的 Git 認證管理員視窗或瀏覽器完成登入，最多等待 5 分鐘");
        send("git.login", J({{"setupHelper", setup}}));
        return;
    }
    if (word == "identity") {
        // The e-mail is the last word, so the name may contain spaces.
        const auto space = rest.find_last_of(' ');
        if (space == std::string::npos) {
            toast("用法：/git identity 名稱 e-mail（加 --repo 只套用到這個倉庫）", "error");
            return;
        }
        std::string name = util::trim(rest.substr(0, space)), email = util::trim(rest.substr(space + 1));
        bool global = true;
        if (util::startsWith(name, "--repo ")) {
            global = false;
            name = util::trim(name.substr(7));
        }
        send("git.identity", J({{"name", name}, {"email", email}, {"global", global}}));
        return;
    }
    if (word == "select") {
        if (!gitState_["ok"].boolean()) {
            gitAsked_ = true;
            send("git.open");
            note("先讀取 Git 狀態，再輸入一次 /git select");
            return;
        }
        Picker p;
        p.title = "選擇要提交的檔案（黃：修改 · 綠：新增 · 紅：不建議提交）";
        p.footer = "Enter 選取或取消 · Esc 完成";
        for (const auto& f : gitState_["files"].items()) {
            if (f["locked"].boolean()) continue;
            const auto tone = f["tone"].str();
            Item it{f["path"].str(), f["reasons"].size() ? "不建議：" + f["reasons"].at(0).str() : "", f["path"].str(),
                    tone == "risk"  ? C::kErr
                    : tone == "new" ? C::kOk
                                    : C::kWarn};
            it.checkable = true;
            it.checked = gitSel_.count(f["path"].str()) > 0;
            p.items.push_back(it);
        }
        p.onPick = [this](Item& it) {
            it.checked = !it.checked;
            if (it.checked) gitSel_.insert(it.value);
            else gitSel_.erase(it.value);
            return true;
        };
        openPicker(std::move(p));
        return;
    }
    if (word == "commit" || word == "commit!" || word == "commit-push") {
        if (rest.empty()) {
            toast("用法：/git commit 提交訊息", "error");
            return;
        }
        Json paths = Json::array(), risky = Json::array();
        for (const auto& f : gitState_["files"].items()) {
            if (!gitSel_.count(f["path"].str()) || f["locked"].boolean()) continue;
            paths.push(f["path"].str());
            if (f["tone"].str() == "risk") risky.push(f["path"].str());
        }
        if (!paths.size()) {
            toast("沒有選擇檔案；先用 /git 讀取狀態或 /git select 選擇", "error");
            return;
        }
        send("git.commit", J({{"paths", paths},
                              {"message", rest},
                              {"push", word == "commit-push"},
                              {"task", true},
                              {"allowRisk", risky},
                              {"allowSecrets", word == "commit!"}}));
        return;
    }
    if (word == "never" || word == "allow") {
        Json rules = Json::array();
        bool present = false;
        for (const auto& r : settings_["git"]["neverCommit"].items()) {
            present |= r.str() == rest;
            if (word == "never" || r.str() != rest) rules.push(r);
        }
        if (rest.empty()) {
            std::string list;
            for (const auto& r : rules.items()) list += (list.empty() ? "" : "、") + r.str();
            note("永遠不提交：" + (list.empty() ? std::string("（沒有規則）") : list));
            return;
        }
        if (word == "never" && !present) rules.push(rest);
        send("git.rules", J({{"rules", rules}}));
        toast(word == "never" ? "已加入永遠不提交規則 " + rest : "已移除規則 " + rest, "ok");
        return;
    }
    if (word == "auto") {
        Json a = gitState_["auto"];
        const bool fetch = rest == "fetch on" ? true : rest == "fetch off" ? false : a["fetch"].boolean();
        const bool pull = rest == "pull on" ? true : rest == "pull off" ? false : a["pull"].boolean();
        send("git.auto", J({{"fetch", fetch || pull}, {"pull", pull}, {"minutes", a["minutes"].i64(5)}}));
        note(std::string("自動擷取 ") + (fetch || pull ? "開啟" : "關閉") + " · 自動快轉拉取 " +
             (pull ? "開啟" : "關閉") + " · 每 " + std::to_string(a["minutes"].i64(5)) + " 分鐘");
        return;
    }
    toast(
        "用法：/git [login|identity 名稱 e-mail|select|commit 訊息|commit-push 訊息|push|pull|fetch|never 規則|allow "
        "規則|auto fetch|pull on|off]",
        "error");
}

void Cli::showStatus() {
    std::vector<Line> out = {{}, {{tui::fg(C::kBrand, true), std::string("  ") + CCA_PRODUCT + " CLI " + version_}}};
    for (const char* a : {"claude", "codex", "gemini"}) {
        const Json& s = auth_[a];
        std::string st = s["state"].str("checking");
        bool ok = st == "ok";
        std::string label = std::string(a) == "claude"   ? "Claude Code"
                            : std::string(a) == "gemini" ? "Gemini CLI"
                                                         : "Codex";
        Line l = {{tui::fg(ok ? C::kOk : C::kWarn), ok ? "  ✓ " : "  ! "}, {Style(), label}};
        if (!s["version"].str().empty()) l.push_back({tui::muted(), " " + s["version"].str()});
        if (!s["plan"].str().empty()) l.push_back({tui::muted(), " · 方案 " + s["plan"].str()});
        if (!ok) l.push_back({tui::fg(C::kWarn), " · " + s["message"].str(st)});
        out.push_back(l);
        if (std::string(a) == "gemini") {
            const auto& quota = usage_[a]["quota"];
            for (const auto& b : quota["buckets"].items()) {
                std::string remaining = b["remainingFraction"].isNull()
                                            ? "比例未知"
                                            : std::to_string((int)(b["remainingFraction"].num() * 100)) + "%";
                out.push_back({{tui::muted(), "    " + b["modelId"].str() + " · 剩餘 " + remaining + " · 重置 " +
                                                  b["resetTime"].str("未知")}});
            }
            if (!quota["error"].str().empty()) out.push_back({{tui::muted(), "    " + quota["error"].str()}});
        }
        const Json& sess = usage_[a]["session"];
        if (sess["turns"].i64() > 0) {
            out.push_back({{tui::muted(), "    這次開啟後：" + std::to_string(sess["turns"].i64()) + " 次回覆 · 輸入 " +
                                              tui::tokens(sess["inputTokens"].i64()) + " · 輸出 " +
                                              tui::tokens(sess["outputTokens"].i64())}});
        }
    }
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "  記憶體：%.1f MiB（工作集），私有 %.1f MiB。AI 子程序與終端機另計。",
                      pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0);
        out.push_back({{tui::muted(), buf}});
    }
    out.push_back({{tui::muted(), "  資料：" + util::narrow(util::appDataDir())}});
    out.push_back({{tui::muted(), std::string("  去 AI 化：") + (settings_["humanize"]["enabled"].boolean() ? "開啟" : "關閉")}});
    commit(out);
    ui(J({{"type", "auth.check"}}));
}

void Cli::runCommand(const std::string& cmd, const std::string& args) {
    if (cmd == "/gemini-login") {
        ui(J({{"type", "auth.login"}, {"agent", "gemini"}}));
        return;
    }
    const Json* c = conv();
    bool group = c && (*c)["kind"].str() == "group";
    auto needGroup = [&] {
        if (group) return true;
        toast("這個指令只能在群組裡用", "error");
        return false;
    };
    if (cmd == "/theme") {
        if (args.empty()) {
            for (const auto& theme : themeCatalogue().items())
                if (themeAllowed(theme)) note(theme["id"].str() + " · " + theme["name"].str());
            for (const auto& theme : pluginThemes().items())
                note(theme["id"].str() + " · " + theme["name"].str() + "（" + theme["kicker"].str() + "）");
        } else ui(J({{"type", "theme.set"}, {"theme", args}, {"requestToken", util::newId()}}));
        return;
    }
    if (cmd == "/auto") {
        std::string agent = c && ((*c)["kind"].str() == "codex" || (*c)["lead"].str() == "codex") ? "codex" : "claude";
        std::string action = args;
        if (!util::startsWith(action, "preview ")) {
            for (const std::string provider : {"claude", "codex"}) {
                if (!util::endsWith(action, " " + provider)) continue;
                agent = provider;
                action = util::trim(action.substr(0, action.size() - provider.size()));
            }
        }
        Json request = J({{"type", "routing.set"}, {"id", current_}, {"agent", agent}});
        if (action == "on" || action == "off") {
            request["model"] = request["effort"] = action == "on";
            ui(request);
        } else if (action == "model on" || action == "model off" || action == "effort on" || action == "effort off") {
            request[util::startsWith(action, "model ") ? "model" : "effort"] = util::endsWith(action, " on");
            ui(request);
        } else if (util::startsWith(action, "preview "))
            ui(J({{"type", "routing.preview"}, {"id", current_}, {"agent", agent}, {"text", action.substr(8)}}));
        else {
            note("/auto on|off [claude|codex]；/auto model|effort on|off [claude|codex]；/auto preview 需求");
            if (c) note((*c)["adaptive"].dump());
        }
        return;
    }
    if (cmd == "/context") {
        ui(J({{"type", "context.get"}, {"id", current_}}));
        return;
    }
    if (cmd == "/queue") {
        size_t space = args.find(' ');
        auto action = args.substr(0, space);
        auto rest = space == std::string::npos ? "" : util::trim(args.substr(space + 1));
        Json message = J({{"id", current_}, {"requestToken", util::newId()}});
        if (action.empty()) action = "get";
        if (action == "add") message["text"] = rest;
        else if (action == "edit" || action == "move") {
            auto split = rest.find(' ');
            message["itemId"] = rest.substr(0, split);
            auto value = split == std::string::npos ? "" : rest.substr(split + 1);
            if (action == "edit") message["text"] = value;
            else {
                bool valid = !value.empty() &&
                             std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
                long long to = valid && value.size() < 4 ? std::atoll(value.c_str()) : -1;
                message["to"] = to - 1;
            }
        } else if (action == "remove" || action == "retry") message["itemId"] = rest;
        else if (action != "get" && action != "pause" && action != "resume") {
            toast("/queue [add 文字|edit ID 文字|move ID 順位|remove ID|retry ID|pause|resume]", "error");
            return;
        }
        message["type"] = "queue." + action;
        ui(message);
        return;
    }
    if (cmd == "/git") {
        gitCommand(args);
        return;
    }
    if (cmd == "/resume-auto") {
        Json resume = settings_["autoResume"];
        if (args == "on" || args == "off") {
            resume["enabled"] = args == "on";
            ui(J({{"type", "autoResume.set"}, {"settings", resume}}));
        }
        const bool on = args.empty() ? resume["enabled"].boolean() : args == "on";
        note(std::string("額度恢復後自動接續：") + (on ? "開啟" : "關閉") + " · 重置後再等 " +
             std::to_string(resume["delayMinutes"].i64(3)) + " 分鐘 · 最多 " +
             std::to_string(resume["maxAttempts"].i64(3)) +
             " 次；時段與各 AI 的預設等待在桌面版 設定 › 額度與自動接續。");
        return;
    }
    if (cmd == "/update" && (util::startsWith(args, "switch ") || args == "return")) {
        const std::string wanted = args == "return" ? "" : util::trim(args.substr(7));
        Json chosen;
        for (const auto& choice : updateChoices_.items())
            if (choice["edition"].str() == wanted) chosen = choice;
        if (chosen.isNull()) {
            toast("資料夾裡沒有這個版本；先執行 /update check", "error");
            return;
        }
        Ask approval;
        approval.title =
            args == "return"
                ? "退回普通版本 " + chosen["version"].str() +
                      "：保存的主題、性格、插件與各對話的模式設定會還原。輸入 ALLOW 繼續"
                : "切換到 " + chosen["editionName"].str() + " " + chosen["version"].str() +
                      "：它不支援的設定會保存在 editions\\ordinary-vault.json，回到普通版本時還原。輸入 ALLOW 繼續";
        approval.placeholder = "ALLOW";
        approval.done = [this, wanted](const std::string& answer) {
            if (answer == "ALLOW") ui(J({{"type", "update.install"}, {"choice", wanted.empty() ? "normal" : wanted}}));
            else toast("沒有切換版本");
        };
        ask(std::move(approval));
        return;
    }
    if (cmd == "/update") {
        updateAsked_ = true;
        if (args == "check" || args == "install" || args == "cancel" || args == "open")
            ui(J({{"type", "update." + args}}));
        else if (args == "auto on" || args == "auto off") ui(J({{"type", "update.auto"}, {"auto", args == "auto on"}}));
        else if (args == "source" || util::startsWith(args, "source ")) ui(J({{"type", "update.source"}}));
        else ui(J({{"type", "update.get"}}));
        return;
    }
    if (cmd == "/help" || cmd == "/?") { showHelp(); return; }
    if (cmd == "/exit" || cmd == "/quit") { quit_ = true; return; }
    if (cmd == "/status") { showStatus(); return; }
    if (cmd == "/about") {
        std::vector<Line> out = {{}, {{tui::fg(C::kBrand, true), std::string("  ") + CCA_PRODUCT + " " + version_}}};
        for (const char* text :
             {"CCA 將多階段的開發工作整理為可持續交接、獨立審查與核對結果的流程。",
              "需求與計畫保留版本，交接附上實際變更、指令狀態與待辦；驗收依據由主機取得，並與來源版本連結。",
              "使用者掌握權限、流程及驗收。模型與表達風格可以替換，技術規範與工具權限保持一致。",
              "沿用官方 CLI 登入與本機專案；API 可另行設定。性格由本機資料管理，新安裝預設不啟用角色。",
              "不同模型同意或測試通過，仍不能保證軟體不存在錯誤。"})
            for (auto& line : tui::wrap({{tui::muted(), text}}, std::max(20, term::cols() - 1), L("  "), L("  ")))
                out.push_back(line);
        commit(out);
        return;
    }
    if (cmd == "/badges") {
        if (args.empty()) {
            note("本機徽章 · " + std::to_string(badges_["owned"].size()) + " 枚 · 展示上限 6 枚", C::kBrand);
            Json sorted = Json::array();
            for (const auto& id : badges_["order"].items())
                for (const auto& badge : badges_["catalog"].items())
                    if (badge["id"] == id) sorted.push(badge);
            for (const auto& badge : badges_["catalog"].items()) {
                bool owned = false;
                for (const auto& id : badges_["owned"].items()) owned |= id == badge["id"];
                if (!owned) sorted.push(badge);
            }
            for (const auto& badge : sorted.items()) {
                const std::string id = badge["id"].str();
                bool owned = false, hidden = false, displayed = false;
                for (const auto& item : badges_["owned"].items()) owned |= item.str() == id;
                for (const auto& item : badges_["hidden"].items()) hidden |= item.str() == id;
                for (const auto& item : badges_["display"].items()) displayed |= item.str() == id;
                note(std::string(owned ? (hidden      ? "[隱藏] "
                                          : displayed ? "[展示] "
                                                      : "[收藏] ")
                                       : "[未取得] ") +
                         badge["name"].str() + " · " + id,
                     owned ? C::kBrand : C::kMuted);
            }
            note("排序與隱藏僅調整展示；本機徽章不提供權限或帳號認證。");
        } else {
            size_t space = args.find(' ');
            std::string action = args.substr(0, space),
                        id = space == std::string::npos ? "" : util::trim(args.substr(space + 1));
            bool owned = false;
            for (const auto& item : badges_["owned"].items()) owned |= item.str() == id;
            if (!owned || (action != "hide" && action != "show" && action != "first")) {
                toast("用法：/badges hide|show|first ID；僅可調整已持有的徽章。", "error");
                return;
            }
            Json order = Json::array(), hidden = Json::array();
            if (action == "first") order.push(id);
            for (const auto& item : badges_["order"].items())
                if (action != "first" || item.str() != id) order.push(item);
            for (const auto& item : badges_["hidden"].items())
                if (item.str() != id || action == "first") hidden.push(item);
            if (action == "hide") hidden.push(id);
            ui(J({{"type", "badges.display"}, {"order", order}, {"hidden", hidden}, {"requestToken", "cli-badges"}}));
        }
        return;
    }
    if (cmd == "/activity") {
        std::string mode = args.empty() ? "daily" : args == "total" ? "cumulative" : args;
        if (mode != "daily" && mode != "weekly" && mode != "cumulative") {
            toast("用法：/activity daily、/activity weekly 或 /activity total", "error");
            return;
        }
        const Json& activity = usage_["activity"];
        const auto& days = activity["days"].items();
        note("Token 活動 · 已保存紀錄累計 " + tui::tokens(activity["total"].i64()), C::kBrand);
        long long peak = 1;
        size_t begin = days.size() > 14 ? days.size() - 14 : 0;
        for (size_t i = begin; i < days.size(); ++i) peak = std::max(peak, (long long)days[i][mode].i64());
        std::vector<Line> out;
        for (size_t i = begin; i < days.size(); ++i) {
            const Json& day = days[i];
            long long value = day[mode].i64();
            int blocks = value > 0 ? std::max(1, int(double(value) / peak * 20)) : 0;
            std::string bar;
            for (int j = 0; j < blocks; ++j) bar += "▪";
            out.push_back({{tui::muted(), "  " + day["date"].str() + "  "},
                           {tui::fg(C::kBrandDeep), bar},
                           {Style(), std::string(22 - blocks, ' ') + tui::tokens(value)},
                           {tui::muted(), mode == "daily" && !day["recorded"].boolean() ? " · 尚無當日紀錄" : ""}});
        }
        commit(out);
        note(mode == "daily"    ? "每日輸入與輸出用量"
             : mode == "weekly" ? "截至該日期的近 7 日用量"
                                : "已保存紀錄截至該日期的累計用量");
        note("終端機顯示最近 14 個日期；桌面日曆顯示近一年。日期依記錄時的主機時區。");
        return;
    }
    if (cmd == "/flow") {
        if (args == "auto" || args == "compact" || args == "off") {
            ui(J({{"type", "visual.set"}, {"mode", args}}));
        } else if (!args.empty()) toast("用法：/flow auto、/flow compact 或 /flow off", "error");
        else {
            Picker p;
            p.title = "協作流程圖";
            p.items = {{"自動", "寬視窗顯示節點和傳輸線，窄視窗自動精簡", "auto"},
                       {"精簡", "只顯示成員和工作狀態", "compact"}, {"隱藏", "保留原本的文字狀態列", "off"}};
            p.onPick = [this](Item& it) { ui(J({{"type", "visual.set"}, {"mode", it.value}})); return false; };
            openPicker(std::move(p));
        }
        return;
    }
    if (cmd == "/motion") {
        if (!args.empty() && args != "on" && args != "off") { toast("用法：/motion on 或 /motion off", "error"); return; }
        bool on = args == "on" || (args.empty() && !settings_["visual"]["motion"].boolean(true));
        ui(J({{"type", "visual.set"}, {"motion", on}}));
        toast(on && !systemMotion_ ? "Windows 已停用動態效果，流程圖保留靜態狀態" : on ? "動態效果已開啟" : "動態效果已關閉");
        return;
    }
    if (cmd == "/groups") {
        Json order = Json::array();
        for (const auto& x : convs_.items())
            if (x["kind"].str() == "group") order.push(x["id"]);
        if (args.empty()) {
            note("協作群組 · 固定順序", C::kBrand);
            int index = 0;
            for (const auto& x : convs_.items())
                if (x["kind"].str() == "group")
                    note(std::to_string(++index) + ". " + convTitle(x) +
                         (x["id"].str() == current_ ? " · 目前群組" : ""));
            if (!index) note("尚未建立協作群組");
        } else {
            if (!needGroup()) return;
            if (args != "up" && args != "down") {
                toast("用法：/groups、/groups up 或 /groups down", "error");
                return;
            }
            int index = -1;
            for (size_t i = 0; i < order.size(); ++i)
                if (order.at(i).str() == current_) index = (int)i;
            const int next = index + (args == "up" ? -1 : 1);
            if (index < 0 || next < 0 || next >= (int)order.size()) {
                toast("此群組已位於邊界");
                return;
            }
            std::swap(order.items()[index], order.items()[next]);
            ui(J({{"type", "groups.reorder"}, {"order", order}, {"requestToken", "cli-groups"}}));
        }
        return;
    }
    if (cmd == "/chat" || cmd == "/chats") {
        Picker p;
        p.title = "切換對話";
        for (const auto& x : convs_.items()) {
            bool g = x["kind"].str() == "group";
            std::string hint = g ? (x["cwd"].str().empty() ? "群組 · 沒有資料夾" : "群組 · " + shortPath(x["cwd"].str())) : "私訊";
            if (states_[x["id"].str()]["running"].boolean()) hint += " · 工作中";
            if (unread_.count(x["id"].str())) hint += " · 新回覆";
            Item it{(g ? "# " : "@ ") + convTitle(x), hint, x["id"].str(), g ? C::kBrand : tui::agentColor(x["kind"].str())};
            it.checkable = true;
            it.checked = it.value == current_;
            p.items.push_back(it);
        }
        p.items.push_back({"＋ 建立新的群組", "", "+new", C::kBrand});
        p.onPick = [this](Item& it) {
            if (it.value == "+new") { runCommand("/new", ""); return false; }
            if (it.value != current_) open(it.value);
            return false;
        };
        openPicker(std::move(p));
        return;
    }
    if (cmd == "/new") {
        std::string here = util::narrow(launchDir_);
        Json m = J({{"type", "conv.create"}, {"name", args.empty() ? "協作群組" : args}});
        if (util::projectFolderProblem(launchDir_).empty()) m["cwd"] = here;
        ui(m);
        return;
    }
    if (!c) { toast("還沒有對話", "error"); return; }
    if (cmd == "/lead") {
        if ((*c)["kind"].str() == "codex") { toast("GPT 私訊固定由 GPT 處理；請在群組或 Claude 私訊切換主刀"); return; }
        auto setLead = [this](const std::string& lead) {
            ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"lead", lead}})}}));
        };
        if (args == "gpt" || args == "codex") setLead("codex");
        else if (args == "claude") setLead("claude");
        else if (!args.empty()) toast("用法：/lead claude 或 /lead gpt", "error");
        else {
            Picker p;
            p.title = "選擇主刀手";
            p.items = {{"Claude", "Claude 實作，GPT 審查", "claude"}, {"GPT", "GPT 實作，Claude 可用時負責審查", "codex"}};
            p.onPick = [setLead](Item& it) { setLead(it.value); return false; };
            openPicker(std::move(p));
        }
        return;
    }
    if (cmd == "/takeover") { ui(J({{"type", "conv.takeover"}, {"id", current_}})); return; }
    if (cmd == "/fallback") {
        if (!args.empty() && args != "on" && args != "off") { toast("用法：/fallback on 或 /fallback off", "error"); return; }
        bool on = args == "on" || (args.empty() && !(*c)["autoFallback"].boolean(true));
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"autoFallback", on}})}}));
        toast(on ? "已開啟 GPT 自動接手" : "已關閉 GPT 自動接手");
        return;
    }
    if (cmd == "/cd") {
        if (args.empty()) {
            std::string cwd = (*c)["cwd"].str();
            toast(cwd.empty() ? "這個對話還沒有專案資料夾。用法：/cd 路徑，/cd . 是目前資料夾" : "專案資料夾：" + cwd);
            return;
        }
        std::wstring raw = util::widen(args == "~" ? util::narrow(util::homeDir()) : args);
        if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') raw = raw.substr(1, raw.size() - 2);
        if (util::startsWith(args, "~\\") || util::startsWith(args, "~/")) raw = util::joinPath(util::homeDir(), util::widen(args.substr(2)));
        wchar_t full[MAX_PATH * 4];
        std::wstring prev;
        prev.resize(MAX_PATH * 4);
        DWORD n = GetCurrentDirectoryW((DWORD)prev.size(), prev.data());
        prev.resize(n);
        SetCurrentDirectoryW(launchDir_.c_str());
        DWORD len = GetFullPathNameW(raw.c_str(), MAX_PATH * 4, full, nullptr);
        SetCurrentDirectoryW(prev.c_str());
        std::wstring path = len > 0 && len < MAX_PATH * 4 ? std::wstring(full, len) : raw;
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        std::string before = (*c)["cwd"].str();
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"cwd", util::narrow(path)}})}}));
        if (const Json* after = conv()) {
            if ((*after)["cwd"].str() != before) note("專案資料夾改為 " + shortPath((*after)["cwd"].str()), C::kBrand, "✓");
        }
        return;
    }
    if ((cmd == "/model" || cmd == "/effort") && (*c)["kind"].str() == "gemini") {
        note("Gemini CLI 沒有回報模型目錄與思考程度，CCA 使用 CLI 預設值，不猜測模型名稱。");
        return;
    }
    if (cmd == "/model") {
        if (group) pickAgent("要改誰的模型？", [this](const std::string& a) { pickModel(a); });
        else pickModel((*c)["kind"].str());
        return;
    }
    if (cmd == "/effort") {
        if (group) pickAgent("要改誰的思考程度？", [this](const std::string& a) { pickEffort(a); });
        else pickEffort((*c)["kind"].str());
        return;
    }
    if (cmd == "/gemini") {
        if (!needGroup()) return;
        if (!args.empty() && args != "on" && args != "off") {
            toast("用法：/gemini on 或 /gemini off", "error");
            return;
        }
        bool on = args == "on" || (args.empty() && !(*c)["geminiMember"].boolean());
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"geminiMember", on}})}}));
        toast(on ? "Gemini 加入群組，擔任第三位審查者；額度不足或帳號無法使用時會跳過" : "Gemini 已離開群組");
        return;
    }
    if (cmd == "/permission" || cmd == "/mode") {
        pickPermission();
        return;
    }
    if (cmd == "/stop") {
        ui(J({{"type", "conv.stop"}, {"id", current_}}));
        return;
    }
    if (cmd == "/continue") {
        if (needGroup()) ui(J({{"type", "group.continue"}, {"id", current_}}));
        return;
    }
    if (cmd == "/resume") {
        if (!needGroup()) return;
        if (state()["task"]["status"].str() != "interrupted") {
            toast("這個群組沒有中斷的任務", "error");
            return;
        }
        ui(J({{"type", "task.resume"}, {"id", current_}}));
        return;
    }
    if (cmd == "/pause" || cmd == "/rereview") {
        if (needGroup()) ui(J({{"type", cmd == "/pause" ? "task.pause" : "task.rereview"}, {"id", current_}}));
        return;
    }
    if (cmd == "/provider-models" || cmd == "/provider-remove" || cmd == "/toolserver-test" ||
        cmd == "/toolserver-remove") {
        std::string type = cmd == "/provider-models"   ? "provider.models"
                           : cmd == "/provider-remove" ? "provider.remove"
                           : cmd == "/toolserver-test" ? "toolserver.test"
                                                       : "toolserver.remove";
        ui(J({{"type", type}, {"id", args}}));
        return;
    }
    if (cmd == "/toolserver-pin") {
        std::istringstream words(args);
        std::string id, name;
        words >> id;
        Json pins = Json::object();
        while (words >> name) {
            if (!toolHashes_[id][name].isString()) {
                toast("沒有檢視過 " + name + "：先執行 /toolserver-test " + id, "error");
                return;
            }
            pins[name] = toolHashes_[id][name];
        }
        if (id.empty() || pins.fields().empty()) {
            toast("用法：/toolserver-pin <id> <工具名稱…>（先用 /toolserver-test 檢視）", "error");
            return;
        }
        ui(J({{"type", "toolserver.pin"}, {"id", id}, {"pins", pins}}));
        return;
    }
    if (cmd == "/evidence") {
        note(states_[current_]["collaboration"]["evidence"].dump());
        return;
    }
    if (cmd == "/quality-check" || cmd == "/browser") {
        ui(J({{"type", cmd == "/browser" ? "browser.action" : "quality.get"},
              {"id", current_},
              {"operation", "open"},
              {"url", args}}));
        return;
    }
    if (cmd == "/verify" || cmd == "/reproduce" || cmd == "/browser-run") {
        std::string text;
        if (!util::readFile(util::widen(args), text) || text.size() > 65536) {
            toast("讀不到驗證 JSON 檔", "error");
            return;
        }
        Json request = Json::parse(text);
        if (!request.isObject()) {
            toast("驗證設定須為 JSON 物件", "error");
            return;
        }
        request["type"] = cmd == "/browser-run" ? "browser.action" : "quality.run";
        request["operation"] = "verify";
        request["kind"] = cmd == "/reproduce" ? "repro" : "test";
        request["id"] = current_;
        request["revision"] = states_[current_]["collaboration"]["plan"]["revision"].i64();
        request["confirm"] = true;
        ui(request);
        return;
    }
    if (cmd == "/inbox") {
        showInbox_ = true;
        ui(J({{"type", "inbox.get"}}));
        return;
    }
    if (cmd == "/plan" || cmd == "/focus-import") {
        if (cmd == "/plan" && args.empty()) {
            const auto& plan = states_[current_]["collaboration"]["plan"];
            note(plan.isObject() ? plan.dump() : "尚無計畫。用 /plan import 計畫.json 匯入。");
            planRevision_ = plan["revision"].i64(-1);
            planConv_ = current_;
            return;
        }
        const auto path = cmd == "/plan" && util::startsWith(args, "import ") ? args.substr(7) : args;
        std::string text;
        if (path.empty() || !util::readFile(util::widen(path), text) || text.size() > 65536) {
            toast("讀不到 JSON 檔或超過 64 KiB", "error");
            return;
        }
        Json parsed = Json::parse(text);
        if (cmd == "/focus-import") ui(J({{"type", "focus.profiles"}, {"profiles", parsed}}));
        else
            ui(J({{"type", "plan.save"},
                  {"id", current_},
                  {"revision", states_[current_]["collaboration"]["plan"]["revision"].i64()},
                  {"plan", parsed}}));
        note("已送出儲存要求；請用 /plan 檢查保存結果後再核准。");
        return;
    }
    if (cmd == "/plan-comment" || cmd == "/plan-approve" || cmd == "/plan-run" || cmd == "/plan-accept") {
        if (planConv_ != current_ || planRevision_ != states_[current_]["collaboration"]["plan"]["revision"].i64(-2)) {
            toast("先用 /plan 檢查目前版本", "error");
            return;
        }
        const auto type = cmd == "/plan-comment"   ? "plan.comment"
                          : cmd == "/plan-approve" ? "plan.approve"
                          : cmd == "/plan-run"     ? "plan.execute"
                                                   : "plan.accept";
        ui(J({{"type", type},
              {"id", current_},
              {"revision", planRevision_},
              {"text", args},
              {"step", cmd == "/plan-accept" ? args : ""},
              {"accepted", true}}));
        return;
    }
    if (cmd == "/focus") {
        auto split = args.find(' ');
        if (args.empty()) {
            note(settings_["reviewProfiles"].dump());
            note(states_[current_]["collaboration"]["focus"].dump());
            return;
        }
        ui(J({{"type", "focus.assign"},
              {"id", current_},
              {"agent", args.substr(0, split)},
              {"focus", split == std::string::npos ? "" : util::trim(args.substr(split + 1))}}));
        return;
    }
    if (cmd == "/opinions" || cmd == "/opinions-stop" || cmd == "/opinion-plan") {
        if (cmd != "/opinions") {
            ui(J({{"type", cmd == "/opinions-stop" ? "opinion.stop" : "opinion.plan"}, {"id", current_}}));
            return;
        }
        if (args.empty()) {
            note(states_[current_]["collaboration"]["opinion"].dump());
            return;
        }
        auto split = args.find(' ');
        if (split == std::string::npos) {
            toast("用法：/opinions claude,codex 問題（會使用所選成員的額度）", "error");
            return;
        }
        Json agents = Json::array();
        std::string list = args.substr(0, split);
        size_t start = 0;
        while (start < list.size()) {
            auto comma = list.find(',', start);
            agents.push(list.substr(start, comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        note("開始唯讀意見蒐集；共識整理另需一次模型回覆。");
        ui(J({{"type", "opinion.start"}, {"id", current_}, {"agents", agents}, {"question", args.substr(split + 1)}}));
        return;
    }
    if (cmd == "/workspace") {
        if (args.empty()) {
            note(states_[current_]["collaboration"]["workspace"].dump());
            return;
        }
        if (args == "merge" && (mergeConv_ != current_ || !mergePreview_["clean"].boolean())) {
            toast("先用 /workspace check 查看可合併版本", "error");
            return;
        }
        if (args != "create" && args != "check" && args != "merge") {
            toast("工作區動作是 create、check 或 merge", "error");
            return;
        }
        ui(J({{"type", "worktree." + args},
              {"id", current_},
              {"source", mergePreview_["source"]},
              {"target", mergePreview_["target"]}}));
        return;
    }
    if (cmd == "/pipeline" || cmd == "/provider-import" || cmd == "/toolserver-import") {
        if (cmd == "/pipeline" && !needGroup()) return;
        if (cmd == "/pipeline" && args == "off") {
            ui(J({{"type", "pipeline.set"}, {"id", current_}, {"pipeline", Json()}}));
            return;
        }
        std::wstring path = util::widen(args);
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
        std::string data;
        if (args.empty() || !util::readFile(path, data) || data.size() > 65536) {
            toast("讀不到設定檔，或檔案超過 64 KiB", "error");
            return;
        }
        Json config = Json::parse(data);
        if (!config.isObject()) {
            toast("設定檔必須是 JSON 物件", "error");
            return;
        }
        if (cmd == "/pipeline") ui(J({{"type", "pipeline.set"}, {"id", current_}, {"pipeline", config}}));
        else if (cmd == "/provider-import")
            ui(J({{"type", "provider.save"}, {"provider", config["provider"]}, {"apiKey", config["apiKey"]}}));
        else {
            Json server = config.has("server") ? config["server"] : config;
            Ask approval;
            approval.title =
                "MCP 會在宿主機執行 " + server["command"].str() + "。確認已檢查來源、參數及允許工具後，輸入 ALLOW";
            approval.placeholder = "ALLOW";
            approval.done = [this, server](const std::string& answer) {
                if (answer == "ALLOW") ui(J({{"type", "toolserver.save"}, {"server", server}, {"confirm", true}}));
                else toast("沒有啟用 MCP 伺服器");
            };
            ask(std::move(approval));
        }
        return;
    }
    if (cmd == "/task-history") {
        ui(J({{"type", "task.history"}, {"id", current_}, {"after", (long long)std::atoll(args.c_str())}}));
        return;
    }
    if (cmd == "/sandbox") {
        ui(J({{"type", "sandbox.status"}}));
        return;
    }
    if (cmd == "/claude-usage") {
        bool on = args == "on" ? true : args == "off" ? false : !settings_["claudeUsageLive"].boolean();
        ui(J({{"type", "claudeUsage.set"}, {"enabled", on}}));
        toast(on ? "已開啟：每 60 秒（Claude 工作時每 20 秒）查詢額度，權杖只用來查詢、不會保存" : "已關閉即時額度查詢",
              "ok");
        return;
    }
    if (cmd == "/review") {
        std::string scope = util::trim(args).empty() ? "task" : util::trim(args);
        if (scope != "turn" && scope != "task" && scope != "all") {
            toast("範圍是 turn（這一輪）、task（這次任務）或 all（整個對話）", "error");
            return;
        }
        ui(J({{"type", "review.open"}, {"id", current_}, {"scope", scope}}));
        return;
    }
    if (cmd == "/revert") {
        if (reviewBase_.empty() || reviewConv_ != current_ || util::trim(args).empty()) {
            toast("先用 /review 看變更，再用 /revert 檔案路徑", "error");
            return;
        }
        ui(J({{"type", "review.revert"},
              {"id", current_},
              {"base", reviewBase_},
              {"head", reviewHead_},
              {"path", util::trim(args)},
              {"hunk", -1}}));
        return;
    }
    if (cmd == "/undo-revert") {
        if (revertBackup_.empty()) {
            toast("這次開啟後還沒有用 /revert 還原過檔案", "error");
            return;
        }
        ui(J({{"type", "rollback.undo"}, {"convId", current_}, {"backup", revertBackup_}}));
        revertBackup_.clear();
        return;
    }
    if (cmd == "/accept") {
        if (needGroup()) ui(J({{"type", "group.accept"}, {"id", current_}}));
        return;
    }
    if (cmd == "/end") {
        if (needGroup()) ui(J({{"type", "group.end"}, {"id", current_}}));
        return;
    }
    if (cmd == "/daily") {
        if (group) { toast("日常模式只能在私訊用", "error"); return; }
        bool on = !(*c)["daily"].boolean();
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"daily", on}})}}));
        toast(on ? "日常模式：改用最省額度的模型，改檔案或執行指令前會先問你" : "回到工作模式", "ok");
        return;
    }
    if (cmd == "/rounds") {
        if (!needGroup()) return;
        int n = std::atoi(args.c_str());
        if (n < 1 || n > 20) { toast("用法：/rounds 1 到 20", "error"); return; }
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"maxRounds", n}})}}));
        toast("審查輪數上限改為 " + std::to_string(n), "ok");
        return;
    }
    if (cmd == "/plugins") {
        if (args == "scan" || args == "open") {
            ui(J({{"type", "extensions." + args}}));
            return;
        }
        if (util::startsWith(args, "disable ")) {
            ui(J({{"type", "extensions.disable"}, {"id", util::trim(args.substr(8))}}));
            return;
        }
        if (util::startsWith(args, "enable ")) {
            const auto id = util::trim(args.substr(7));
            Json found;
            for (const auto& p : extensions_["plugins"].items())
                if (p["id"].str() == id) found = p;
            if (found.isNull() || !found["errors"].empty()) {
                toast("找不到這個插件，或它有錯誤；先用 /plugins 查看", "error");
                return;
            }
            // Show everything the plugin would change before asking for approval of this exact file.
            note("插件 " + found["name"].str() + " · SHA-256 " + found["hash"].str().substr(0, 16));
            for (const auto& t : found["themes"].items()) note("  主題 " + t["id"].str() + " · " + t["name"].str());
            for (const auto& mode : found["modes"].items()) {
                note("  模式 " + mode["id"].str() + " · 設定 " + mode["settings"].dump());
                if (!mode["instructions"].str().empty())
                    note("    指示：" + util::truncateUtf8(mode["instructions"].str(), 600));
            }
            for (const auto& c : found["commands"].items())
                note("  指令 /" + c["name"].str() + "：" + util::truncateUtf8(c["template"].str(), 400));
            Ask approval;
            approval.title = "已檢視以上內容，啟用插件 " + found["name"].str() + "？輸入 ALLOW";
            approval.placeholder = "ALLOW";
            const auto hash = found["hash"].str();
            approval.done = [this, id, hash](const std::string& answer) {
                if (answer == "ALLOW") ui(J({{"type", "extensions.enable"}, {"id", id}, {"hash", hash}}));
                else toast("沒有啟用插件");
            };
            ask(std::move(approval));
            return;
        }
        note("插件資料夾：" + extensions_["folder"].str());
        if (extensions_["plugins"].empty()) note("還沒有插件；每個插件是子資料夾裡的 plugin.json（docs/plugins.md）");
        for (const auto& p : extensions_["plugins"].items()) {
            const std::string state = p["enabled"].boolean()   ? "已啟用"
                                      : p["changed"].boolean() ? "內容已改變"
                                                               : "未啟用";
            note(p["id"].str(p["folder"].str()) + " · " + p["name"].str() + " · " + state + " · 主題 " +
                 std::to_string(p["themes"].size()) + " · 模式 " + std::to_string(p["modes"].size()) + " · 指令 " +
                 std::to_string(p["commands"].size()));
            for (const auto& e : p["errors"].items()) note("  錯誤：" + e.str(), C::kWarn);
        }
        return;
    }
    if (cmd == "/plugin-mode") {
        const Json* c = conv();
        if (!c) return;
        if (args.empty()) {
            note("目前插件模式：" + ((*c)["mode"].str().empty() ? std::string("無") : (*c)["mode"].str()));
            for (const auto& mode : extensions_["modes"].items())
                note(mode["id"].str() + " · " + mode["name"].str() + " · " + mode["description"].str());
            return;
        }
        ui(J({{"type", "extensions.mode"}, {"id", current_}, {"mode", args == "off" ? std::string() : args}}));
        return;
    }
    if (cmd == "/test-guard") {
        if (!needGroup()) return;
        const Json* c = conv(current_);
        if (args.empty()) {
            const bool on = c && (*c)["regressionGuard"].boolean();
            note(std::string("測試守門：") + (on ? "開啟" : "關閉") + "；指令：" +
                 (c && !(*c)["testCommand"].str().empty() ? (*c)["testCommand"].str() : "（未設定）"));
            return;
        }
        Json settings = Json::object();
        if (args == "on" || args == "off") settings["regressionGuard"] = args == "on";
        else {
            // The command goes first so "on" sticks in the same message; the host rejects multi-line or long text.
            settings["testCommand"] = args;
            settings["regressionGuard"] = true;
        }
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", settings}}));
        toast(args == "off" ? "測試守門已關閉" : "測試守門已送出設定；審查者改檔後會執行測試", "ok");
        return;
    }
    if (cmd == "/rename") {
        if (!needGroup()) return;
        if (args.empty()) { toast("用法：/rename 新名稱", "error"); return; }
        ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"name", args}})}}));
        return;
    }
    if (cmd == "/locals") {
        if (!needGroup()) return;
        Picker p;
        p.title = "本地審查員（唯讀，只看 diff 提建議）";
        p.footer = "Enter 邀請或移除 · Esc 完成";
        std::set<std::string> in;
        for (const auto& l : (*c)["localMembers"].items()) in.insert(l.str());
        for (const char* slot : {"local1", "local2"}) {
            std::string model = settings_["locals"][slot]["model"].str();
            Item it{name(slot), model.empty() ? "還沒選模型（在桌面版 設定 › 本地模型）" : model, slot, tui::agentColor(slot)};
            it.checkable = true;
            it.checked = in.count(slot) > 0;
            p.items.push_back(it);
        }
        p.onPick = [this](Item& it) {
            it.checked = !it.checked;
            Json members = Json::array();
            if (picking_) {
                for (const auto& x : picking_->items) {
                    if (x.checked) members.push(x.value);
                }
            }
            ui(J({{"type", "conv.settings"}, {"id", current_}, {"settings", J({{"localMembers", members}})}}));
            return true;
        };
        openPicker(std::move(p));
        return;
    }
    if (cmd == "/clear") {
        Picker p;
        p.title = "清除「" + convTitle(*c) + "」？";
        p.items.push_back({"取消", "", "no"});
        p.items.push_back({"清除", "刪除所有訊息，AI 也會忘記之前的內容", "yes", C::kErr});
        p.onPick = [this](Item& it) {
            if (it.value == "yes") ui(J({{"type", "conv.reset"}, {"id", current_}}));
            return false;
        };
        openPicker(std::move(p));
        return;
    }
    if (cmd == "/show") {
        tui::Names names{[this](const std::string& w) { return name(w); }};
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
            std::string a = (*it)["author"].str();
            if (a == "user" || a == "system" || (*it)["blocks"].size() == 0) continue;
            commit(tui::renderDetails(*it, names, std::max(term::cols() - 1, 20)));
            return;
        }
        toast("還沒有回覆可以顯示");
        return;
    }
    if (cmd == "/name") {
        pickAgent(
            "要改誰的名稱？",
            [this](const std::string& who) {
                Ask a;
                a.title = "新的名稱（" + name(who) + "）";
                a.placeholder = "留空恢復預設名稱";
                a.allowEmpty = true;
                a.done = [this, who](const std::string& n) {
                    ui(J({{"type", "profile.set"}, {"who", who}, {"name", n}}));
                    toast("名稱改為 " + name(who), "ok");
                };
                ask(std::move(a), name(who));
            },
            true, true, true);
        return;
    }
    if (cmd == "/persona" || cmd == "/settings") {
        pickAgent("要設定誰的性格？", [this](const std::string& who) { pickPersona(who); }, true, false, true);
        return;
    }
    if (cmd == "/humanize") {
        if (!args.empty() && args != "on" && args != "off") { toast("用法：/humanize on 或 /humanize off", "error"); return; }
        bool on = args == "on" ? true : args == "off" ? false : !settings_["humanize"]["enabled"].boolean();
        ui(J({{"type", "humanize.set"}, {"enabled", on}}));
        toast(on ? "去 AI 化已開啟：做網頁、美術和文字時避開 AI 預設風格" : "去 AI 化已關閉", "ok");
        return;
    }
    // A CCA plugin command goes to the conversation as typed; the host replaces it with the plugin's template.
    for (const auto& command : extensions_["commands"].items()) {
        if ("/" + command["name"].str() != cmd) continue;
        if (!conv()) {
            toast("還沒有對話", "error");
            return;
        }
        ui(J({{"type", "conv.send"}, {"id", current_}, {"text", cmd + (args.empty() ? "" : " " + args)}}));
        return;
    }
    toast("沒有這個指令：" + cmd + "（/help 看所有指令）", "error");
}

// ------------------------------------------------------------ run

int Cli::run(int argc, wchar_t** argv) {
    // Before the console is set up: wide output to a console, UTF-8 bytes to a pipe or file.
    auto say = [](const std::string& s) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD n = 0, mode = 0;
        std::wstring w = util::widen(s);
        if (GetConsoleMode(out, &mode)) WriteConsoleW(out, w.c_str(), (DWORD)w.size(), &n, nullptr);
        else WriteFile(out, s.data(), (DWORD)s.size(), &n, nullptr);
    };
    std::string want;
    for (int i = 1; i < argc; ++i) {
        std::string a = util::narrow(argv[i]);
        if (a == "--help" || a == "-h" || a == "/?") {
            say(std::string(CCA_PRODUCT) + " CLI " + CCA_VERSION + "\n\n用法：ChatClaudeAgent-CLI [claude | gpt | group | 群組名稱]\n\n"
                "在終端機裡和 Claude、GPT 對話，或讓兩個一起寫程式。對話和桌面版共用，兩邊不能同時開著。\n"
                "開啟後輸入 /help 看所有指令。\n");
            return 0;
        }
        if (a == "--version" || a == "-v") {
            say(std::string(CCA_PRODUCT) + (*CCA_EDITION_NAME ? " " CCA_EDITION_NAME : "") + " CLI " + CCA_VERSION +
                "\n");
            return 0;
        }
        want = a;
    }
    std::string err;
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) systemMotion_ = animations != FALSE;
    if (!term::init(&err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    // The desktop window from an older build only holds its own single-instance lock.
    if (util::getEnv(L"CCA_DATA_DIR").empty()) {
        if (HANDLE old = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\CCA.SingleInstance")) {
            CloseHandle(old);
            term::write("\x1b[38;2;221;169;75m! ChatClaudeAgent 桌面版正在執行，它和 CLI 共用同一份對話資料。請先關閉桌面版，再開啟 CLI。\x1b[0m\r\n");
            term::restore();
            return 1;
        }
    }
    void* lock = util::lockDataDir();
    if (!lock) {
        term::write("\x1b[38;2;221;169;75m! 另一個 ChatClaudeAgent（桌面版或 CLI）正在使用同一份對話資料，請先關閉它。\x1b[0m\r\n");
        term::restore();
        return 1;
    }

    wchar_t oldTitle[512] = L"";
    GetConsoleTitleW(oldTitle, 512);
    {
        wchar_t cwd[MAX_PATH * 4];
        DWORD n = GetCurrentDirectoryW(MAX_PATH * 4, cwd);
        launchDir_ = std::wstring(cwd, n);
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onConsoleClose, TRUE);

    app_ = std::make_unique<App>([this](const std::string& s) { inbox_.push_back(s); });
    app_->hostFlavor = "cli";
    app_->windowAction = [this](const std::string& action) {
        if (action == "close") quit_ = true;
        if (action == "flash") term::write("\a");  // the terminal's own "needs attention"
    };
    app_->openExternal = [](const std::wstring& target) { ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    ui(J({{"type", "ready"}}));
    printHeader();

    // Which conversation: the argument, else the last one used, else the group.
    std::string pick;
    std::string w = toLowerAscii(want);
    for (const auto& c : convs_.items()) {
        std::string kind = c["kind"].str();
        if ((w == "claude" && kind == "claude") || ((w == "gpt" || w == "codex") && kind == "codex") ||
            (w == "gemini" && kind == "gemini"))
            pick = c["id"].str();
        if (pick.empty() && !w.empty() && kind == "group" && (w == "group" || toLowerAscii(c["name"].str()) == w)) pick = c["id"].str();
    }
    if (!want.empty() && pick.empty()) note("找不到對話「" + want + "」，開啟上次的對話", C::kWarn, "!");
    if (pick.empty() && conv(settings_["lastConv"].str())) pick = settings_["lastConv"].str();
    if (pick.empty()) {
        for (const auto& c : convs_.items()) {
            if (c["kind"].str() == "group") { pick = c["id"].str(); break; }
        }
    }
    open(pick);

    long long nextTick = 0;
    while (!quit_ && !g_closing) {
        HANDLE hs[2] = {term::input(), g_wake};
        WaitForMultipleObjects(2, hs, FALSE, 50);
        for (const auto& k : term::readKeys()) {
            handleKey(k);
            drain();
            if (quit_) break;
        }
        std::deque<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> l(g_mu);
            q.swap(g_queue);
        }
        for (auto& f : q) f();
        long long now = util::nowMs();
        if (now >= nextTick) {
            app_->tick();
            nextTick = now + 50;
        }
        drain();
        if (!toast_.empty() && now >= toastUntil_) { toast_.clear(); dirty_ = true; }
        bool animate = running() && now - lastFrame_ >= (motionOn() && tui::workflowAnimating(state()["workflow"], now) ? 90 : 1000);
        if (term::cols() != lastCols_) dirty_ = true;
        if ((dirty_ && now - lastFrame_ >= 30) || animate) frame();
    }

    // Leave the screen tidy: the live region goes, a one-line goodbye stays.
    app_->shutdown();
    drain();
    picker_.reset();
    ask_.reset();
    std::string out = "\r";
    int up = 0;
    for (int i = 0; i < caretRow_ && i < (int)liveWidths_.size(); ++i) up += std::max(1, (liveWidths_[i] + term::cols() - 1) / term::cols());
    if (up > 0) out += "\x1b[" + std::to_string(up) + "A";
    out += "\x1b[J" + pendingOut_;
    out += tui::ansi({{tui::faint(), "  已離開 " CCA_PRODUCT "，對話已儲存。"}}) + "\r\n";
    term::write(out);
    SetConsoleTitleW(oldTitle);
    term::restore();
    app_.reset();
    CloseHandle((HANDLE)lock);
    SetEvent(g_closed);
    return 0;
}

}  // namespace

void postToUi(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_queue.push_back(std::move(fn));
    }
    if (g_wake) SetEvent(g_wake);
}

int wmain(int argc, wchar_t** argv) {
    int updateResult = updater::helperMain();
    if (updateResult >= 0) return updateResult;
    if (argc == 3 && std::wstring(argv[1]) == L"--cca-update-started") argc = 1;
    Cli cli;
    return cli.run(argc, argv);
}
