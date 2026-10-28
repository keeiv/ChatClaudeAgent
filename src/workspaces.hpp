// Git worktrees isolate tasks; merge previews bind both committed heads before applying a merge.
#pragma once

#include "json.hpp"

Json createTaskWorktree(const std::wstring& origin, const std::string& taskId);
Json checkTaskMerge(const Json& workspace, bool apply, const std::string& target, const std::string& source);
