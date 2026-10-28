// Calendar-day token activity shared by the desktop and terminal views.
#pragma once

#include <string>

#include "json.hpp"

std::string activityToday();
Json tokenActivity(const Json& history, const std::string& today, int days = 365);
