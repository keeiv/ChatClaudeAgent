// Resolve an optional local grant file and normalize the six visible profile slots.
#include "badges.hpp"

#include <windows.h>

#include <algorithm>
#include <set>

#include "util.hpp"

namespace {

bool known(const std::string& id) {
    const Json catalog = badgeCatalog();
    for (const auto& badge : catalog.items())
        if (badge["id"].str() == id) return true;
    return false;
}

std::vector<std::string> ownedIds(const Json& grants) {
    std::vector<std::string> ids;
    const Json catalog = badgeCatalog();
    for (const auto& badge : catalog.items()) {
        const std::string id = badge["id"].str();
        bool owned = id == "member";
        for (const auto& grant : grants["grants"].items()) owned |= grant.str() == id;
        if (owned) ids.push_back(id);
    }
    return ids;
}

bool contains(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

}  // namespace

Json badgeCatalog() {
    return Json::Array{
        J({{"id", "developer"},
           {"name", "CCA 開發者"},
           {"description", "參與 CCA 核心設計與開發。"},
           {"criteria", "由本機授予資料指定。"}}),
        J({{"id", "community-developer"},
           {"name", "社群開發者"},
           {"description", "為 CCA 提供程式、工具或整合貢獻。"},
           {"criteria", "由本機授予資料指定；目前沒有公開申請流程。"}}),
        J({{"id", "origin"},
           {"name", "CCA 原初"},
           {"description", "紀念最初的橘綠交環圖標。"},
           {"criteria", "版本紀念徽章，由本機授予資料指定。"}}),
        J({{"id", "familiar"},
           {"name", "CCA 熟識"},
           {"description", "紀念 1.0.0 以前的三段式 C 與圓點。"},
           {"criteria", "版本紀念徽章，由本機授予資料指定。"}}),
        J({{"id", "present"},
           {"name", "CCA 今時"},
           {"description", "紀念 1.0.0 的相向銜接圖標。"},
           {"criteria", "版本紀念徽章，由本機授予資料指定，不會依執行版本自動授予。"}}),
        J({{"id", "pioneer"},
           {"name", "先鋒支持者"},
           {"description", "紀念早期測試、回饋與支持。"},
           {"criteria", "由本機授予資料指定，沒有付費或訂閱條件。"}}),
        J({{"id", "member"},
           {"name", "成員"},
           {"description", "CCA 使用者的基本徽章。"},
           {"criteria", "所有安裝內建，無須登入或申請。"}}),
    };
}

Json loadBadgeGrants(const std::wstring& path, std::string& error) {
    error.clear();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            error = "無法讀取本機徽章授予檔；僅顯示內建成員徽章。";
        return Json::object();
    }
    std::string data;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    DWORD read = 0;
    bool readable =
        file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64 * 1024;
    if (readable) {
        data.resize((size_t)size.QuadPart);
        readable = ReadFile(file, data.data(), (DWORD)data.size(), &read, nullptr) && read == data.size();
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Json grants = Json::parse(data);
    bool valid = readable && grants.isObject() && grants["version"].num() == 1 && grants["profile"].isString() &&
                 grants["profile"].str().size() <= 128 && grants["grants"].isArray() && grants["grants"].size() <= 7;
    std::set<std::string> seen;
    for (const auto& id : grants["grants"].items())
        valid &= id.isString() && known(id.str()) && seen.insert(id.str()).second;
    if (!valid) {
        error = "本機徽章授予檔無法讀取；保留原檔，僅顯示內建成員徽章。";
        return Json::object();
    }
    return grants;
}

Json profileBadges(const Json& grants, const Json& preferences) {
    const auto owned = ownedIds(grants);
    std::vector<std::string> order;
    for (const auto& id : preferences["order"].items())
        if (contains(owned, id.str()) && !contains(order, id.str())) order.push_back(id.str());
    for (const auto& id : owned)
        if (!contains(order, id)) order.push_back(id);
    Json ordered = Json::array(), hidden = Json::array(), display = Json::array(), all = Json::array();
    for (const auto& id : owned) all.push(id);
    for (const auto& id : order) {
        ordered.push(id);
        bool hide = false;
        for (const auto& item : preferences["hidden"].items()) hide |= item.str() == id;
        if (hide) hidden.push(id);
        else if (display.size() < 6) display.push(id);
    }
    return J({{"catalog", badgeCatalog()},
              {"owned", all},
              {"order", ordered},
              {"hidden", hidden},
              {"display", display},
              {"scope", "local"},
              {"profile", grants["profile"].str()}});
}

bool validBadgeDisplay(const Json& request, const Json& grants) {
    const auto owned = ownedIds(grants);
    if (!request["order"].isArray() || request["order"].size() != owned.size() || !request["hidden"].isArray() ||
        request["hidden"].size() > owned.size())
        return false;
    for (const char* field : {"order", "hidden"}) {
        std::set<std::string> seen;
        for (const auto& id : request[field].items())
            if (!id.isString() || !contains(owned, id.str()) || !seen.insert(id.str()).second) return false;
    }
    return true;
}
