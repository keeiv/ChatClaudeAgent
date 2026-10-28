// Shared task memory for one group task: the request, every agent's structured handoff
// (revisions, changed files, tests, issues, next step) and the outcome. It is saved after
// every change under %APPDATA%\CCA\tasks so a restart can pick the task up again, and it is
// summarized into a short brief for prompts instead of resending whole conversations.
// The project folder and its git state stay the source of truth for the code itself.
#pragma once

#include <string>

#include "json.hpp"

// {task_id, conv_id, request, status: running|paused|interrupted|done|stopped|failed,
//  created, updated, handoffs: [...], earlier: count of handoffs folded away}
Json newTask(const std::string& convId, const std::string& request, long long now);

// One agent turn as a handoff record:
// {seq, task_id, agent_id, role, base_revision, head_revision, changed_files, diff_reference,
//  summary, issues, test_results, verdict, next_action, ts}
Json handoffFromTurn(const Json& task, const Json& msg, const std::string& agent, const std::string& role,
                     const Json& files, const std::string& verdict, long long now);

// Appends a handoff and keeps the newest 40 (older ones are counted in `earlier`).
void addHandoff(Json& task, Json handoff);
// Fills in the newest handoff's next_action / head_revision when they are still empty.
void setNextAction(Json& task, const std::string& action);
void setHeadRevision(Json& task, const std::string& revision);

// What the UI shows: status and the newest 10 handoffs without their long text.
Json taskView(const Json& task);

// The compact brief agents get: request, status and the latest handoffs, within maxBytes.
std::string taskBrief(const Json& task, size_t maxBytes);

// tasks/<convId>.json in the data folder. load returns Null when there is none.
bool saveTask(const Json& task);
Json loadTask(const std::string& convId);
void deleteTask(const std::string& convId);

// Indexed immutable handoffs, including those compacted out of the current brief.
Json taskHistory(const std::string& taskId, long long after, size_t limit = 20);
