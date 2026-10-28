// Verified public model dates; unknown IDs deliberately have no guessed date.
#include "model_dates.hpp"

#include <algorithm>
#include <cctype>

#include "util.hpp"

namespace {

bool originalLocalTag(const std::string& tag) {
    const size_t split = tag.find_first_of("-_");
    std::string suffix = split == std::string::npos ? "" : tag.substr(split);
    if (util::startsWith(suffix, "-instruct")) suffix.erase(0, 9);
    if (suffix.empty()) return true;
    suffix.erase(0, 1);
    // Quantization changes packaging; retrained variants need their own release record.
    for (const char* quant : {"q2_k", "q3_k_s", "q3_k_m", "q3_k_l", "q4_0", "q4_1", "q4_k", "q4_k_s", "q4_k_m", "q5_0",
                              "q5_1", "q5_k_s", "q5_k_m", "q6_k", "q8_0", "f16", "fp16", "bf16", "f32"}) {
        if (suffix == quant) return true;
    }
    return false;
}

struct Entry {
    const char* id;
    const char* name;
    const char* date;
    const char* source;
    const char* url;
};

// Checked against primary sources on 2026-10-24. Dolphin uses the author's repository
// creation date, explicitly labelled as an upload date rather than a release announcement.
const Entry kModels[] = {
    {"claude-fable-5-1", "Claude Fable 5.1", "2026-09-15", "Anthropic",
     "https://www.anthropic.com/claude-fable-and-mythos-5-1"},
    {"claude-opus-5-5", "Claude Opus 5.5", "2026-09-22", "Anthropic", "https://www.anthropic.com/claude-opus-5-5"},
    {"claude-sonnet-5-5", "Claude Sonnet 5.5", "2026-09-28", "Anthropic",
     "https://www.anthropic.com/claude-sonnet-5-5"},
    {"claude-haiku-5-5", "Claude Haiku 5.5", "2026-10-07", "Anthropic", "https://www.anthropic.com/news/models"},
    {"gpt-5.6-sol", "GPT-5.6 Sol", "2026-07-09", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-5.6-terra", "GPT-5.6 Terra", "2026-07-09", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-5.6-luna", "GPT-5.6 Luna", "2026-07-09", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-6-astra", "GPT-6 Astra", "2026-09-03", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-6-sol", "GPT-6 Sol", "2026-09-22", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-6-luna", "GPT-6 Luna", "2026-09-22", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"gpt-6.1-sol", "GPT-6.1 Sol", "2026-09-29", "OpenAI", "https://developers.openai.com/api/docs/changelog"},
    {"llama3.1", "Llama 3.1", "2024-07-23", "Meta", "https://ai.meta.com/blog/meta-llama-3-1/"},
    {"qwen2.5-coder:7b", "Qwen2.5-Coder 1.5B / 7B", "2024-09-19", "Qwen",
     "https://qwenlm.github.io/blog/qwen2.5-coder/"},
    {"qwen2.5-coder:14b", "Qwen2.5-Coder 0.5B / 3B / 14B / 32B", "2024-11-12", "Qwen",
     "https://qwenlm.github.io/blog/qwen2.5-coder-family/"},
    {"qwen3", "Qwen3", "2025-04-29", "Qwen", "https://qwenlm.github.io/blog/qwen3/"},
    {"dolphin3", "Dolphin 3.0 Llama 3.1 8B", "2024-12-29", "作者模型庫（上傳日）",
     "https://huggingface.co/api/models/dphn/Dolphin3.0-Llama3.1-8B"},
};

// Claude Code's short names mean the newest model of that family.
const char* kAliases[][2] = {
    {"opus", "claude-opus-5-5"},
    {"sonnet", "claude-sonnet-5-5"},
    {"haiku", "claude-haiku-5-5"},
    {"fable", "claude-fable-5-1"},
    {"qwen2.5-coder:1.5b", "qwen2.5-coder:7b"},
    {"qwen2.5-coder:0.5b", "qwen2.5-coder:14b"},
    {"qwen2.5-coder:3b", "qwen2.5-coder:14b"},
    {"qwen2.5-coder:32b", "qwen2.5-coder:14b"},
};

}  // namespace

Json modelDateTable() {
    Json out = Json::array();
    for (const auto& e : kModels) {
        out.push(J({{"id", e.id}, {"name", e.name}, {"date", e.date}, {"source", e.source}, {"url", e.url}}));
    }
    return out;
}

Json modelDateAliases() {
    Json out = Json::object();
    for (const auto& a : kAliases) out[a[0]] = a[1];
    return out;
}

std::string modelReleaseDate(const std::string& model) {
    std::string id = util::toLower(util::trim(model));
    size_t tag = id.find(':');
    if (tag != std::string::npos) {
        std::string base = id.substr(0, tag);
        std::string variant = id.substr(tag + 1);
        if (originalLocalTag(variant)) {
            std::string size = variant.substr(0, variant.find_first_of("-_"));
            if (base == "qwen2.5-coder") id = base + ":" + size;
            else if ((base == "llama3.1" && (size == "latest" || size == "8b" || size == "70b" || size == "405b")) ||
                     (base == "qwen3" &&
                      (size == "latest" || size == "0.6b" || size == "1.7b" || size == "4b" || size == "8b" ||
                       size == "14b" || size == "30b" || size == "32b" || size == "235b")) ||
                     (base == "dolphin3" && (size == "latest" || size == "8b")))
                id = base;
        }
    }
    for (const auto& a : kAliases) {
        if (id == a[0]) id = a[1];
    }
    // An exact id, or the id with a dated snapshot after it ("claude-opus-5-5-20260922").
    for (const auto& e : kModels) {
        std::string key = e.id;
        std::string snapshot = id.size() == key.size() + 9 ? id.substr(key.size() + 1) : "";
        bool dated = snapshot.size() == 8 && id[key.size()] == '-' &&
                     std::all_of(snapshot.begin(), snapshot.end(), [](unsigned char c) { return isdigit(c); });
        if (id == key || (dated && id.compare(0, key.size(), key) == 0)) { return e.date; }
    }
    return "";
}
