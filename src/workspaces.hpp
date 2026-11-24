// Git worktrees isolate tasks; merge previews bind both committed heads before applying a merge.
#pragma once

#include "json.hpp"

Json createTaskWorktree(const std::wstring& origin, const std::string& taskId);
Json checkTaskMerge(const Json& workspace, bool apply, const std::string& target, const std::string& source);
// Merges the target branch into the task branch inside the worktree without committing: {ok, conflicts:[path]}.
// With no conflict the merge is aborted again and the branch is left as it was.
Json startWorktreeResolve(const Json& workspace);
