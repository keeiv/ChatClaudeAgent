// When each model was released, for the "加入時間" on an AI's profile card. An AI joins
// CCA as the model it runs on, so by default it joined on that model's release day.
#pragma once

#include <string>

#include "json.hpp"

// [{id, name, date: "YYYY-MM-DD", source, url}] and {alias: id}, sent to the desktop UI.
Json modelDateTable();
Json modelDateAliases();

// Resolves known IDs, dated snapshots and documented aliases such as "opus" or "llama3.1:8b".
// Empty when the model is not in the table.
std::string modelReleaseDate(const std::string& model);
