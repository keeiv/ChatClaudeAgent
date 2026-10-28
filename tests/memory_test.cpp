// Memory stress for the parsers and stores that read untrusted input (JSON, diffs, ZIP/deflate, project files).
// Built normally it checks that private bytes stay flat over thousands of iterations; built with CCA_SANITIZE=1
// (MSVC AddressSanitizer) the same loops also catch overflows, use-after-free and leaks. Inputs come from a fixed-seed
// generator so a failure reproduces. No network, no models; the data folder is a fresh temp directory.
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "diff.hpp"
#include "haiku_sim.hpp"
#include "humanize.hpp"
#include "json.hpp"
#include "util.hpp"

#pragma comment(lib, "psapi.lib")

void postToUi(std::function<void()> fn) { fn(); }

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

struct Rng {
    uint64_t s = 0x9e3779b97f4a7c15ull;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 11);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof info;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof info);
    return info.PrivateUsage;
}

// Runs `body` a few warm-up rounds, then measures how much private memory a much longer run adds.
// Under AddressSanitizer freed blocks sit in a quarantine, so private bytes say nothing about leaks there (and MSVC has
// no leak sanitizer); that build only runs the loops to catch overflows and use-after-free, and growth is just printed.
void leakCheck(const char* name, int rounds, const std::function<void(int)>& body) {
#ifdef __SANITIZE_ADDRESS__
    const size_t budget = SIZE_MAX;
#else
    const size_t budget = 16u << 20;
#endif
    for (int i = 0; i < rounds / 10 + 1; ++i) body(i);
    const size_t before = privateBytes();
    for (int i = 0; i < rounds; ++i) body(i);
    const size_t after = privateBytes();
    const size_t grew = after > before ? after - before : 0;
    std::printf("  %-28s +%zu KiB over %d rounds\n", name, grew >> 10, rounds);
    check(grew < budget, name);
}

std::string randomBytes(Rng& rng, size_t n) {
    std::string out(n, '\0');
    for (auto& c : out) c = char(rng.below(256));
    return out;
}

std::string randomJsonText(Rng& rng, int depth) {
    switch (depth > 6 ? rng.below(4) : rng.below(7)) {
    case 0: return std::to_string(int(rng.below(100000)) - 50000);
    case 1: return "\"" + std::string("a\\n\\u00e9\\ud83d\\ude00 中文").substr(0, 4 + rng.below(20)) + "\"";
    case 2: return rng.below(2) ? "true" : "false";
    case 3: return "null";
    case 4: {
        std::string out = "[";
        for (uint32_t i = 0, n = rng.below(5); i < n; ++i) out += (i ? "," : "") + randomJsonText(rng, depth + 1);
        return out + "]";
    }
    default: {
        std::string out = "{";
        for (uint32_t i = 0, n = rng.below(5); i < n; ++i)
            out += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":" + randomJsonText(rng, depth + 1);
        return out + "}";
    }
    }
}

void mutate(Rng& rng, std::string& text) {
    for (uint32_t i = 0, n = 1 + rng.below(4); i < n && !text.empty(); ++i) {
        const size_t at = rng.below(uint32_t(text.size()));
        switch (rng.below(3)) {
        case 0: text[at] = char(rng.below(256)); break;
        case 1: text.erase(at, 1 + rng.below(8)); break;
        default: text.insert(at, 1, "{}[]\":,\\u"[rng.below(9)]); break;
        }
    }
}

void jsonRounds() {
    Rng rng;
    leakCheck("json parse and dump", 20000, [&](int) {
        std::string text = randomJsonText(rng, 0);
        std::string error;
        Json whole = Json::parse(text, &error);
        const std::string dumped = whole.dump();
        std::string again;
        Json reparsed = Json::parse(dumped, &again);
        if (error.empty()) check(again.empty() && reparsed == whole, "json round trip");
        mutate(rng, text);
        std::string ignored;
        Json::parse(text, &ignored).dump();
    });
    // Nesting deeper than the parser allows must be refused, not recursed into until the stack runs out.
    std::string deep(200000, '[');
    std::string error;
    check(Json::parse(deep, &error).isNull() && !error.empty(), "json refuses runaway nesting");
    std::string deepObject;
    for (int i = 0; i < 100000; ++i) deepObject += "{\"a\":";
    error.clear();
    check(Json::parse(deepObject, &error).isNull() && !error.empty(), "json refuses deep objects");
    std::string big = "[";
    for (int i = 0; i < 200000; ++i) big += (i ? ",1" : "1");
    big += "]";
    error.clear();
    check(Json::parse(big, &error).size() == 200000, "json wide array");
}

void diffRounds() {
    Rng rng;
    leakCheck("diff and verdict parsing", 4000, [&](int) {
        std::string before, after;
        for (uint32_t i = 0, n = rng.below(60); i < n; ++i) before += "line " + std::to_string(rng.below(30)) + "\n";
        for (uint32_t i = 0, n = rng.below(60); i < n; ++i) after += "line " + std::to_string(rng.below(30)) + "\n";
        DiffStat stat;
        const std::string diff = lineDiff(before, after, &stat);
        countUnifiedDiff(diff);
        newFileDiff(after, &stat);
        std::string stripped;
        parseVerdict(randomBytes(rng, rng.below(300)) + "\nVERDICT: APPROVE", &stripped);
        normalizeCodexDiff(rng.below(2) ? "add" : "update", randomBytes(rng, rng.below(400)), &stat);
        humanizeInstructions(randomBytes(rng, rng.below(2000)));
    });
}

void zipRounds() {
    Rng rng;
    std::string out;
    // Garbage and truncated deflate streams must fail cleanly at any claimed size.
    leakCheck("inflate on garbage", 20000, [&](int) {
        const std::string junk = randomBytes(rng, rng.below(600));
        out.clear();
        haikusim::inflateRaw(junk, rng.below(5000), out);
    });
    const std::wstring dir = util::joinPath(util::getEnv(L"TEMP"), L"cca-memory-" + util::widen(util::randomHex(4)));
    util::ensureDir(dir);
    const std::wstring a = util::joinPath(dir, L"a.txt");
    util::writeFileAtomic(a, std::string(5000, 'x'));
    const std::wstring zipPath = util::joinPath(dir, L"t.zip");
    std::string error;
    check(haikusim::writeZip(zipPath, {{"assets/www/a.txt", a}}, error), "writeZip for the stress input");
    std::string zip;
    check(util::readFile(zipPath, zip), "read the zip back");
    leakCheck("zip reader on mutated archives", 10000, [&](int) {
        std::string copy = zip;
        mutate(rng, copy);
        if (rng.below(4) == 0) copy.resize(rng.below(uint32_t(copy.size() + 1)));
        std::map<std::string, std::string> files;
        std::string problem;
        haikusim::readZipFolder(copy, "assets/www/", files, problem);
    });
    DeleteFileW(a.c_str());
    DeleteFileW(zipPath.c_str());
    RemoveDirectoryW(dir.c_str());
}

void projectRounds() {
    const std::wstring dir = util::joinPath(util::getEnv(L"TEMP"), L"cca-memory-proj-" + util::widen(util::randomHex(4)));
    util::ensureDir(dir);
    {
        haikusim::ProjectStore store(dir);
        std::string error;
        check(store.create("stress", error), "create project");
        Rng rng;
        leakCheck("project files and checkpoints", 1500, [&](int i) {
            std::string problem;
            store.write("stress", "page" + std::to_string(i % 20) + ".html", randomBytes(rng, rng.below(3000)), problem);
            std::string text;
            store.read("stress", "page" + std::to_string(i % 20) + ".html", text, problem);
            // Hostile names must be refused every time.
            check(!store.write("stress", "..\\..\\escape" + std::to_string(i) + ".txt", "x", problem), "path escape refused");
            check(!store.write("stress", "con.txt", "x", problem), "device name refused");
            if (i % 50 == 0) {
                std::string id;
                store.checkpoint("stress", id, problem);
            }
            std::vector<haikusim::FileInfo> files;
            store.files("stress", files, problem);
        });
        store.remove("stress", error);
    }
    RemoveDirectoryW(util::joinPath(dir, L"projects").c_str());
    RemoveDirectoryW(dir.c_str());
}
}  // namespace

int main() {
    std::printf("memory test (%s)\n",
#ifdef __SANITIZE_ADDRESS__
                "AddressSanitizer"
#else
                "private-bytes growth"
#endif
    );
    jsonRounds();
    diffRounds();
    zipRounds();
    projectRounds();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
