// User-owned persona presets stay outside the binary and source checkout.
#include "personas.hpp"

#include <windows.h>

#include <set>

#include "util.hpp"

bool parsePersonaPack(const Json& data, Json& result, std::string& error) {
    result = Json::array();
    error.clear();
    if (!data.isArray() || data.size() > 128) {
        error = "性格預設檔必須是最多 128 項的 JSON 陣列";
        return false;
    }
    std::set<std::string> ids;
    for (const auto& item : data.items()) {
        std::string id = item["id"].str(), agent = item["agent"].str("any");
        bool validId = !id.empty() && id.size() <= 80 && id != "none";
        for (unsigned char c : id)
            validId &=
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!item.isObject() || !validId || !ids.insert(id).second ||
            (item.has("agent") && !item["agent"].isString()) ||
            (agent != "any" && agent != "claude" && agent != "codex" && agent != "gemini" && agent != "local") ||
            (item.has("assignOnFirstUse") &&
             (!item["assignOnFirstUse"].isBool() || (agent == "any" && item["assignOnFirstUse"].boolean()))) ||
            !item["name"].isString() || util::trim(item["name"].str()).empty() || item["name"].str().size() > 96 ||
            !item["prompt"].isString() || util::trim(item["prompt"].str()).empty() ||
            item["prompt"].str().size() > 12000 ||
            (item.has("summary") && (!item["summary"].isString() || item["summary"].str().size() > 400)) ||
            (item.has("address") && (!item["address"].isString() || item["address"].str().size() > 60))) {
            error = "性格預設有無效或重複的 ID、適用對象或文字欄位；原設定未變更";
            result = Json::array();
            return false;
        }
        result.push(J({{"id", id},
                       {"name", util::trim(item["name"].str())},
                       {"agent", agent},
                       {"summary", item["summary"].str()},
                       {"address", item["address"].str()},
                       {"prompt", item["prompt"].str()},
                       {"builtin", true},
                       {"assignOnFirstUse", item["assignOnFirstUse"].boolean()}}));
    }
    return true;
}

Json loadPersonaPresets(const std::wstring& path, std::string& error) {
    error.clear();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            error = "無法讀取本機性格預設檔；原設定未變更";
        return Json::array();
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh || info.nFileSizeLow > 1024 * 1024) {
        error = "本機性格預設檔必須小於 1 MB";
        return Json::array();
    }
    std::string text;
    Json result;
    if (!util::readFile(path, text) || text.size() > 1024 * 1024) {
        error = "無法讀取本機性格預設檔；原設定未變更";
        return Json::array();
    }
    if (!parsePersonaPack(Json::parse(text), result, error)) return Json::array();
    return result;
}

Json findPersona(const Json& custom, const std::string& id, const Json& presets) {
    if (id.empty() || id == "none") return Json();
    // A user's saved copy has priority over an optional preset with the same ID.
    for (const auto& list : {&custom, &presets})
        for (const auto& p : list->items())
            if (p["id"].str() == id) return p;
    return Json();
}

std::string personaInstructions(const Json& persona) {
    if (!persona.isObject()) return {};
    std::string prompt = util::trim(persona["prompt"].str());
    if (prompt.empty()) return {};
    std::string out = "# 角色設定：" + persona["name"].str("自訂角色") + "\n\n" + prompt + "\n";
    std::string address = util::trim(persona["address"].str());
    if (!address.empty()) out += "\n稱呼使用者為「" + address + "」。\n";
    out +=
        "\n扮演規則：\n"
        "- 角色只影響語氣、用詞和態度。程式碼、技術判斷和事實必須正確完整，不能因為角色而降低品質。\n"
        "- 系統、權限、工具使用的規定，以及 CCA 協作流程的要求（例如回覆最後一行的 VERDICT "
        "格式），一律優先於角色設定。\n"
        "- 不要捏造背景或設定；不確定的就不要提。\n"
        "- 不要用角色當理由拒絕或敷衍工作。\n";
    return out;
}
