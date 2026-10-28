#include "cli_render.hpp"
#include "themes.hpp"

#include <algorithm>
#include <cstdio>

#include "logo_art.hpp"
#include "util.hpp"

namespace tui {

namespace {
uint32_t geminiColor = 0x88b6ff;

struct Range {
    char32_t lo, hi;
};

// Zero-width: combining marks, joiners, variation selectors, skin tones.
const Range kZero[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0x302A, 0x302D}, {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0000, 0xE0FFF},
};

// East Asian Wide and Fullwidth, plus emoji shown as wide. Ambiguous characters
// (box drawing, ●, …) stay narrow, the way Windows Terminal draws them.
const Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x2E99},
    {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x2FFF}, {0x3000, 0x303E}, {0x3041, 0x3096}, {0x3099, 0x30FF},
    {0x3105, 0x312F}, {0x3131, 0x318E}, {0x3190, 0x31E3}, {0x31EF, 0x321E}, {0x3220, 0x3247}, {0x3250, 0x4DBF},
    {0x4E00, 0xA48C}, {0xA490, 0xA4C6}, {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE52}, {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x16FF0, 0x16FF1}, {0x17000, 0x187F7}, {0x18800, 0x18CD5}, {0x18D00, 0x18D08}, {0x1AFF0, 0x1B2FF},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202},
    {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

template <size_t N>
bool inRanges(const Range (&ranges)[N], char32_t c) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < ranges[mid].lo) hi = mid;
        else if (c > ranges[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

bool isWide(char32_t c) { return c >= 0x1100 && inRanges(kWide, c); }

std::string hex6(uint32_t rgb) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u;%u;%u", (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    return buf;
}

bool themeActive = false;
uint32_t themeForeground = 0, themeBackground = 0;

std::string sgr(const Style& s) {
    std::string out = "\x1b[0";
    if (s.bold) out += ";1";
    if (s.italic) out += ";3";
    if (s.underline) out += ";4";
    if (s.strike) out += ";9";
    if (s.hasFg || themeActive) out += ";38;2;" + hex6(s.hasFg ? s.fg : themeForeground);
    if (s.hasBg || themeActive) out += ";48;2;" + hex6(s.hasBg ? s.bg : themeBackground);
    return out + "m";
}

// A styled character, the unit the wrapper works with.
struct Cell {
    char32_t c;
    int style;
};

void flatten(const Line& l, std::vector<Style>& styles, std::vector<Cell>& cells) {
    for (const auto& sp : l) {
        int idx = -1;
        for (size_t i = 0; i < styles.size(); ++i) {
            if (styles[i] == sp.style) { idx = (int)i; break; }
        }
        if (idx < 0) { styles.push_back(sp.style); idx = (int)styles.size() - 1; }
        for (char32_t c : decode(sp.text)) {
            if (c == '\t') {
                for (int k = 0; k < 4; ++k) cells.push_back({U' ', idx});
            } else if (c != '\r') {
                cells.push_back({c, idx});
            }
        }
    }
}

Line toLine(const std::vector<Cell>& cells, size_t from, size_t to, const std::vector<Style>& styles) {
    Line out;
    for (size_t i = from; i < to; ++i) {
        const Style& st = styles[cells[i].style];
        std::u32string one(1, cells[i].c);
        if (!out.empty() && out.back().style == st) out.back().text += encode(one);
        else out.push_back({st, encode(one)});
    }
    return out;
}

Line concat(Line a, const Line& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// ---- inline Markdown: `code`, **bold**, *italic*, ~~strike~~, [text](url)

Line inlineSpans(const std::string& text, const Style& base) {
    Line out;
    std::u32string s = decode(text);
    std::u32string cur;
    Style st = base;
    auto flush = [&] {
        if (!cur.empty()) out.push_back({st, encode(cur)});
        cur.clear();
    };
    bool bold = false, italic = false, strike = false;
    auto restyle = [&] {
        flush();
        st = base;
        st.bold = base.bold || bold;
        st.italic = italic;
        st.strike = strike;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c == '`') {
            size_t end = s.find(U'`', i + 1);
            if (end != std::u32string::npos) {
                flush();
                Style code = fg(color::kBrand);
                code.bold = bold;
                out.push_back({code, encode(s.substr(i + 1, end - i - 1))});
                i = end;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            size_t end = s.find(std::u32string(2, c), i + 2);
            if (bold || end != std::u32string::npos) {
                bold = !bold;
                restyle();
                ++i;
                continue;
            }
        }
        if (c == '*' && (italic || (i + 1 < s.size() && s[i + 1] != ' ' && s.find(U'*', i + 1) != std::u32string::npos))) {
            italic = !italic;
            restyle();
            continue;
        }
        if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') {
            size_t end = s.find(U"~~", i + 2);
            if (strike || end != std::u32string::npos) {
                strike = !strike;
                restyle();
                ++i;
                continue;
            }
        }
        if (c == '[') {
            size_t close = s.find(U']', i + 1);
            if (close != std::u32string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t paren = s.find(U')', close + 2);
                if (paren != std::u32string::npos) {
                    flush();
                    Style link = st;
                    link.underline = true;
                    out.push_back({link, encode(s.substr(i + 1, close - i - 1))});
                    std::u32string url = s.substr(close + 2, paren - close - 2);
                    if (!url.empty()) out.push_back({faint(), encode(U" (" + url + U")")});
                    i = paren;
                    continue;
                }
            }
        }
        cur += c;
    }
    flush();
    return out;
}

size_t leadingSpaces(const std::string& s) {
    size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) n += 1;
    return n;
}

bool isRule(const std::string& t) {
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (char x : t) {
        if (x == c) ++count;
        else if (x != ' ') return false;
    }
    return count >= 3;
}

Line toolLine(const Json& b, int cols) {
    std::string status = b["status"].str();
    Line meta;
    if (status == "running") meta.push_back({muted(), " …"});
    else if (status == "ok") meta.push_back({fg(color::kOk), " ✓"});
    else if (status == "declined") meta.push_back({fg(color::kWarn), " 已拒絕"});
    else if (status == "interrupted") meta.push_back({fg(color::kWarn), " 已中斷"});
    else meta.push_back({fg(color::kErr), " ✗"});
    if (b.has("exitCode") && b["exitCode"].i64() != 0) meta.push_back({fg(color::kErr), " 結束碼 " + std::to_string(b["exitCode"].i64())});
    if (status != "running" && b["durationMs"].i64() > 0) meta.push_back({faint(), " " + duration(b["durationMs"].i64())});
    std::string title = b["title"].str(b["tool"].str("工具"));
    std::string detail;
    if (!b["command"].str().empty()) detail = util::splitLines(b["command"].str()).empty() ? "" : util::splitLines(b["command"].str())[0];
    else if (!b["path"].str().empty()) detail = b["path"].str();
    else detail = util::replaceAll(b["detail"].str(), "\n", " ");
    Line l = {{faint(), "  ⎿ "}, {Style(), title}};
    int room = cols - width(l) - width(meta) - 2;
    if (!detail.empty() && room > 4) l.push_back({fg(color::kCode), "  " + clipText(detail, room)});
    return concat(l, meta);
}

Line fileLine(const Json& b, int cols) {
    std::string change = b["change"].str();
    std::string label = change == "add" ? "新增" : change == "delete" ? "刪除" : "修改";
    uint32_t c = change == "add" ? color::kOk : change == "delete" ? color::kErr : color::kBrand;
    Line tail = {{fg(color::kOk), "  +" + std::to_string(b["added"].i64())}, {fg(color::kErr), " −" + std::to_string(b["removed"].i64())}};
    Line l = {{faint(), "  ⎿ "}, {fg(c), label}};
    int room = cols - width(l) - width(tail) - 2;
    l.push_back({fg(color::kCode), "  " + clipText(b["path"].str(), std::max(room, 4))});
    return concat(l, tail);
}

std::string approvalState(const std::string& state) {
    if (state == "allow") return "已允許";
    if (state == "allowSession") return "已允許（本次對話不再詢問同類操作）";
    if (state == "deny") return "已拒絕";
    if (state == "abort") return "已拒絕並停止";
    if (state == "cancelled") return "已取消";
    return state;
}

std::string answersText(const Json& answers) {
    std::string out;
    for (const auto& f : answers.fields()) {
        std::string v;
        if (f.second.isArray()) {
            for (const auto& x : f.second.items()) v += (v.empty() ? "" : "、") + x.str();
        } else {
            v = f.second.str();
        }
        out += (out.empty() ? "" : "；") + v;
    }
    return out;
}

std::vector<Line> todoLines(const Json& b, int cols) {
    std::vector<Line> out;
    int done = 0, total = (int)b["items"].size();
    for (const auto& it : b["items"].items()) done += it["status"].str() == "completed";
    out.push_back({{faint(), "  ⎿ "}, {Style(), "待辦"}, {muted(), "  " + std::to_string(done) + "/" + std::to_string(total)}});
    for (const auto& it : b["items"].items()) {
        std::string s = it["status"].str();
        Style st = s == "completed" ? muted() : s == "inProgress" ? fg(color::kBrand, true) : Style();
        if (s == "completed") st.strike = true;
        std::string box = s == "completed" ? "☒ " : s == "inProgress" ? "◐ " : "☐ ";
        for (auto& l : wrap({{st, it["text"].str()}}, cols, {{faint(), "    "}, {s == "inProgress" ? fg(color::kBrand) : muted(), box}}, line("      "))) {
            out.push_back(std::move(l));
        }
    }
    return out;
}

std::string modelName(const Json& m) {
    std::string model = m["resolvedModel"].str();
    if (model.empty()) model = m["model"].str();
    return model;
}

}  // namespace

void setTheme(const std::string& id) {
    Json theme = themeById(id);
    geminiColor = theme.isNull() ? 0x88b6ff : themeColor(theme, "gemini");
    themeActive = !theme.isNull() && id != "cca";
    using namespace color;
    if (!themeActive) {
        kBrand = 0xE9EDF1;
        kBrandDeep = 0x7CC4FF;
        kClaude = 0xD97757;
        kGpt = 0x19B88F;
        kLocal1 = 0xEB6FB0;
        kLocal2 = 0xA894F0;
        kMuted = 0x8B949E;
        kFaint = 0x5C636B;
        kCode = 0xC9D4E0;
        kOk = 0x4FBF7F;
        kErr = 0xE5675E;
        kWarn = 0xDDA94B;
        return;
    }
    themeForeground = themeColor(theme, "text");
    themeBackground = themeColor(theme, "bg-chat");
    kBrand = themeColor(theme, "brand");
    kBrandDeep = themeColor(theme, "signal");
    kClaude = themeColor(theme, "claude-name");
    kGpt = themeColor(theme, "codex-name");
    kLocal1 = themeColor(theme, "local1-name");
    kLocal2 = themeColor(theme, "local2-name");
    kMuted = themeColor(theme, "text-muted");
    kFaint = themeColor(theme, "text-faint");
    kCode = themeColor(theme, "text");
    kOk = themeColor(theme, "green");
    kErr = themeColor(theme, "red");
    kWarn = themeColor(theme, "yellow");
}

bool Style::operator==(const Style& o) const {
    return fg == o.fg && bg == o.bg && hasFg == o.hasFg && hasBg == o.hasBg && bold == o.bold && italic == o.italic &&
           underline == o.underline && strike == o.strike;
}

Style fg(uint32_t rgb, bool bold) {
    Style s;
    s.fg = rgb;
    s.hasFg = true;
    s.bold = bold;
    return s;
}
Style muted() { return fg(color::kMuted); }
Style faint() { return fg(color::kFaint); }

Line line(const std::string& text, Style st) { return text.empty() ? Line{} : Line{{st, text}}; }

int charWidth(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
    if (c < 0x300) return 1;
    if (inRanges(kZero, c)) return 0;
    return isWide(c) ? 2 : 1;
}

std::u32string decode(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { out += U'�'; ++i; continue; }
        if (i + len > s.size()) { out += U'�'; break; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out += U'�'; ++i; continue; }
        out += cp;
        i += len;
    }
    return out;
}

std::string encode(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

int width(const std::string& s) {
    int w = 0;
    for (char32_t c : decode(s)) w += c == '\t' ? 4 : charWidth(c);
    return w;
}

int width(const Line& l) {
    int w = 0;
    for (const auto& sp : l) w += width(sp.text);
    return w;
}

std::string ansi(const Line& l) {
    std::string out;
    for (const auto& sp : l) {
        if (sp.text.empty()) continue;
        out += sgr(sp.style);
        // Tool output, names and model text are data. Only our renderer may emit
        // terminal controls; an embedded ESC/OSC must never redraw a permission prompt.
        for (char32_t c : decode(sp.text)) {
            if (c == '\t') out += "    ";
            else if (c >= 0x20 && !(c >= 0x7F && c < 0xA0)) out += encode(std::u32string(1, c));
        }
    }
    if (themeActive) out += sgr(Style()) + "\x1b[K";
    if (!l.empty() || themeActive) out += "\x1b[0m";
    return out;
}

Line clip(const Line& l, int cols) {
    if (width(l) <= cols) return l;
    Line out;
    int used = 0;
    int room = std::max(cols - 1, 0);
    for (const auto& sp : l) {
        std::u32string kept;
        for (char32_t c : decode(sp.text)) {
            int w = charWidth(c);
            if (used + w > room) break;
            kept += c;
            used += w;
        }
        if (!kept.empty()) out.push_back({sp.style, encode(kept)});
        if (used >= room || kept.size() < decode(sp.text).size()) {
            out.push_back({sp.style, "…"});
            return out;
        }
    }
    return out;
}

std::string clipText(const std::string& s, int cols) {
    Line l = clip(line(s), cols);
    std::string out;
    for (const auto& sp : l) out += sp.text;
    return out;
}

Line padTo(const Line& l, int cols) {
    Line out = clip(l, cols);
    int w = width(out);
    if (w < cols) out.push_back({Style(), std::string(cols - w, ' ')});
    return out;
}

std::vector<Line> wrap(const Line& content, int cols, const Line& first, const Line& rest) {
    std::vector<Style> styles;
    std::vector<Cell> cells;
    flatten(content, styles, cells);
    std::vector<Line> out;
    const size_t n = cells.size();
    size_t pos = 0;
    bool firstLine = true;
    while (true) {
        const Line& prefix = firstLine ? first : rest;
        int avail = std::max(cols - width(prefix), 4);
        size_t i = pos, lastBreak = std::string::npos;
        int w = 0;
        while (i < n && cells[i].c != '\n') {
            int cw = charWidth(cells[i].c);
            if (w + cw > avail) break;
            w += cw;
            char32_t c = cells[i].c;
            if (c == ' ' || isWide(c) || (i + 1 < n && isWide(cells[i + 1].c))) lastBreak = i + 1;
            ++i;
        }
        size_t end, next;
        bool newline = false;
        if (i >= n) {
            end = next = n;
        } else if (cells[i].c == '\n') {
            end = i;
            next = i + 1;
            newline = true;
        } else if (cells[i].c == ' ') {
            end = next = i;  // the text fits exactly up to a space
        } else if (lastBreak != std::string::npos && lastBreak > pos) {
            end = next = lastBreak;
        } else {
            end = next = std::max(i, pos + 1);  // one word wider than the line: cut it
        }
        size_t trimmed = end;
        while (trimmed > pos && cells[trimmed - 1].c == ' ') --trimmed;
        out.push_back(concat(prefix, toLine(cells, pos, trimmed, styles)));
        firstLine = false;
        pos = next;
        if (!newline) {
            while (pos < n && cells[pos].c == ' ') ++pos;
        }
        if (pos >= n) break;
    }
    return out;
}

std::vector<MdLine> markdown(const std::string& text, int cols, const Line& prefix) {
    std::vector<MdLine> out;
    std::vector<std::string> src;
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                std::string l = text.substr(start, i - start);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                src.push_back(l);
                start = i + 1;
            }
        }
    }
    bool fence = false;
    bool lastBlank = true;  // no blank line at the very top
    auto emit = [&](std::vector<Line> lines, int idx) {
        for (auto& l : lines) out.push_back({std::move(l), idx});
        lastBlank = false;
    };
    for (int idx = 0; idx < (int)src.size(); ++idx) {
        const std::string& raw = src[idx];
        std::string t = util::trim(raw);
        if (util::startsWith(t, "```") || util::startsWith(t, "~~~")) {
            fence = !fence;
            continue;
        }
        if (fence) {
            Line bar = concat(prefix, {{faint(), "│ "}});
            emit(wrap({{fg(color::kCode), raw}}, cols, bar, bar), idx);
            continue;
        }
        if (t.empty()) {
            if (!lastBlank) out.push_back({Line{}, idx});
            lastBlank = true;
            continue;
        }
        if (t[0] == '#') {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            if (h <= 6 && h < t.size() && t[h] == ' ') {
                Style st = h <= 2 ? fg(color::kBrand, true) : Style();
                st.bold = true;
                emit(wrap(inlineSpans(util::trim(t.substr(h)), st), cols, prefix, prefix), idx);
                continue;
            }
        }
        if (isRule(t)) {
            int w = std::min(cols - width(prefix), 48);
            std::string rule;
            for (int k = 0; k < w; ++k) rule += "─";
            emit({concat(prefix, {{faint(), rule}})}, idx);
            continue;
        }
        if (t[0] == '>') {
            std::string body = util::trim(t.substr(1));
            Line bar = concat(prefix, {{faint(), "▎ "}});
            Style st = muted();
            st.italic = true;
            emit(wrap(inlineSpans(body, st), cols, bar, bar), idx);
            continue;
        }
        if (t[0] == '|') {
            bool sep = t.find_first_not_of("|-: ") == std::string::npos;
            if (sep) {
                int w = std::min(cols - width(prefix), width(t));
                std::string rule;
                for (int k = 0; k < w; ++k) rule += "─";
                emit({concat(prefix, {{faint(), rule}})}, idx);
            } else {
                emit(wrap(inlineSpans(t, Style()), cols, prefix, prefix), idx);
            }
            continue;
        }
        size_t lead = leadingSpaces(raw);
        std::string indent((lead / 2) * 2, ' ');
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            Line first = concat(prefix, {{Style(), indent}, {fg(color::kBrand), "• "}});
            Line rest = concat(prefix, {{Style(), indent + "  "}});
            emit(wrap(inlineSpans(util::trim(t.substr(2)), Style()), cols, first, rest), idx);
            continue;
        }
        size_t d = 0;
        while (d < t.size() && t[d] >= '0' && t[d] <= '9') ++d;
        if (d > 0 && d < 4 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
            std::string num = t.substr(0, d + 1) + " ";
            Line first = concat(prefix, {{Style(), indent}, {fg(color::kBrand), num}});
            Line rest = concat(prefix, {{Style(), indent + std::string(num.size(), ' ')}});
            emit(wrap(inlineSpans(util::trim(t.substr(d + 2)), Style()), cols, first, rest), idx);
            continue;
        }
        Line p = concat(prefix, {{Style(), indent}});
        emit(wrap(inlineSpans(t, Style()), cols, p, p), idx);
    }
    while (!out.empty() && out.back().line.empty()) out.pop_back();
    return out;
}

uint32_t agentColor(const std::string& who) {
    if (who == "claude") return color::kClaude;
    if (who == "codex") return color::kGpt;
    if (who == "gemini") return geminiColor;
    if (who == "local1") return color::kLocal1;
    if (who == "local2") return color::kLocal2;
    return color::kBrand;
}

std::string withAgentName(const std::string& title, const Names& names) {
    if (util::startsWith(title, "Claude")) return names.of("claude") + " " + util::trim(title.substr(6));
    if (util::startsWith(title, "GPT")) return names.of("codex") + " " + util::trim(title.substr(3));
    if (util::startsWith(title, "Gemini")) return names.of("gemini") + " " + util::trim(title.substr(6));
    return title;
}

std::string handoffText(const Json& m, const Names& names) {
    std::string from = m["from"].str(), to = m["to"].str(), reason = m["reason"].str();
    if (from.empty() || to.empty() || reason.empty()) return m["text"].str();
    std::string a = names.of(from), b = names.of(to);
    if (reason == "first") return a + " 把第一版交給 " + b + " 審查";
    if (reason == "changed") return a + " 修改了 " + std::to_string(m["files"].i64()) + " 個檔案，交給 " + b + " 複審";
    if (reason == "answered") return a + " 回應了 " + b + " 的疑慮（沒有修改），交給 " + b + " 確認";
    if (reason == "issues") return a + " 提出了疑慮，交給 " + b + " 處理";
    return m["text"].str();
}

std::string tokens(int64_t n) {
    char buf[32];
    if (n >= 1000000) std::snprintf(buf, sizeof buf, "%.1fM", n / 1e6);
    else if (n >= 10000) std::snprintf(buf, sizeof buf, "%.0fk", n / 1e3);
    else if (n >= 1000) std::snprintf(buf, sizeof buf, "%.1fk", n / 1e3);
    else std::snprintf(buf, sizeof buf, "%lld", (long long)n);
    return buf;
}

std::string duration(int64_t ms) {
    char buf[48];
    if (ms < 1000) std::snprintf(buf, sizeof buf, "%lld 毫秒", (long long)ms);
    else if (ms < 10000) std::snprintf(buf, sizeof buf, "%.1f 秒", ms / 1000.0);
    else if (ms < 60000) std::snprintf(buf, sizeof buf, "%lld 秒", (long long)(ms / 1000));
    else std::snprintf(buf, sizeof buf, "%lld 分 %lld 秒", (long long)(ms / 60000), (long long)(ms / 1000 % 60));
    return buf;
}

Rendered renderMessage(const Json& m, const Names& names, int cols) {
    Rendered r;
    auto add = [&](Line l) { r.lines.push_back(std::move(l)); };
    auto addAll = [&](std::vector<Line> v) {
        for (auto& l : v) r.lines.push_back(std::move(l));
    };
    std::string author = m["author"].str();
    const Line indent = line("  ");

    if (author == "user") {
        add({});
        addAll(wrap(line(m["text"].str()), cols, {{fg(color::kBrand, true), "› "}}, indent));
        r.stable = r.lines.size();
        r.complete = true;
        return r;
    }

    if (author == "system") {
        std::string kind = m["kind"].str();
        add({});
        if (kind == "handoff") {
            addAll(wrap({{muted(), handoffText(m, names)}}, cols, {{fg(color::kBrand), "  ⇄ "}}, line("    ")));
        } else if (kind == "paused") {
            addAll(wrap({{fg(color::kWarn), m["text"].str()}}, cols, {{fg(color::kWarn), "  ‖ "}}, line("    ")));
        } else if (kind == "skip") {
            // Skips stay one muted line with a fixed tag, so a long run of exhausted members stays readable.
            addAll(wrap({{fg(color::kWarn, true), "跳過 "}, {Style(), m["text"].str()}}, cols,
                        {{fg(color::kWarn), "  » "}}, line("    ")));
        } else if (kind == "summary") {
            bool ok = (m["outcome"].str() == "consensus" || m["outcome"].str() == "completed");
            bool quota = m["outcome"].str() == "quota";
            Line body = {{Style(), (ok      ? "協作完成："
                                    : quota ? "額度用完，強制停止："
                                            : "協作結束：") +
                                       m["text"].str()},
                         {muted(), " · " + std::to_string(m["rounds"].i64()) + " 輪審查"}};
            addAll(wrap(body, cols,
                        {{fg(ok      ? color::kOk
                             : quota ? color::kErr
                                     : color::kMuted),
                          ok ? "  ✓ " : "  ■ "}},
                        line("    ")));
            int shown = 0;
            for (const auto& f : m["files"].items()) {
                if (++shown > 12) {
                    add({{muted(), "    … 還有 " + std::to_string(m["files"].size() - 12) + " 個檔案"}});
                    break;
                }
                Json b = f;
                Line fl = fileLine(b, cols - 2);
                fl.insert(fl.begin(), {Style(), "  "});
                add(fl);
            }
        } else if (kind == "guard") {
            uint32_t tone = m["final"].boolean() ? color::kErr : color::kWarn;
            addAll(wrap({{fg(tone), m["text"].str()}}, cols, {{fg(tone), "  ⚑ "}}, line("    ")));
            for (const auto& i : m["issues"].items())
                addAll(wrap({{muted(), i["text"].str()}}, cols, line("    - "), line("      ")));
        } else if (kind == "regression") {
            // The reviewer test guard: one tagged line, then the tail of the failing output.
            const std::string status = m["status"].str();
            uint32_t tone = status == "passed"                                     ? color::kOk
                            : (status == "reverted" || status == "restore-failed") ? color::kErr
                                                                                   : color::kWarn;
            const bool gate = m["gate"].boolean();
            if (gate && status == "gate-red") tone = color::kErr;
            const char* tag = gate && status == "passed"     ? "完成閘門通過 "
                              : gate && status == "gate-red" ? "完成閘門未過 "
                              : status == "passed"           ? "測試通過 "
                              : status == "reverted"         ? "已還原 "
                              : status == "baseline-red"     ? "基準也是紅 "
                              : status == "restore-failed"   ? "還原失敗 "
                                                             : "沒有驗證 ";
            addAll(wrap({{fg(tone, true), tag}, {Style(), m["text"].str()}}, cols, {{fg(tone), "  ⚑ "}}, line("    ")));
            if (status != "passed") {
                int shown = 0;
                std::string tail = m["tail"].str();
                size_t at = 0;
                std::vector<std::string> rows;
                while (at < tail.size()) {
                    size_t end = tail.find('\n', at);
                    if (end == std::string::npos) end = tail.size();
                    rows.push_back(tail.substr(at, end - at));
                    at = end + 1;
                }
                for (size_t i = rows.size() > 8 ? rows.size() - 8 : 0; i < rows.size(); ++i, ++shown)
                    add({{muted(), "    │ " + rows[i]}});
            }
        } else if (kind == "error") {
            addAll(wrap({{fg(color::kErr), m["text"].str()}}, cols, {{fg(color::kErr), "  ✗ "}}, line("    ")));
        } else if (kind == "artifact") {
            for (const auto& block : m["blocks"].items())
                if (block["type"].str() == "image")
                    addAll(wrap({{muted(), "測試圖片：" + block["artifact"]["file"].str() + "（桌面版可預覽）"}}, cols,
                                line("  "), line("    ")));
        } else if (kind == "rollback") {
            addAll(wrap({{Style(), m["text"].str()}}, cols, {{fg(color::kBrand), "  ↺ "}}, line("    ")));
        } else {
            addAll(wrap({{muted(), m["text"].str()}}, cols, {{muted(), "  · "}}, line("    ")));
        }
        r.stable = r.lines.size();
        r.complete = true;
        return r;
    }

    // An agent's reply.
    add({});
    uint32_t c = agentColor(author);
    std::string label = m["agent_label"].str(), profile = names.of(author);
    if (label.empty()) label = profile;
    else if (label != profile) label += " · " + profile;
    Line head = {{fg(c), "● "}, {fg(c, true), label}};
    if (!m["billing"].str().empty()) head.push_back({muted(), " · " + m["billing"].str()});
    static const char* phases[][2] = {{"selfcheck", "自查"}, {"write", "撰寫"},  {"review", "審查"},
                                      {"recheck", "複審"},   {"verify", "驗證"}, {"advise", "唯讀審查"}};
    std::string tag;
    for (auto& p : phases) {
        if (m["phase"].str() == p[0]) tag = p[1];
    }
    if (!tag.empty() && m.has("round")) tag += " · 第 " + std::to_string(m["round"].i64()) + " 輪";
    if (tag.empty() && (author == "local1" || author == "local2")) tag = "唯讀審查";
    if (!tag.empty()) head.push_back({muted(), "  " + tag});
    if (m["daily"].boolean()) head.push_back({fg(color::kBrand), "  日常"});
    // CCA's compliance checks on this reply (guard.hpp).
    std::string guard = m["guard"]["status"].str();
    if (guard == "rejected") head.push_back({fg(color::kErr), "  未通過規範"});
    else if (guard == "accepted") head.push_back({fg(color::kWarn), "  已接受"});
    else if (guard == "passed") head.push_back({fg(color::kOk), "  已修正"});
    add(head);

    bool streaming = m["streaming"].boolean();
    size_t stable = std::string::npos;
    auto unstableAt = [&](size_t n) {
        if (stable == std::string::npos) stable = n;
    };
    const Json* todo = nullptr;
    const auto& blocks = m["blocks"].items();
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Json& b = blocks[i];
        std::string type = b["type"].str();
        bool last = i + 1 == blocks.size();
        if (type == "text") {
            const std::string& text = b["text"].strRef();
            if (util::trim(text).empty()) continue;
            std::vector<MdLine> md = markdown(text, cols, indent);
            bool open = streaming && last && !b["done"].boolean();
            int completeSrc = (int)std::count(text.begin(), text.end(), '\n');
            for (auto& l : md) {
                if (open && l.src >= completeSrc) unstableAt(r.lines.size());
                add(std::move(l.line));
            }
        } else if (type == "tool") {
            if (b["status"].str() == "running") unstableAt(r.lines.size());
            add(toolLine(b, cols));
            if (b["status"].str() != "running" && b["status"].str() != "ok" && b["status"].str() != "declined") {
                std::vector<std::string> out;
                for (const auto& l : util::splitLines(b["output"].str())) {
                    if (!util::trim(l).empty()) out.push_back(l);
                }
                size_t from = out.size() > 3 ? out.size() - 3 : 0;
                for (size_t k = from; k < out.size(); ++k) add({{faint(), "    │ "}, {muted(), clipText(out[k], cols - 6)}});
            }
        } else if (type == "file") {
            add(fileLine(b, cols));
        } else if (type == "approval" || type == "question") {
            std::string state = b["state"].str();
            if (state == "pending") {
                unstableAt(r.lines.size());
                continue;
            }
            bool ok = state == "allow" || state == "allowSession";
            if (type == "approval") {
                std::string what = !b["command"].str().empty() ? util::splitLines(b["command"].str())[0] : b["path"].str();
                Line l = {{faint(), "  ⎿ "}, {Style(), withAgentName(b["title"].str("權限請求"), names)}};
                Line tail = {{fg(ok ? color::kOk : state == "cancelled" ? color::kMuted : color::kErr), "  → " + approvalState(state)}};
                int room = cols - width(l) - width(tail) - 2;
                if (!what.empty() && room > 4) l.push_back({fg(color::kCode), "  " + clipText(what, room)});
                add(concat(l, tail));
                if (!b["feedback"].str().empty()) addAll(wrap({{muted(), b["feedback"].str()}}, cols, line("    "), line("    ")));
            } else {
                std::string ans = answersText(b["answers"]);
                Line l = {{faint(), "  ⎿ "}, {Style(), state == "cancelled" ? "問題已取消" : "回答了問題"}};
                if (!ans.empty()) l.push_back({muted(), "：" + ans});
                addAll(wrap(l, cols, {}, line("    ")));
            }
        } else if (type == "todo") {
            todo = &b;  // drawn live under the status line while running, and once at the end
        } else if (type == "notice") {
            bool warn = b["level"].str() == "warn";
            addAll(wrap({{warn ? fg(color::kWarn) : muted(), b["text"].str()}}, cols,
                        {{warn ? fg(color::kWarn) : muted(), warn ? "  ! " : "  · "}}, line("    ")));
        } else if (type == "image") {
            addAll(wrap({{muted(), "測試圖片：" + b["artifact"]["file"].str() + " · " + b["artifact"]["result"].str() +
                                       "（桌面版可預覽）"}},
                        cols, line("  "), line("    ")));
        } else if (type == "error") {
            addAll(wrap({{fg(color::kErr), b["text"].str()}}, cols, {{fg(color::kErr), "  ✗ "}}, line("    ")));
        }
    }

    if (streaming) {
        r.stable = stable == std::string::npos ? r.lines.size() : stable;
        return r;
    }

    if (todo) addAll(todoLines(*todo, cols));
    std::string v = m["verdict"].str();
    if (v == "approve") add({{fg(color::kOk), "  ✓ 結論：同意，沒有修改"}});
    else if (v == "changed") add({{fg(color::kBrand), "  ✎ 結論：修改了檔案"}});
    else if (v == "issues") add({{fg(color::kWarn), "  ! 結論：有疑慮"}});

    std::vector<std::string> foot;
    const auto& route = m["routing"];
    if (route["autoModel"].boolean() || route["autoEffort"].boolean()) {
        addAll(wrap({{muted(), "依需求選擇：" + route["reason"].str()}}, cols, line("  "), line("    ")));
    }
    std::string model = modelName(m);
    if (!model.empty()) foot.push_back(model + (m["effort"].str().empty() ? "" : " " + m["effort"].str()));
    const Json& u = m["usage"];
    if (u.isObject() && (u["inputTokens"].i64() || u["outputTokens"].i64())) {
        foot.push_back("輸入 " + tokens(u["inputTokens"].i64()) + " · 輸出 " + tokens(u["outputTokens"].i64()));
    }
    int64_t ms = u["durationMs"].i64();
    if (ms <= 0 && m["endedAt"].i64() > m["ts"].i64() && m["ts"].i64() > 0) ms = m["endedAt"].i64() - m["ts"].i64();
    if (ms > 0) foot.push_back(duration(ms));
    Line fl = {{faint(), "  "}};
    for (size_t k = 0; k < foot.size(); ++k) fl.push_back({faint(), (k ? " · " : "") + foot[k]});
    std::string status = m["status"].str();
    if (status == "interrupted") fl.push_back({fg(color::kWarn), foot.empty() ? "已中斷" : " · 已中斷"});
    if (status == "failed") fl.push_back({fg(color::kErr), foot.empty() ? "沒有完成" : " · 沒有完成"});
    if (fl.size() > 1) add(clip(fl, cols));
    r.stable = r.lines.size();
    r.complete = true;
    return r;
}

std::vector<Line> renderDetails(const Json& m, const Names& names, int cols) {
    std::vector<Line> out;
    std::string author = m["author"].str();
    uint32_t c = agentColor(author);
    out.push_back({});
    out.push_back({{fg(c), "● "}, {fg(c, true), names.of(author)}, {muted(), "  詳細內容"}});
    auto addWrapped = [&](const std::string& text, Style st, const Line& prefix) {
        for (auto& l : wrap({{st, text}}, cols, prefix, prefix)) out.push_back(std::move(l));
    };
    for (const auto& b : m["blocks"].items()) {
        std::string type = b["type"].str();
        if (type == "text") {
            for (auto& l : markdown(b["text"].str(), cols, line("  "))) out.push_back(std::move(l.line));
        } else if (type == "tool") {
            out.push_back(toolLine(b, cols));
            if (!b["command"].str().empty()) addWrapped(b["command"].str(), fg(color::kCode), {{faint(), "    $ "}});
            else if (!b["input"].str().empty()) addWrapped(b["input"].str(), fg(color::kCode), {{faint(), "    "}});
            for (const auto& l : util::splitLines(b["output"].str())) addWrapped(l, muted(), {{faint(), "    │ "}});
        } else if (type == "file" || (type == "approval" && !b["diff"].str().empty())) {
            if (type == "file") out.push_back(fileLine(b, cols));
            else out.push_back({{faint(), "  ⎿ "}, {Style(), withAgentName(b["title"].str(), names)}});
            int shown = 0;
            for (const auto& l : util::splitLines(b["diff"].str())) {
                if (++shown > 400) {
                    out.push_back({{muted(), "    …（diff 太長，其餘請直接開檔案）"}});
                    break;
                }
                Style st = util::startsWith(l, "@@") ? faint() : util::startsWith(l, "+") ? fg(color::kOk) : util::startsWith(l, "-") ? fg(color::kErr) : muted();
                out.push_back({{Style(), "    "}, {st, clipText(l, cols - 4)}});
            }
        } else if (type == "todo") {
            for (auto& l : todoLines(b, cols)) out.push_back(std::move(l));
        } else if (type == "notice" || type == "error") {
            addWrapped(b["text"].str(), type == "error" ? fg(color::kErr) : muted(), line("  · "));
        }
    }
    return out;
}

std::vector<Line> logo() {
    std::vector<Line> out;
    auto rgbOf = [](char key, uint32_t* rgb) {
        for (const auto& c : logo_art::kColors) {
            if (c.key == key) { *rgb = c.rgb; return true; }
        }
        return false;
    };
    for (int y = 0; y < logo_art::kRows; y += 2) {
        Line l;
        for (int x = 0; x < logo_art::kCols; ++x) {
            uint32_t top = 0, bottom = 0;
            bool t = rgbOf(logo_art::kPixels[y][x], &top);
            bool b = y + 1 < logo_art::kRows && rgbOf(logo_art::kPixels[y + 1][x], &bottom);
            Span sp;
            if (t && b && top == bottom) { sp.style = fg(top); sp.text = "█"; }
            else if (t && b) { sp.style = fg(top); sp.style.bg = bottom; sp.style.hasBg = true; sp.text = "▀"; }
            else if (t) { sp.style = fg(top); sp.text = "▀"; }
            else if (b) { sp.style = fg(bottom); sp.text = "▄"; }
            else sp.text = " ";
            if (!l.empty() && l.back().style == sp.style) l.back().text += sp.text;
            else l.push_back(sp);
        }
        out.push_back(l);
    }
    return out;
}

}  // namespace tui
