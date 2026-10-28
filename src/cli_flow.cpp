#include "cli_render.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace tui {
namespace {
bool animated(const std::string& status) {
    return status == "thinking" || status == "writing" || status == "reviewing" || status == "tool" ||
           status == "preparing" || status == "loading" || status == "retry";
}
uint32_t dim(uint32_t rgb, double amount) {
    unsigned r = (unsigned)(((rgb >> 16) & 255) * amount);
    unsigned g = (unsigned)(((rgb >> 8) & 255) * amount);
    unsigned b = (unsigned)((rgb & 255) * amount);
    return (r << 16) | (g << 8) | b;
}
uint32_t nodeColor(const Json& node, long long now, bool motion) {
    std::string status = node["status"].str();
    if (status == "waitingUser" || status == "paused" || status == "retry") return color::kWarn;
    if (status == "failed") return color::kErr;
    if (status == "waiting" || status == "unavailable") return color::kFaint;
    double amount = motion && animated(status) ? .72 + .28 * (1 + std::sin(now / 350.0)) / 2 : 1;
    return dim(agentColor(node["agent"].str(node["id"].str())), amount);
}
std::string glyph(const Json& node, long long now, bool motion) {
    std::string status = node["status"].str();
    if (status == "done") return "✓";
    if (status == "failed") return "×";
    if (status == "waitingUser") return "?";
    if (status == "paused" || status == "stopped" || status == "stopping") return "‖";
    if (animated(status)) {
        static const char* frames[] = {"◜", "◝", "◞", "◟"};
        return motion && (status == "thinking" || status == "loading" || status == "preparing") ? frames[(now / 120) % 4] : "●";
    }
    return "○";
}
struct Cell { std::string text = " "; Style style; bool tail = false; };
class Canvas {
public:
    int cols;
    std::vector<std::vector<Cell>> cells;
    Canvas(int cols, int rows) : cols(cols), cells(rows, std::vector<Cell>(cols)) {}
    void put(int x, int y, const std::string& text, Style style) {
        if (y < 0 || y >= (int)cells.size()) return;
        for (char32_t c : decode(text)) {
            int w = charWidth(c);
            if (w <= 0) continue;
            if (x < 0 || x + w > cols) break;
            cells[y][x] = {encode(std::u32string(1, c)), style, false};
            for (int k = 1; k < w; ++k) cells[y][x + k] = {"", style, true};
            x += w;
        }
    }
    std::vector<Line> lines() const {
        std::vector<Line> result;
        for (const auto& row : cells) {
            Line line;
            for (const auto& cell : row) {
                if (cell.tail) continue;
                if (!line.empty() && line.back().style == cell.style) line.back().text += cell.text;
                else line.push_back({cell.style, cell.text});
            }
            result.push_back(std::move(line));
        }
        return result;
    }
};
std::string rule(int count) { std::string out; for (int i = 0; i < count; ++i) out += "─"; return out; }
}

bool workflowAnimating(const Json& flow, long long now) {
    if (flow["status"].str() != "running") return false;
    for (const auto& node : flow["nodes"].items()) if (animated(node["status"].str())) return true;
    for (const auto& event : flow["transfers"].items()) if (now >= event["ts"].i64() && now - event["ts"].i64() < 1300) return true;
    return false;
}

std::vector<Line> renderWorkflow(const Json& flow, const Names& names, int cols, long long now, bool compact, bool motion) {
    if (cols < 20 || flow["nodes"].size() == 0) return {};
    std::map<std::string, Json> nodes;
    for (const auto& node : flow["nodes"].items()) nodes[node["id"].str()] = node;
    bool locals = nodes.count("local1") || nodes.count("local2");
    Line head = {{fg(color::kBrand, true), "  協作流程"}, {muted(), " · " + flow["label"].str("等待工作")}};
    if (flow["round"].i64() > 0) head.push_back({faint(), " · 第 " + std::to_string(flow["round"].i64()) + " 輪"});
    if (flow["files"].i64() > 0) head.push_back({muted(), " · " + std::to_string(flow["files"].i64()) + " 檔案 +" + std::to_string(flow["added"].i64()) + " −" + std::to_string(flow["removed"].i64())});
    if (flow["layout"].str() == "pipeline") {
        std::vector<Line> out = {clip(head, cols)};
        int index = 0;
        for (const auto& node : flow["nodes"].items()) {
            std::string agent = node["agent"].str(), who = names.of(agent);
            if (who.empty() || who == agent) who = node["profile"].str(who);
            std::string alias = node["name"].str();
            if (!alias.empty() && alias != who) who = alias + " · " + who;
            Line row = {{faint(), "  " + std::to_string(++index) + ". "},
                        {fg(nodeColor(node, now, motion), true), glyph(node, now, motion) + " " + who},
                        {muted(), " · " + node["role"].str() + " · " + node["label"].str()}};
            out.push_back(clip(row, cols));
        }
        return out;
    }
    if (compact || cols < 68 || nodes.count("gemini")) {
        std::vector<Line> out = {clip(head, cols)};
        auto members = [&](std::initializer_list<const char*> ids, bool readOnly) {
            const std::string prefix = readOnly && cols >= 48 ? "  唯讀 " : "  ";
            const std::string gap = readOnly ? "   " : " ── ";
            const int room = cols - width(prefix);
            Line row = line(prefix, faint());
            for (const char* id : ids) if (nodes.count(id)) {
                const Json& node = nodes[id];
                std::string label = clipText(node["label"].str(), room - 4);
                std::string name = clipText(names.of(id), std::max(1, room - width(label) - 3));
                Line item = {{fg(nodeColor(node, now, motion), !readOnly), glyph(node, now, motion) + " " + name},
                             {muted(), " " + label}};
                if (row.size() > 1 && width(row) + width(gap) + width(item) > cols) {
                    out.push_back(row);
                    row = line(prefix, faint());
                }
                if (row.size() > 1) row.push_back({faint(), gap});
                row.insert(row.end(), item.begin(), item.end());
            }
            if (row.size() > 1) out.push_back(row);
        };
        members({"claude", "codex", "gemini"}, false);
        if (locals) members({"local1", "local2"}, true);
        return out;
    }
    int nodeWidth = std::min(28, (cols - 14) / 2);
    int left = 2, right = cols - nodeWidth - 2;
    Canvas canvas(cols, locals ? 7 : 5);
    std::vector<Line> result = {clip(head, cols)};
    std::map<std::string, int> center = {{"claude", left + nodeWidth / 2}, {"codex", right + nodeWidth / 2}};
    for (const char* id : {"claude", "codex"}) {
        const Json& node = nodes[id];
        int x = std::string(id) == "claude" ? left : right;
        Style color = fg(nodeColor(node, now, motion));
        std::string vendor = std::string(id) == "claude" ? "Claude" : "GPT";
        std::string title = vendor + (names.of(id) == vendor ? "" : " · " + names.of(id));
        title = clipText(title, nodeWidth - 6);
        canvas.put(x, 0, "╭─ " + title + " " + rule(nodeWidth - width(title) - 5) + "╮", color);
        canvas.put(x, 1, "│" + std::string(nodeWidth - 2, ' ') + "│", color);
        canvas.put(x + 2, 1, clipText(glyph(node, now, motion) + " " + node["label"].str(), nodeWidth - 4), color);
        canvas.put(x, 2, "╰" + rule(nodeWidth - 2) + "╯", color);
    }
    canvas.put(left + nodeWidth, 1, rule(right - left - nodeWidth), faint());
    if (locals) {
        canvas.put(center["claude"], 2, "┬", faint());
        canvas.put(center["codex"], 2, "┬", faint());
        canvas.put(center["claude"], 3, "└" + rule(center["codex"] - center["claude"] - 1) + "┘", faint());
        for (const char* id : {"local1", "local2"}) if (nodes.count(id)) {
            bool both = nodes.count("local1") && nodes.count("local2");
            int middle = cols / 2 + (both ? (std::string(id) == "local1" ? -1 : 1) * (nodeWidth / 2 + 2) : 0);
            int x = middle - nodeWidth / 2;
            const Json& node = nodes[id];
            center[id] = x + nodeWidth / 2;
            canvas.put(center[id], 3, "┬", faint());
            std::string label = glyph(node, now, motion) + " " + names.of(id) + " · " + node["label"].str();
            label = clipText(label, nodeWidth - 4);
            canvas.put(x, 4, "[ " + label + std::string(std::max(0, nodeWidth - width(label) - 4), ' ') + " ]", fg(nodeColor(node, now, motion)));
        }
    }
    // Packet positions are derived exclusively from dispatch timestamps. No packet
    // travels while an agent is merely thinking or waiting for permission.
    if (motion && flow["status"].str() == "running") for (const auto& event : flow["transfers"].items()) {
        long long age = now - event["ts"].i64();
        std::string from = event["from"].str(), to = event["to"].str();
        if (age < 0 || age >= 1300 || !center.count(from) || !center.count(to)) continue;
        bool fromLocal = from == "local1" || from == "local2", toLocal = to == "local1" || to == "local2";
        std::vector<std::pair<int, int>> path;
        int start = fromLocal ? center[from] : (from == "claude" ? left + nodeWidth : right - 1);
        int end = toLocal ? center[to] : (to == "claude" ? left + nodeWidth : right - 1);
        int row = fromLocal || toLocal ? 3 : 1;
        if (row == 3) { start = center[from]; end = center[to]; }
        int step = start <= end ? 1 : -1;
        for (int x = start; x != end + step; x += step) path.push_back({x, row});
        int at = std::min((int)path.size() - 1, (int)(age * path.size() / 1300));
        for (int tail = 4; tail >= 0; tail -= 2) if (at >= tail) {
            auto p = path[at - tail];
            canvas.put(p.first, p.second, tail ? "·" : "●", fg(dim(agentColor(to), tail ? .45 : 1)));
        }
    }
    const auto& events = flow["transfers"].items();
    if (!events.empty()) {
        const Json& event = events.back();
        canvas.put(2, locals ? 6 : 4, clipText(names.of(event["from"].str()) + " → " + names.of(event["to"].str()) + " · " + event["label"].str(), cols - 4), muted());
    } else {
        canvas.put(2, locals ? 6 : 4, motion ? "思考／撰寫：節點亮起　送審／建議：圓點沿線移動" : "動態效果關閉 · 保留狀態與交接方向", faint());
    }
    auto body = canvas.lines();
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
}  // namespace tui
