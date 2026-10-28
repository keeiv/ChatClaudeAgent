// Compliance checks CCA runs on every AI reply: Traditional Chinese, the persona's
// address and voice, the group VERDICT protocol, and evidence behind approvals and
// test claims. Prompts alone drift; a reply that fails these checks is sent back.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

struct GuardRules {
    std::string speaker;                  // the AI's display name, used in messages
    std::string personaName;              // empty: no persona assigned
    std::string address;                  // how the persona must address the user; empty: not required
    bool requireVerdict = false;          // group review, recheck and self-check turns
    bool requireEvidence = false;         // an APPROVE must rest on files read, searches or commands
    bool checkTestClaims = true;          // "tests passed" needs a command run in the same turn
    long long changedFiles = 0;           // files this turn changed (an APPROVE must not change any)
    std::vector<std::string> localNames;  // local reviewers whose notes came with the prompt
};

struct GuardIssue {
    std::string code;  // language | simplified | address | persona | verdict | approve-changed |
                       // no-evidence | test-claim | local-notes | local-unread | local-cite | local-format
    std::string text;  // Traditional Chinese; shown to the user and sent back to the agent
};

// Checks one reply. `blocks` are the message's blocks (tool blocks are the evidence).
std::vector<GuardIssue> guardCheck(const GuardRules& rules, const std::string& reply, const Json& blocks);

// The short rule card prepended to every prompt CCA sends, and the matching system-prompt section.
std::string guardRuleCard(const GuardRules& rules);
std::string guardInstructions();

// The message that sends a failed reply back. attempt is 1-based.
std::string guardCorrection(const GuardRules& rules, const std::vector<GuardIssue>& issues, int attempt,
                            int maxAttempts);
Json guardIssuesJson(const std::vector<GuardIssue>& issues);
std::string guardSummary(const std::vector<GuardIssue>& issues);  // one line for system messages

// Exposed for tests.
std::string guardProse(const std::string& reply);                    // reply without code, paths and URLs
std::vector<std::string> guardSimplified(const std::string& prose);  // distinct Simplified-only characters
bool guardSaysTestsPassed(const std::string& prose);
