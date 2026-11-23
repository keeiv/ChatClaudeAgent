// Text layout for the CLI: display widths (CJK takes two columns), styled lines,
// wrapping, a small Markdown renderer, and how messages look in the terminal.
// Pure functions, so the unit tests can check them without a console.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "json.hpp"

namespace tui {

namespace color {
inline uint32_t kBrand = 0x6FB1FF;  // CCA's azure signal, matching the desktop UI.
inline uint32_t kBrandDeep = 0x4F95E8;
inline uint32_t kClaude = 0xEF9B78;
inline uint32_t kGpt = 0x48CFA6;
inline uint32_t kLocal1 = 0xF08AC3;
inline uint32_t kLocal2 = 0xEDBA7A;
inline uint32_t kMuted = 0x8796A4;
inline uint32_t kFaint = 0x6C7B89;
inline uint32_t kCode = 0xC8D2DB;
inline uint32_t kOk = 0x5CCF8A;
inline uint32_t kErr = 0xFF6B6B;
inline uint32_t kWarn = 0xF0C050;
}  // namespace color

void setTheme(const std::string& id);

struct Style {
    uint32_t fg = 0, bg = 0;
    bool hasFg = false, hasBg = false;
    bool bold = false, italic = false, underline = false, strike = false;
    bool operator==(const Style& o) const;
};
Style fg(uint32_t rgb, bool bold = false);
Style muted();
Style faint();

struct Span {
    Style style;
    std::string text;
};
using Line = std::vector<Span>;

Line line(const std::string& text, Style st = Style());

// Display columns of one character / a plain UTF-8 string / a styled line.
int charWidth(char32_t c);
int width(const std::string& utf8);
int width(const Line& l);
std::u32string decode(const std::string& utf8);
std::string encode(const std::u32string& s);

// The line as text with SGR colour codes, reset at the end.
std::string ansi(const Line& l);
// Cuts to at most `cols` columns, ending in "…" when something was cut.
Line clip(const Line& l, int cols);
std::string clipText(const std::string& s, int cols);
// Pads with spaces to exactly `cols` columns (clipping first if needed).
Line padTo(const Line& l, int cols);

// Wraps at word boundaries (and between CJK characters) so no line is wider than
// `cols`. `first` starts the first line and `rest` every following one.
std::vector<Line> wrap(const Line& content, int cols, const Line& first = {}, const Line& rest = {});

// Markdown for agent replies. Each display line remembers the source line it came from,
// so a streaming reply can commit the lines that can no longer change.
struct MdLine {
    Line line;
    int src = 0;
};
std::vector<MdLine> markdown(const std::string& text, int cols, const Line& prefix);

// ---- messages

struct Names {
    std::function<std::string(const std::string& who)> of;  // user | claude | codex | gemini | local1 | local2
};
uint32_t agentColor(const std::string& who);
// "Claude 想要執行指令" with the agent's display name in front.
std::string withAgentName(const std::string& title, const Names& names);
std::string handoffText(const Json& m, const Names& names);
std::string tokens(int64_t n);
std::string duration(int64_t ms);

struct Rendered {
    std::vector<Line> lines;
    size_t stable = 0;      // the first `stable` lines will not change any more
    bool complete = false;  // the message is finished and every line is final
};
// `cols` is the usable width (one less than the terminal, so lines never wrap on their own).
Rendered renderMessage(const Json& msg, const Names& names, int cols);
// Everything a finished reply did, with command output and diffs (for /show).
std::vector<Line> renderDetails(const Json& msg, const Names& names, int cols);

// The mark as half-block pixel art (src/logo_art.hpp), one Line per terminal row.
std::vector<Line> logo();

// A bounded workflow diagram; no retained frame buffer or animation thread.
bool workflowAnimating(const Json& flow, long long now);
std::vector<Line> renderWorkflow(const Json& flow, const Names& names, int cols, long long now, bool compact, bool motion);

}  // namespace tui
