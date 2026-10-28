// Stable group navigation order, shared by desktop and CLI without changing conversation storage.
#include "app.hpp"

#include <set>

#include "util.hpp"

Json App::groupOrder() const {
    std::set<std::string> remaining;
    for (const auto& c : convs_)
        if (c->kind == "group") remaining.insert(c->id);
    Json order = Json::array();
    for (const auto& id : settings_["groupOrder"].items())
        if (id.isString() && remaining.erase(id.str())) order.push(id);
    // Existing users keep creation/storage order; incoming activity must not move their icons.
    for (const auto& c : convs_)
        if (remaining.erase(c->id)) order.push(c->id);
    return order;
}

void App::handleGroupOrder(const Json& message) {
    const Json current = groupOrder();
    const Json& order = message["order"];
    std::set<std::string> expected;
    for (const auto& id : current.items()) expected.insert(id.str());
    bool valid = order.isArray() && order.size() == current.size();
    for (const auto& id : order.items()) valid &= id.isString() && expected.erase(id.str()) == 1;
    std::string error;
    if (!valid || !expected.empty()) {
        error = "群組清單已變更或排序無效，請依目前清單重新調整。";
    } else {
        const Json old = settings_;
        settings_["groupOrder"] = order;
        if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
            settings_ = old;
            error = "群組排序未能儲存，原順序已保留。";
        }
    }
    sendConvs();
    send(J({{"type", "groups.order.result"},
            {"requestToken", message["requestToken"].str()},
            {"ok", error.empty()},
            {"error", error}}));
}
