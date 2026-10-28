#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace util {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

int64_t nowMs();
std::string randomHex(size_t bytes);
std::string newId();  // short unique id for messages and conversations

std::wstring appDataDir();       // %APPDATA%\CCA, created on demand
// Held for as long as a ChatClaudeAgent (desktop or CLI) uses the data folder, so two
// never write the same conversations at once. Null when another one already holds it.
void* lockDataDir();
std::wstring localAppDataDir();  // %LOCALAPPDATA%\CCA, created on demand
std::wstring homeDir();
std::wstring exeDir();
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
bool ensureDir(const std::wstring& path);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);

bool readFile(const std::wstring& path, std::string& out);
// Writes to a temp file then renames, so a crash never leaves half a file.
bool writeFileAtomic(const std::wstring& path, const std::string& data);

std::wstring getEnv(const wchar_t* name);
std::string windowsUserName();
// Looks up an executable on PATH, trying PATHEXT extensions. Empty if not found.
std::wstring findOnPath(const std::wstring& name);

std::string base64UrlDecode(const std::string& in);
// Decodes the payload of a JWT without verifying it (only used to read local claims).
std::string jwtPayload(const std::string& token);

// Quotes one argument for CreateProcess using the CommandLineToArgvW rules.
std::wstring quoteArg(const std::wstring& arg);

std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
std::string toLower(std::string s);
std::vector<std::string> splitLines(const std::string& s);
// Cuts to at most maxBytes without splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string& s, size_t maxBytes);
// Keeps the last maxBytes, starting at a UTF-8 boundary.
std::string tailUtf8(const std::string& s, size_t maxBytes);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
// Shows a path inside root relative to it, with Windows separators.
std::string displayPath(const std::string& path, const std::wstring& root);

// Why a folder is too broad to hand to an AI as its project (empty when it is fine).
// Both agents get write access to the project folder, and Codex's Windows sandbox
// changes ACLs on it, so drive roots, the profile folder and system folders are refused.
std::string projectFolderProblem(const std::wstring& path);

}  // namespace util
