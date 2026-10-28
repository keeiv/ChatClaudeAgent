// Trusted built-in theme metadata, shared with the generated desktop catalogue.
#pragma once

#include <cstdint>
#include <string>

#include "json.hpp"

const Json& themeCatalogue();
Json themeById(const std::string& id);
uint32_t themeColor(const Json& theme, const std::string& token);
// Themes from enabled CCA plugins; themeById finds them too. Set on the UI thread when plugins load.
void setPluginThemes(const Json& themes);
const Json& pluginThemes();
// Whether this edition may show a theme: built-in themes may be reserved for one edition (Fable's own look).
bool themeAllowed(const Json& theme);
