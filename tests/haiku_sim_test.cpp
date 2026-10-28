// Tests of the Haiku phone simulator's native side: the same checks as the phone's CoreTest.java (project paths, files,
// checkpoints, export, the model client against a local server), plus the parts Windows adds: inflate against zlib,
// reading the real APK, DPAPI keys, and the bridge replies. No model quota and no network beyond loopback.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "haiku_sim.hpp"
#include "util.hpp"

#pragma comment(lib, "ws2_32.lib")

using namespace haikusim;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

// zlib output of plainText() (raw deflate), generated once; the test rebuilds the text and compares.
const unsigned char kDynamic[] = {
    115, 118, 118, 84,  240, 72,  204, 204, 46,  85,  40,  47,  202, 44,  73,  45,  86,  40,  206, 77,  204, 201,
    81,  40,  79,  77,  82,  40,  40,  202, 207, 74,  77,  46,  41,  86,  200, 207, 83,  168, 204, 47,  45,  82,
    40,  200, 200, 207, 75,  213, 83,  112, 30,  217, 90,  210, 74,  243, 146, 75,  50,  129, 130, 137, 41,  41,
    26,  137, 58,  73,  154, 213, 69,  169, 37,  165, 69,  64,  190, 118, 82,  173, 130, 190, 190, 66,  113, 105,
    110, 49,  215, 96,  85,  165, 160, 168, 164, 172, 162, 170, 166, 174, 161, 169, 165, 173, 163, 171, 167, 111,
    96,  104, 100, 108, 98,  106, 102, 110, 97,  105, 101, 109, 99,  107, 103, 239, 224, 232, 228, 236, 226, 234,
    230, 238, 225, 233, 229, 237, 227, 235, 231, 31,  16,  24,  20,  28,  18,  26,  22,  30,  17,  25,  21,  29,
    19,  27,  23,  159, 144, 152, 148, 156, 146, 154, 150, 158, 145, 153, 149, 157, 147, 155, 151, 95,  80,  88,
    84,  92,  82,  90,  86,  94,  81,  89,  85,  93,  83,  91,  71,  99,  227, 1};
const unsigned char kFixed[] = {
    115, 118, 118, 84,  240, 72,  204, 204, 46,  85,  40,  47,  202, 44,  73,  45,  86,  40,  206, 77,  204, 201,
    81,  40,  79,  77,  82,  40,  40,  202, 207, 74,  77,  46,  41,  86,  200, 207, 83,  168, 204, 47,  45,  82,
    40,  200, 200, 207, 75,  213, 83,  112, 30,  217, 90,  210, 74,  243, 146, 75,  50,  129, 130, 137, 41,  41,
    26,  137, 58,  73,  154, 213, 69,  169, 37,  165, 69,  64,  190, 118, 82,  173, 130, 190, 190, 66,  113, 105,
    110, 49,  215, 96,  85,  165, 160, 168, 164, 172, 162, 170, 166, 174, 161, 169, 165, 173, 163, 171, 167, 111,
    96,  104, 100, 108, 98,  106, 102, 110, 97,  105, 101, 109, 99,  107, 103, 239, 224, 232, 228, 236, 226, 234,
    230, 238, 225, 233, 229, 237, 227, 235, 231, 31,  16,  24,  20,  28,  18,  26,  22,  30,  17,  25,  21,  29,
    19,  27,  23,  159, 144, 152, 148, 156, 146, 154, 150, 158, 145, 153, 149, 157, 147, 155, 151, 95,  80,  88,
    84,  92,  82,  90,  86,  94,  81,  89,  85,  93,  83,  91,  71,  99,  227, 1};
const unsigned char kStored[] = {
    1,   174, 2,   81,  253, 67,  67,  65,  32,  72,  97,  105, 107, 117, 32,  119, 114, 105, 116, 101, 115, 32,  115,
    109, 97,  108, 108, 32,  119, 101, 98,  32,  112, 114, 111, 106, 101, 99,  116, 115, 32,  111, 110, 32,  121, 111,
    117, 114, 32,  112, 104, 111, 110, 101, 46,  32,  67,  67,  65,  32,  72,  97,  105, 107, 117, 32,  119, 114, 105,
    116, 101, 115, 32,  115, 109, 97,  108, 108, 32,  119, 101, 98,  32,  112, 114, 111, 106, 101, 99,  116, 115, 32,
    111, 110, 32,  121, 111, 117, 114, 32,  112, 104, 111, 110, 101, 46,  32,  67,  67,  65,  32,  72,  97,  105, 107,
    117, 32,  119, 114, 105, 116, 101, 115, 32,  115, 109, 97,  108, 108, 32,  119, 101, 98,  32,  112, 114, 111, 106,
    101, 99,  116, 115, 32,  111, 110, 32,  121, 111, 117, 114, 32,  112, 104, 111, 110, 101, 46,  32,  67,  67,  65,
    32,  72,  97,  105, 107, 117, 32,  119, 114, 105, 116, 101, 115, 32,  115, 109, 97,  108, 108, 32,  119, 101, 98,
    32,  112, 114, 111, 106, 101, 99,  116, 115, 32,  111, 110, 32,  121, 111, 117, 114, 32,  112, 104, 111, 110, 101,
    46,  32,  67,  67,  65,  32,  72,  97,  105, 107, 117, 32,  119, 114, 105, 116, 101, 115, 32,  115, 109, 97,  108,
    108, 32,  119, 101, 98,  32,  112, 114, 111, 106, 101, 99,  116, 115, 32,  111, 110, 32,  121, 111, 117, 114, 32,
    112, 104, 111, 110, 101, 46,  32,  67,  67,  65,  32,  72,  97,  105, 107, 117, 32,  119, 114, 105, 116, 101, 115,
    32,  115, 109, 97,  108, 108, 32,  119, 101, 98,  32,  112, 114, 111, 106, 101, 99,  116, 115, 32,  111, 110, 32,
    121, 111, 117, 114, 32,  112, 104, 111, 110, 101, 46,  32,  102, 117, 110, 99,  116, 105, 111, 110, 32,  97,  100,
    100, 40,  97,  44,  98,  41,  123, 114, 101, 116, 117, 114, 110, 32,  97,  43,  98,  125, 32,  47,  47,  32,  115,
    117, 109, 115, 10,  102, 117, 110, 99,  116, 105, 111, 110, 32,  97,  100, 100, 40,  97,  44,  98,  41,  123, 114,
    101, 116, 117, 114, 110, 32,  97,  43,  98,  125, 32,  47,  47,  32,  115, 117, 109, 115, 10,  102, 117, 110, 99,
    116, 105, 111, 110, 32,  97,  100, 100, 40,  97,  44,  98,  41,  123, 114, 101, 116, 117, 114, 110, 32,  97,  43,
    98,  125, 32,  47,  47,  32,  115, 117, 109, 115, 10,  102, 117, 110, 99,  116, 105, 111, 110, 32,  97,  100, 100,
    40,  97,  44,  98,  41,  123, 114, 101, 116, 117, 114, 110, 32,  97,  43,  98,  125, 32,  47,  47,  32,  115, 117,
    109, 115, 10,  102, 117, 110, 99,  116, 105, 111, 110, 32,  97,  100, 100, 40,  97,  44,  98,  41,  123, 114, 101,
    116, 117, 114, 110, 32,  97,  43,  98,  125, 32,  47,  47,  32,  115, 117, 109, 115, 10,  32,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,
    83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105,
    106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102,
    103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126};

std::string plainText() {
    std::string t;
    for (int i = 0; i < 6; ++i) t += "CCA Haiku writes small web projects on your phone. ";
    for (int i = 0; i < 5; ++i) t += "function add(a,b){return a+b} // sums\n";
    for (int k = 0; k < 2; ++k)
        for (int c = 32; c < 127; ++c) t.push_back((char)c);
    return t;
}

// A loopback HTTP server that answers the model providers' paths and remembers the last request.
struct Server {
    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    std::mutex mu;
    std::string seen;
    bool stop = false;
    std::thread worker;
    Server() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener, (sockaddr*)&addr, sizeof addr);
        int len = sizeof addr;
        getsockname(listener, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        listen(listener, 8);
        worker = std::thread([this] { loop(); });
    }
    ~Server() {
        stop = true;
        closesocket(listener);
        worker.join();
        WSACleanup();
    }
    static std::string header(const std::string& request, const std::string& name) {
        std::string lower = util::toLower(request), key = "\r\n" + util::toLower(name) + ":";
        auto at = lower.find(key);
        if (at == std::string::npos) return "null";
        at += key.size();
        auto end = request.find("\r\n", at);
        return util::trim(request.substr(at, end - at));
    }
    void loop() {
        while (!stop) {
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::string request;
            char buf[4096];
            size_t need = 0;
            for (;;) {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                request.append(buf, n);
                auto split = request.find("\r\n\r\n");
                if (split == std::string::npos) continue;
                if (!need) {
                    auto cl = header(request, "content-length");
                    need = split + 4 + (cl == "null" ? 0 : (size_t)std::stoul(cl));
                }
                if (request.size() >= need) break;
            }
            auto line = request.substr(0, request.find("\r\n"));
            auto method = line.substr(0, line.find(' '));
            auto path = line.substr(line.find(' ') + 1);
            path = path.substr(0, path.find(' '));
            auto body = request.substr(request.find("\r\n\r\n") + 4);
            {
                std::lock_guard<std::mutex> lock(mu);
                seen = method + " " + path + " | " + header(request, "x-api-key") + " | " +
                       header(request, "anthropic-version") + " | " + header(request, "authorization") + " | " + body;
            }
            const bool bad = path.size() >= 12 && path.substr(path.size() - 12) == "/completions";
            const std::string reply =
                bad ? "{\"error\":{\"message\":\"invalid key sk-test-openai-123456\"}}" : "{\"ok\":true}";
            const std::string http =
                std::string("HTTP/1.1 ") + (bad ? "401 Unauthorized" : "200 OK") +
                "\r\ncontent-type: application/json\r\ncontent-length: " + std::to_string(reply.size()) +
                "\r\nconnection: close\r\n\r\n" + reply;
            send(c, http.data(), (int)http.size(), 0);
            closesocket(c);
        }
    }
    std::string last() {
        std::lock_guard<std::mutex> lock(mu);
        return seen;
    }
};

bool refuses(bool ok, const std::string&) {
    return !ok;
}
std::wstring tempRoot() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = util::joinPath(tmp, util::widen("cca-haiku-sim-" + util::newId()));
    util::ensureDir(root);
    return root;
}
}  // namespace

int main() {
    const auto root = tempRoot();

    // ---- inflate: three block types against zlib's output
    {
        const std::string plain = plainText();
        std::string out;
        check(inflateRaw(std::string((const char*)kDynamic, sizeof kDynamic), plain.size(), out) && out == plain,
              "inflate: zlib's best-compression stream");
        check(inflateRaw(std::string((const char*)kFixed, sizeof kFixed), plain.size(), out) && out == plain,
              "inflate: fixed Huffman block");
        check(inflateRaw(std::string((const char*)kStored, sizeof kStored), plain.size(), out) && out == plain,
              "inflate: stored block");
        std::string cut((const char*)kDynamic, sizeof kDynamic / 2);
        check(!inflateRaw(cut, plain.size(), out), "inflate: a truncated stream is refused");
        check(!inflateRaw(std::string((const char*)kDynamic, sizeof kDynamic), plain.size() - 1, out),
              "inflate: more output than announced is refused");
    }

    // ---- ZIP: write what the simulator exports, read it back with the APK reader
    {
        const auto folder = util::joinPath(root, L"zip");
        util::ensureDir(folder);
        util::writeFileAtomic(util::joinPath(folder, L"a.txt"), "alpha\n");
        util::writeFileAtomic(util::joinPath(folder, L"b.txt"), std::string(5000, 'b'));
        std::string error;
        const auto target = util::joinPath(root, L"out.zip");
        check(writeZip(target,
                       {{"assets/www/a.txt", util::joinPath(folder, L"a.txt")},
                        {"assets/www/js/b.txt", util::joinPath(folder, L"b.txt")},
                        {"other/c.txt", util::joinPath(folder, L"a.txt")}},
                       error),
              "zip: an archive is written");
        std::string data;
        util::readFile(target, data);
        std::map<std::string, std::string> files;
        check(readZipFolder(data, "assets/www/", files, error) && files.size() == 2 && files["a.txt"] == "alpha\n" &&
                  files["js/b.txt"] == std::string(5000, 'b'),
              "zip: the reader returns the folder's entries unchanged and skips the rest");
        data[47] ^= 0x55;  // inside the first stored entry's content, after its 30-byte header and 16-byte name
        check(!readZipFolder(data, "assets/www/", files, error), "zip: a damaged entry fails its CRC");
        check(!readZipFolder("not a zip at all, but long enough to be looked at....", "", files, error),
              "zip: garbage is refused");
    }

    // ---- the real APK, when it has been built: its web assets must match the sources they came from
    {
        std::wstring apk = util::joinPath(util::exeDir(), L"android\\CCA-Haiku-0.0.2.apk");
        if (!util::fileExists(apk)) apk = L"build\\android\\CCA-Haiku-0.0.2.apk";
        if (util::fileExists(apk)) {
            std::string data, error, source;
            util::readFile(apk, data);
            std::map<std::string, std::string> assets;
            check(readZipFolder(data, "assets/www/", assets, error) && assets.count("index.html") &&
                      assets.count("haiku.js") && assets.count("haiku.css"),
                  "apk: the editor page is read out of the APK");
            check(!assets.count("mock-host.js"), "apk: the development stand-in host is not packaged");
            if (util::readFile(L"android\\haiku\\web\\haiku.js", source))
                check(assets["haiku.js"] == source, "apk: haiku.js equals the source file byte for byte");
        } else {
            std::printf("SKIP apk checks: build\\android\\CCA-Haiku-0.0.2.apk is not built\n");
        }
    }

    // ---- projects (CoreTest.java)
    {
        ProjectStore p(util::joinPath(root, L"store"));
        std::string error, text, id;
        check(ProjectStore::validName(L"記帳小工具") && ProjectStore::validName(L"my app_2") &&
                  !ProjectStore::validName(L"../x") && !ProjectStore::validName(L".hidden") &&
                  !ProjectStore::validName(L"a/b") && !ProjectStore::validName(L"") &&
                  !ProjectStore::validName(L"CON") && !ProjectStore::validName(L"lpt1") &&
                  !ProjectStore::validName(std::wstring(41, L'a')) && !ProjectStore::validName(L"trailing "),
              "project names allow any script but no paths or Windows device names");
        const std::string name = util::narrow(L"記帳小工具");
        check(p.create(name, error), "a project is created");
        std::vector<FileInfo> files;
        check(p.files(name, files, error) && files.size() == 3 && p.read(name, "index.html", text, error) &&
                  text.find("<title>" + name + "</title>") != std::string::npos,
              "a new project starts with a page, a stylesheet and a script");
        check(!p.create(name, error), "a second project with the same name is refused");
        for (const char* bad :
             {"../escape.txt", "/etc/passwd", "C:/x.txt", "a\\b.txt", "a//b.txt", "./a.txt", ".haiku/chat.json",
              ".haiku", ".HAIKU/chat.json", "x/../../y", "con.txt", "a/NUL", "trail.", "bad|name.txt", "a:b"}) {
            check(!p.write(name, bad, "x", error), (std::string("path refused: ") + bad).c_str());
        }
        check(p.write(name, "js/chart.js", "const x = 1;\n", error) && p.read(name, "js/chart.js", text, error) &&
                  text == "const x = 1;\n",
              "nested files are written and read back");
        check(!util::fileExists(util::joinPath(root, L"store\\escape.txt")) &&
                  !util::fileExists(util::joinPath(root, L"store\\projects\\escape.txt")),
              "nothing was written outside the project");
        check(p.writeInternal(name, "chat.json", "{\"turns\":[]}", error), "the conversation is saved");
        bool hidden = true;
        p.files(name, files, error);
        for (const auto& f : files) hidden = hidden && f.path.rfind(".haiku", 0) != 0;
        check(hidden && p.readInternal(name, "chat.json", text, error) && text == "{\"turns\":[]}",
              "the conversation is stored but never listed as a project file");
        check(!p.writeInternal(name, "../x.json", "x", error) && !p.writeInternal(name, "Chat.json", "x", error),
              "internal file names are restricted");
        check(!p.write(name, "big.txt", std::string(kMaxFileBytes + 1, 'a'), error), "files over 2 MiB are refused");

        check(p.checkpoint(name, id, error), "a checkpoint is taken");
        p.write(name, "index.html", "<p>changed</p>", error);
        p.write(name, "extra/new.html", "<p>new</p>", error);
        p.removeFile(name, "style.css", error);
        check(p.restore(name, id, error), "a checkpoint is restored");
        check(p.read(name, "index.html", text, error) &&
                  text.find("<title>" + name + "</title>") != std::string::npos &&
                  !p.read(name, "extra/new.html", text, error) && p.read(name, "style.css", text, error) &&
                  text.find("body") != std::string::npos,
              "restoring a checkpoint brings back changed, added and deleted files");
        check(!util::dirExists(util::joinPath(root, L"store\\projects\\" + util::widen(name) + L"\\extra")),
              "empty folders left by a restore are removed");
        check(p.readInternal(name, "chat.json", text, error) && text == "{\"turns\":[]}",
              "restoring files keeps the conversation");
        for (size_t i = 0; i < kMaxCheckpoints + 3; ++i) p.checkpoint(name, id, error);
        std::vector<std::string> ids;
        check(p.checkpoints(name, ids, error) && ids.size() == kMaxCheckpoints,
              "only the most recent checkpoints are kept");
        check(!p.restore(name, "../1", error) && !p.restore(name, "123", error),
              "restore refuses malformed or unknown checkpoints");

        // a link inside the project must not be followed
        const auto outside = util::joinPath(root, L"outside");
        util::ensureDir(outside);
        util::writeFileAtomic(util::joinPath(outside, L"secret.txt"), "secret");
        const auto link = util::joinPath(root, L"store\\projects\\" + util::widen(name) + L"\\jump");
        wchar_t cmd[1024];
        std::swprintf(cmd, 1024, L"cmd /c mklink /J \"%s\" \"%s\" >nul", link.c_str(), outside.c_str());
        _wsystem(cmd);
        if (util::dirExists(link)) {
            check(!p.read(name, "jump/secret.txt", text, error), "a junction inside the project is refused");
            check(!p.write(name, "jump/new.txt", "x", error) && !util::fileExists(util::joinPath(outside, L"new.txt")),
                  "nothing is written through a junction");
            p.files(name, files, error);
            bool listed = false;
            for (const auto& f : files) listed = listed || f.path.rfind("jump", 0) == 0;
            check(!listed, "a junction is not listed as a project folder");
            RemoveDirectoryW(link.c_str());
        } else {
            std::printf("SKIP junction checks: mklink /J is unavailable\n");
        }

        // export: ASCII project so the APK reader's name rules apply to the archive too
        check(p.create("demo", error) && p.write("demo", "js/app.js", "1", error), "an ASCII project is created");
        const auto zipPath = util::joinPath(root, L"exports\\demo.zip");
        check(p.exportZip("demo", zipPath, error), "a project is exported to a ZIP");
        std::string data;
        util::readFile(zipPath, data);
        std::map<std::string, std::string> entries;
        check(readZipFolder(data, "demo/", entries, error) && entries.count("index.html") &&
                  entries.count("js/app.js") && entries.size() == 4,
              "the exported ZIP holds the project files");
        check(data.find(".haiku") == std::string::npos, "the exported ZIP has none of Haiku's own data");
        std::vector<ProjectInfo> projects;
        check(p.list(projects, error) && projects.size() == 2, "projects are listed");
        check(p.remove(name, error) && p.remove("demo", error) && p.list(projects, error) && projects.empty(),
              "a deleted project is gone");
    }

    // ---- keys
    {
        KeyVault vault(util::joinPath(root, L"vault"));
        std::string key;
        check(!vault.has("anthropic") && vault.put("anthropic", "sk-ant-test-1234567890") && vault.has("anthropic") &&
                  vault.get("anthropic", key) && key == "sk-ant-test-1234567890",
              "a key is stored with DPAPI and read back");
        std::string stored;
        util::readFile(util::joinPath(root, L"vault\\keys\\anthropic.key"), stored);
        check(stored.find("sk-ant-test") == std::string::npos, "the key file does not hold the key in clear");
        vault.clear("anthropic");
        check(!vault.has("anthropic") && !vault.get("anthropic", key), "a cleared key is gone");
    }

    // ---- model client and the bridge, against the loopback server
    {
        Server server;
        const std::string base = "http://127.0.0.1:" + std::to_string(server.port);
        SetEnvironmentVariableW(L"CCA_TEST_HAIKU_ANTHROPIC", util::widen(base + "/anthropic").c_str());
        SetEnvironmentVariableW(L"CCA_TEST_HAIKU_OPENAI", util::widen(base + "/openai").c_str());
        Host host(util::joinPath(root, L"host"), util::joinPath(root, L"host-exports"));
        auto call = [&](const char* method, Json params = Json::object()) { return host.handle(method, params); };

        check(call("keys.status").value["anthropic"].boolean() == false, "keys.status starts empty");
        check(!call("keys.set", J({{"provider", "gemini"}, {"key", "sk-test-1234567"}})).ok,
              "keys.set refuses an unknown provider");
        check(!call("keys.set", J({{"provider", "anthropic"}, {"key", "short"}})).ok, "keys.set refuses a short key");
        check(!call("keys.set", J({{"provider", "anthropic"}, {"key", "has space in it 123456"}})).ok,
              "keys.set refuses a key with whitespace");
        check(!call("model.send", J({{"provider", "anthropic"}, {"body", "{}"}})).ok,
              "a request without a key is refused before any request");
        check(call("keys.set", J({{"provider", "anthropic"}, {"key", "sk-test-anthropic-123456"}})).ok &&
                  call("keys.set", J({{"provider", "openai"}, {"key", "sk-test-openai-123456"}})).ok,
              "keys are set");
        const Json status = call("keys.status").value;
        check(status["anthropic"].boolean() && status["openai"].boolean() &&
                  status.dump().find("sk-test") == std::string::npos,
              "the page learns only that keys exist");

        Reply r = call("model.send", J({{"provider", "anthropic"}, {"body", "{\"model\":\"m\"}"}}));
        check(r.ok && r.value["status"].i64() == 200 &&
                  server.last() ==
                      "POST /anthropic/messages | sk-test-anthropic-123456 | 2023-06-01 | null | {\"model\":\"m\"}",
              "Anthropic requests carry the key and version headers and the body unchanged");
        r = call("models.list", J({{"provider", "openai"}}));
        check(r.ok && r.value["status"].i64() == 200 &&
                  server.last().rfind("GET /openai/models | null | null | Bearer sk-test-openai-123456", 0) == 0,
              "OpenAI model listing uses a bearer token");
        r = call("model.send", J({{"provider", "openai"}, {"body", "{}"}}));
        check(r.ok && r.value["status"].i64() == 401 &&
                  r.value["body"].str().find("sk-test-openai-123456") == std::string::npos &&
                  r.value["body"].str().find("[已隱藏]") != std::string::npos,
              "a provider error that echoes the key is redacted before it reaches the page");
        check(!call("model.send", J({{"provider", "gemini"}, {"body", "{}"}})).ok, "an unknown provider is refused");
        check(!call("model.send", J({{"provider", "anthropic"}, {"body", std::string(kMaxBody + 1, 'x')}})).ok,
              "a request over 8 MiB is refused");

        // bridge replies
        check(!call("no.such.method").ok && call("no.such.method").error.find("不支援的操作") == 0,
              "an unknown method is refused with the phone's message");
        check(!call("files.read", J({{"project", "ghost"}, {"path", "index.html"}})).ok,
              "reading a missing project fails");
        check(call("projects.create", J({{"name", "api test"}})).ok && call("projects.list").value.size() == 1 &&
                  call("projects.list").value.at(0)["files"].i64() == 3,
              "projects.create and list");
        check(!call("projects.create", J({})).ok && call("projects.create", J({})).error == "缺少參數 name",
              "a missing parameter is named in the error");
        check(call("preview.open", J({{"project", "api test"}})).ok &&
                  !call("preview.open", J({{"project", "ghost"}})).ok,
              "preview.open fails for a missing project before anything opens");
        Reply took = call("checkpoint.take", J({{"project", "api test"}}));
        check(took.ok && took.value.isString() &&
                  call("files.write", J({{"project", "api test"}, {"path", "a.txt"}, {"text", "hi"}})).ok &&
                  call("checkpoint.restore", J({{"project", "api test"}, {"checkpoint", took.value.str()}})).ok &&
                  !call("files.read", J({{"project", "api test"}, {"path", "a.txt"}})).ok,
              "checkpoint.take and restore through the bridge");
        Reply exported = call("export.zip", J({{"project", "api test"}}));
        check(exported.ok && util::fileExists(util::widen(exported.value.str())) &&
                  util::startsWith(exported.value.str(), util::narrow(util::joinPath(root, L"host-exports"))),
              "export.zip writes into the export folder and returns its path");
        std::string script =
            Host::replyScript("7", call("files.read", J({{"project", "api test"}, {"path", "app.js"}})));
        check(script.rfind("window.Haiku.receive(\"", 0) == 0 && script.back() == ')',
              "the reply is a call to Haiku.receive with a string literal");
        Reply odd;
        odd.ok = true;
        odd.value =
            "a\xE2\x80\xA8"
            "b\xE2\x80\xA9"
            "c";
        script = Host::replyScript("8", odd);
        check(script.find("\xE2\x80\xA8") == std::string::npos && script.find("\xE2\x80\xA9") == std::string::npos &&
                  script.find("\\u2028") != std::string::npos && script.find("\\u2029") != std::string::npos,
              "U+2028 and U+2029 are escaped so they cannot end the script string");
        check(Host::slow("model.send") && Host::slow("models.list") && !Host::slow("files.read"),
              "only the network calls are marked slow");
    }

    std::printf("%d Haiku simulator checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
