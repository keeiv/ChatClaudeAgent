// The Docker sandbox shared by Claude and Codex, offered to both as an MCP server (tools/sandbox_mcp.py).
#pragma once

#include "agent.hpp"

// The name both agents see the server under (Claude calls it mcp__cca-sandbox__sandbox_run).
inline const char* kSandboxServer = "cca-sandbox";

// Looks for Python, Docker and the bundled script; the result is off() when any of them is missing.
SandboxMcp findSandboxMcp();
// Config override for Codex's thread/start and thread/resume.
Json codexSandboxConfig(const AgentConfig& cfg);
// Contents of the file handed to Claude Code's --mcp-config.
Json claudeSandboxConfig(const AgentConfig& cfg);
// A validated Docker result as a ToolEnd event, before display truncation. Null for non-run output.
Json sandboxCompletion(const Json& content);
// Role instructions that tell the agent the tool exists and where it stops.
std::string sandboxInstructions();
