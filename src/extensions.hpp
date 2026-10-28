// CCA plugins: data-only packages that add themes, conversation modes and prompt commands.
#pragma once

#include <string>

#include "json.hpp"

namespace extensions {
// %APPDATA%\CCA\plugins, created on first use.
std::wstring folder();
// Reads and validates every plugin folder. Each record keeps its errors; a plugin with errors cannot be enabled.
Json scan(const std::wstring& dir);
// The full theme record (tokens included) for a validated plugin theme, built from CCA's own base tokens.
Json themeFromPalette(const std::string& id, const Json& theme, const std::string& pluginName);
// WCAG contrast ratio of two #rrggbb colors; 0 for malformed input.
double contrast(const std::string& a, const std::string& b);
}  // namespace extensions
