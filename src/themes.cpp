// Theme changes are acknowledged only after settings have been saved atomically.
#include "themes.hpp"

#include <charconv>

#include "app.hpp"
#include "editions.hpp"
#include "theme_catalog.hpp"
#include "util.hpp"

const Json& themeCatalogue() {
    static const Json themes = Json::parse(kThemeCatalog);
    return themes;
}
namespace {
Json& pluginRegistry() {
    static Json themes = Json::array();
    return themes;
}
}  // namespace
void setPluginThemes(const Json& themes) {
    pluginRegistry() = themes;
}
const Json& pluginThemes() {
    return pluginRegistry();
}
Json themeById(const std::string& id) {
    for (const auto& theme : themeCatalogue().items())
        if (theme["id"].str() == id) return theme;
    for (const auto& theme : pluginRegistry().items())
        if (theme["id"].str() == id) return theme;
    return Json();
}
bool themeAllowed(const Json& theme) {
    return !theme.isNull() && theme["edition"].str() == editionId();
}
uint32_t themeColor(const Json& theme, const std::string& token) {
    std::string color = theme["tokens"][token].str();
    uint32_t value = 0;
    if (color.size() == 7 && color.front() == '#')
        std::from_chars(color.data() + 1, color.data() + color.size(), value, 16);
    return value;
}
bool App::handleTheme(const Json& message) {
    if (message["type"].str() != "theme.set") return false;
    const auto id = message["theme"].str();
    Json previous = settings_;
    bool ok = themeAllowed(themeById(id));
    if (ok) settings_["theme"] = id;
    ok = ok && util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump());
    if (!ok) settings_ = previous;
    else sendSettings();
    send(J({{"type", "theme.result"},
            {"ok", ok},
            {"theme", settings_["theme"]},
            {"requestToken", message["requestToken"]},
            {"error", ok ? "" : "主題未能保存，原設定已保留"}}));
    return true;
}
