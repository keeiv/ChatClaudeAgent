#pragma once
#include <string>
#include "json.hpp"

// Only provider-level quota errors belong here, never arbitrary tool output.
bool isClaudeQuotaError(const std::string& code, const std::string& text);
bool isClaudeQuotaRejected(const Json& info);
// Any provider's quota or rate-limit refusal, from an error code or the error text shown to the user.
bool isQuotaText(const std::string& code, const std::string& text);
// Local wall-clock time "M/D HH:MM" for a Unix time in milliseconds, for schedule messages.
std::string localClock(long long ms);
