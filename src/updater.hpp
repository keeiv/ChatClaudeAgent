// Single-executable release inspection and an isolated, rollback-capable Windows update broker.
#pragma once

#include <string>

#include "json.hpp"

namespace updater {
bool version(const std::string& text, int parts[4]);
int compare(const std::string& left, const std::string& right);
std::string sha256(const std::string& data);
std::wstring executable();
bool validSource(const std::string& source);
Json inspect(const std::string& source, const std::string& current, const std::wstring& exe, const std::string& flavor);
// The automatically created local folder that receives release folders or single executables.
std::wstring inboxDir();
// A cheap fingerprint of the folder's entries; a changed value warrants a full inspection.
std::string inboxStamp(const std::wstring& dir);
// `edition` is empty for the ordinary edition. Newer builds of the running edition are the automatic update;
// the best build of every other edition comes back in "choices" for the user to pick.
Json inspectFolder(const std::wstring& dir, const std::string& current, const std::wstring& exe,
                   const std::string& flavor, const std::string& edition = "");
// Display name of an edition ID ("" is the ordinary edition); empty for an unknown ID.
std::string editionName(const std::string& edition);
// An Android package may only be opened (in the phone simulator) when its size and SHA-256 match the manifest.json
// beside it, which names the same file and the haiku edition.
bool verifyAndroidPackage(const std::wstring& apk, std::string& error);
Json prepare(const Json& inspected, const std::wstring& exe);
// Launches only a prepared copy of this executable. The broker acknowledges readiness before close.
bool launch(const Json& prepared, std::string& error);
// Called before acquiring the application's single-instance lock. -1 means ordinary startup.
int helperMain();
void confirmStartup();
}  // namespace updater
