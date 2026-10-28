// Small, runtime-only workflow state shared by the terminal and desktop views.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "json.hpp"

struct WorkflowRun {
    std::string id, owner, lastMain, phase, outcome;
    std::map<std::string, std::string> turns;
    std::vector<std::string> localReplies;
    std::vector<Json> transfers;  // at most eight real dispatches, never a timer log
    int round = 0, files = 0;
    long long added = 0, removed = 0;  // retained when GroupRun releases its live state
};

void workflowTurn(WorkflowRun& run, const std::string& agent, const std::string& turnId,
                  const std::string& phase, long long now);
void workflowLocalReply(WorkflowRun& run, const std::string& agent);
Json workflowView(const Json& context, const Json& state, const WorkflowRun& run, const std::vector<Json>& messages);
