// Character presets ("性格") that change how an agent talks, and the text that
// turns a persona into extra instructions for native agents.
#pragma once

#include <string>

#include "json.hpp"

// Optional user-owned JSON, validated as a whole. No characters ship in the binary.
bool parsePersonaPack(const Json& data, Json& result, std::string& error);
Json loadPersonaPresets(const std::wstring& path, std::string& error);

// Looks a persona up among the built-ins and the user's own (custom) list.
Json findPersona(const Json& custom, const std::string& id, const Json& presets = Json::array());

// Full instruction text for a persona, including the rules that keep technical
// work and CCA's protocol above the role. Empty for a null persona.
std::string personaInstructions(const Json& persona);
