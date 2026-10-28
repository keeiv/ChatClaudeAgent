// Deterministic, inspectable task routing constrained to backend-reported capabilities.
#pragma once

#include <string>

#include "json.hpp"

Json analyzeTask(const std::string& request, const std::string& previousRequest = "");
// `deepest` treats every request as the hardest kind: whatever is left automatic aims at the
// highest model tier and reasoning level the catalogue reports. Manual choices stay as they are.
Json chooseRoute(const std::string& request, const Json& catalogue, const std::string& pinnedModel,
                 const std::string& pinnedEffort, bool autoModel, bool autoEffort,
                 const std::string& previousRequest = "", bool deepest = false);
