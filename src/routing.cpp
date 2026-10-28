// Routing never invents a model ID or reasoning level, and never changes provider ownership.
#include "routing.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <vector>

#include "util.hpp"

namespace {
bool has(const std::string& text, std::initializer_list<const char*> words) {
    for (const auto word : words)
        if (text.find(word) != std::string::npos) return true;
    return false;
}
int modelTier(const Json& model) {
    const auto id = util::toLower(model["value"].str());
    if (has(id, {"image", "audio", "whisper", "embedding", "realtime", "tts"})) return -2;
    if (has(id, {"haiku", "mini", "nano", "luna", "spark"})) return 0;
    if (has(id, {"opus", "fable", "astra", "-pro"})) return 2;
    if (has(id, {"sonnet", "sol", "terra", "gpt-4", "gpt-5", "gpt-6", "codex"})) return 1;
    return -1;
}
std::vector<int> generation(const std::string& id) {
    std::vector<int> result;
    int number = -1;
    for (unsigned char c : id) {
        if (c >= '0' && c <= '9') number = std::min(999999, std::max(0, number) * 10 + c - '0');
        else if (number >= 0) {
            result.push_back(number);
            number = -1;
        }
    }
    if (number >= 0) result.push_back(number);
    return result;
}
bool usable(const Json& item) {
    const auto id = item["value"].str();
    return !id.empty() && id != "default" && id.size() <= 128 && !item["hidden"].boolean() &&
           std::none_of(id.begin(), id.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
int effortRank(const std::string& value) {
    static const std::array<std::string, 9> levels{"none",  "minimal", "low",   "medium",   "high",
                                                   "xhigh", "max",     "ultra", "ultracode"};
    auto found = std::find(levels.begin(), levels.end(), value);
    return found == levels.end() ? -1 : (int)std::distance(levels.begin(), found);
}
}  // namespace

Json chooseRoute(const std::string& request, const Json& catalogue, const std::string& pinnedModel,
                 const std::string& pinnedEffort, bool autoModel, bool autoEffort, const std::string& previousRequest,
                 bool deepest) {
    Json result = analyzeTask(request, previousRequest);
    // Manual choices stay; only what is left automatic is aimed at the deepest model and effort.
    if (deepest && (autoModel || autoEffort)) {
        result["level"] = 2;
        result["targetEffort"] = "ultracode";
        result["category"] = "最高推理";
        result["reason"] = "每個需求都當成最艱難的任務：自動選擇時用最高層級的模型與目錄回報的最高思考程度";
    }
    result["autoModel"] = autoModel;
    result["autoEffort"] = autoEffort;
    result["model"] = pinnedModel;
    result["effort"] = pinnedEffort;
    result["capabilitySource"] = "backend-catalogue";
    Json selected;
    if (autoModel) {
        int score = INT_MAX;
        for (const auto& model : catalogue.items()) {
            if (!usable(model) || modelTier(model) < 0) continue;
            if (!autoEffort && !pinnedEffort.empty()) {
                bool supported = false;
                for (const auto& effort : model["supportedEffortLevels"].items())
                    supported |= effort.str() == pinnedEffort;
                if (!supported) continue;
            }
            const int tier = modelTier(model), wanted = (int)result["level"].i64();
            const int candidate = std::abs(tier - wanted) * 100 + (tier < wanted ? 10 : 0);
            if (candidate > score) continue;
            if (candidate == score && !selected.isNull() &&
                generation(model["value"].str()) <= generation(selected["value"].str()))
                continue;
            selected = model;
            score = candidate;
        }
        if (selected.isNull()) {
            // Unknown families are not ranked by an invented quality score.
            result["model"] = pinnedModel;
            result["reason"] = "目錄未回報可分級的模型，沿用既有設定或後端預設";
            result["capabilitySource"] = "unreported";
            if (!autoEffort && !pinnedEffort.empty() && !catalogue.empty()) {
                result["blocked"] = true;
                result["reason"] = "目錄未回報支援固定思考程度的候選模型；請調整固定程度或開啟自動思考";
            }
        } else {
            result["model"] = selected["value"];
            if (modelTier(selected) != result["level"].i64())
                result["reason"] = result["reason"].str() + "；目錄沒有同級模型，選用可用的最接近候選";
            if (!autoEffort && !pinnedEffort.empty())
                result["reason"] = result["reason"].str() + "；依固定思考程度篩選相容模型";
        }
    }
    if (selected.isNull()) {
        for (const auto& item : catalogue.items()) {
            if (!usable(item)) continue;
            if ((!result["model"].str().empty() && item["value"].str() == result["model"].str()) ||
                (result["model"].str().empty() && item["isDefault"].boolean())) {
                selected = item;
                break;
            }
        }
    }
    if (autoEffort) {
        result["effort"] = "";
        const int wanted = effortRank(result["targetEffort"].str());
        int best = INT_MAX;
        for (const auto& effort : selected["supportedEffortLevels"].items()) {
            const int rank = effortRank(effort.str());
            if (rank < 0) continue;
            const int score = std::abs(rank - wanted) * 10 + (rank < wanted ? 1 : 0);
            if (score < best) {
                best = score;
                result["effort"] = effort;
            }
        }
        if (result["effort"].str().empty()) result["effortNote"] = "未回報可調整程度，使用後端預設";
        else if (result["effort"].str() != result["targetEffort"].str())
            result["effortNote"] =
                "目標程度 " + result["targetEffort"].str() + "，使用目錄最接近的 " + result["effort"].str();
    }
    result["modelName"] = selected["displayName"].str(result["model"].str("後端預設"));
    if (result["modelName"].str().empty()) result["modelName"] = "後端預設";
    return result;
}
