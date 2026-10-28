// Aggregate civil dates without DST arithmetic; cached counters are not counted twice.
#include "activity.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

namespace {
using Day = std::chrono::sys_days;
constexpr long long kMaxTokens = 9007199254740991LL;

long long add(long long a, long long b) {
    b = std::max(0LL, std::min(kMaxTokens, b));
    return a > kMaxTokens - b ? kMaxTokens : a + b;
}

bool parseDay(const std::string& key, Day& result) {
    if (key.size() != 10 || key[4] != '-' || key[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (key[i] < '0' || key[i] > '9') return false;
    int year = (key[0] - '0') * 1000 + (key[1] - '0') * 100 + (key[2] - '0') * 10 + key[3] - '0';
    std::chrono::year_month_day date{std::chrono::year{year},
                                     std::chrono::month{unsigned((key[5] - '0') * 10 + key[6] - '0')},
                                     std::chrono::day{unsigned((key[8] - '0') * 10 + key[9] - '0')}};
    if (year < 1 || !date.ok()) return false;
    result = Day{date};
    return true;
}

std::string keyOf(Day date) {
    std::chrono::year_month_day ymd{date};
    char key[16];
    std::snprintf(key, sizeof key, "%04d-%02u-%02u", int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()));
    return key;
}
}  // namespace

std::string activityToday() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char key[16];
    std::snprintf(key, sizeof key, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return key;
}

Json tokenActivity(const Json& history, const std::string& today, int count) {
    Day end;
    if (!parseDay(today, end)) return Json::object();
    count = std::max(1, std::min(366, count));
    Day start = end - std::chrono::days(count - 1);
    std::map<Day, long long> totals;
    for (const auto& entry : history.fields()) {
        Day date;
        if (!parseDay(entry.first, date) || date > end || !entry.second.isObject()) continue;
        long long total = 0;
        for (const auto& agent : entry.second.fields()) {
            total = add(total, agent.second["input"].i64());
            total = add(total, agent.second["output"].i64());
        }
        totals[date] = total;
    }
    long long cumulative = 0, visible = 0;
    int active = 0;
    for (const auto& entry : totals)
        if (entry.first < start) cumulative = add(cumulative, entry.second);
    Json days = Json::array();
    for (Day date = start; date <= end; date += std::chrono::days(1)) {
        auto found = totals.find(date);
        long long value = found == totals.end() ? 0 : found->second, week = 0;
        for (int i = 0; i < 7; ++i) {
            auto prior = totals.find(date - std::chrono::days(i));
            if (prior != totals.end()) week = add(week, prior->second);
        }
        cumulative = add(cumulative, value);
        visible = add(visible, value);
        if (value > 0) ++active;
        days.push(J({{"date", keyOf(date)},
                     {"daily", value},
                     {"weekly", week},
                     {"cumulative", cumulative},
                     {"recorded", found != totals.end()}}));
    }
    return J({{"today", today},
              {"start", keyOf(start)},
              {"first", totals.empty() ? "" : keyOf(totals.begin()->first)},
              {"days", days},
              {"total", cumulative},
              {"windowTotal", visible},
              {"activeDays", active}});
}
