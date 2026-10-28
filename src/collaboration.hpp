// Persistent commander decisions and review focus, independent of model transport.
#pragma once

#include "json.hpp"

Json defaultReviewProfiles();
bool validateReviewProfiles(const Json& profiles, std::string* error);
Json makePlan(const Json& previous, const Json& input, long long now, std::string* error);
std::string planRequest(const Json& plan);
Json loadCollaboration(const std::string& convId);
bool saveCollaboration(const std::string& convId, const Json& data);
