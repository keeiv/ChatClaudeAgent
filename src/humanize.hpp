// 去 AI 化: rules that keep what the agents make (pages, artwork, prose) from
// looking like default AI output, and the matching checklist for reviewers.
#pragma once

#include <string>

// Instructions for Claude and GPT. `extra` holds the user's own rules and is
// appended as-is (trimmed, size-capped); it may be empty.
std::string humanizeInstructions(const std::string& extra);

// A short paragraph added to review prompts so reviewers check for the same tells.
std::string humanizeReviewNote(const std::string& extra = "");
