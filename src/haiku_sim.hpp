// The native side of the Haiku phone simulator: what the Android shell (MainActivity, Projects, ModelClient, KeyVault)
// does for the editor page, redone for Windows so the unmodified Haiku APK can run in a phone-sized window.
// Haiku itself is not changed for this; the simulator reads its web assets out of the APK and answers its bridge calls.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "json.hpp"

namespace haikusim {

// ---- ZIP (APK) reading and writing. Only what the simulator needs: stored and deflated entries, no ZIP64.
// Reads the entries whose names `prefix` starts, with every CRC checked. Names must stay inside [A-Za-z0-9_./-].
bool readZipFolder(const std::string& zip, const std::string& prefix, std::map<std::string, std::string>& out,
                   std::string& error);
// RFC 1951 inflate into `out`, which must come out exactly `expected` bytes.
bool inflateRaw(const std::string& in, size_t expected, std::string& out);
struct ZipEntry {
    std::string name;   // UTF-8 path inside the archive
    std::wstring file;  // file on disk to store
};
bool writeZip(const std::wstring& path, const std::vector<ZipEntry>& entries, std::string& error);

// ---- Projects: web projects in a data folder, with per-turn checkpoints. Same rules as the phone's Projects.java.
constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr size_t kMaxFiles = 400;
constexpr size_t kMaxCheckpoints = 20;
constexpr size_t kMaxBody = 8 * 1024 * 1024;

struct FileInfo {
    std::string path;
    long long size = 0;
};
struct ProjectInfo {
    std::string name;
    int files = 0;
    long long updated = 0;
};

class ProjectStore {
public:
    explicit ProjectStore(const std::wstring& dataDir);
    static bool validName(const std::wstring& name);
    bool list(std::vector<ProjectInfo>& out, std::string& error) const;
    bool create(const std::string& name, std::string& error);
    bool remove(const std::string& name, std::string& error);
    bool files(const std::string& name, std::vector<FileInfo>& out, std::string& error) const;
    bool read(const std::string& name, const std::string& path, std::string& text, std::string& error) const;
    bool readBytes(const std::string& name, const std::string& path, std::string& data, std::string& error) const;
    bool write(const std::string& name, const std::string& path, const std::string& text, std::string& error);
    bool removeFile(const std::string& name, const std::string& path, std::string& error);
    bool readInternal(const std::string& name, const std::string& file, std::string& text, std::string& error) const;
    bool writeInternal(const std::string& name, const std::string& file, const std::string& text, std::string& error);
    bool checkpoint(const std::string& name, std::string& id, std::string& error);
    bool checkpoints(const std::string& name, std::vector<std::string>& ids, std::string& error) const;
    bool restore(const std::string& name, const std::string& id, std::string& error);
    bool exportZip(const std::string& name, const std::wstring& target, std::string& error) const;

private:
    std::wstring root_, checkpoints_;
    bool projectDir(const std::string& name, std::wstring& dir, std::string& error) const;
    bool resolve(const std::string& name, const std::string& relative, std::wstring& target, std::string& error) const;
    bool checkpointsOf(const std::string& name, std::wstring& dir, std::string& error) const;
};

// ---- API keys, protected with the Windows user's DPAPI; the page only ever learns whether one is set.
class KeyVault {
public:
    explicit KeyVault(const std::wstring& dataDir);
    bool has(const std::string& provider) const;
    bool put(const std::string& provider, const std::string& key);
    bool get(const std::string& provider, std::string& key) const;
    void clear(const std::string& provider);

private:
    std::wstring dir_;
};

// ---- Model providers over HTTPS. The key is added here and removed from whatever comes back.
struct ModelResponse {
    int status = 0;
    std::string body;
};
class ModelClient {
public:
    // Tests point both providers at a local server with CCA_TEST_HAIKU_ANTHROPIC / CCA_TEST_HAIKU_OPENAI.
    ModelClient();
    static bool knownProvider(const std::string& provider);
    bool models(const std::string& provider, const std::string& key, ModelResponse& out, std::string& error) const;
    bool send(const std::string& provider, const std::string& key, const std::string& body, ModelResponse& out,
              std::string& error) const;

private:
    std::string anthropic_, openai_;
    bool call(const std::string& provider, const std::string& key, const wchar_t* method, const std::string& path,
              const std::string* body, ModelResponse& out, std::string& error) const;
};

// ---- The bridge: the editor page's calls, answered as the Android shell answers them.
struct Reply {
    bool ok = false;
    Json value;
    std::string error;
};
class Host {
public:
    // `dataDir` holds projects, checkpoints and keys; `exportDir` receives ZIP exports.
    Host(const std::wstring& dataDir, const std::wstring& exportDir);
    Reply handle(const std::string& method, const Json& params);
    ProjectStore& projects() { return projects_; }
    // Methods that wait on the network; callers run these off the UI thread.
    static bool slow(const std::string& method) { return method == "models.list" || method == "model.send"; }
    // The reply message the page's Haiku.receive expects, as a JSON string literal safe to put in a script.
    static std::string replyScript(const std::string& id, const Reply& reply);

private:
    ProjectStore projects_;
    KeyVault vault_;
    ModelClient client_;
    std::wstring exportDir_;
};

// MIME type for a file name, as the phone shell serves it.
std::string contentType(const std::string& path);
// The default export folder: Downloads\CCA Haiku.
std::wstring defaultExportDir();

}  // namespace haikusim
