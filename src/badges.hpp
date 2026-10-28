// Local profile decorations. Badge ownership never grants application permissions.
#pragma once

#include <string>

#include "json.hpp"

Json badgeCatalog();
Json loadBadgeGrants(const std::wstring& path, std::string& error);
Json profileBadges(const Json& grants, const Json& preferences);
bool validBadgeDisplay(const Json& request, const Json& grants);
