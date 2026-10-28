// Native side of the Haiku phone simulator; see haiku_sim.hpp. The rules follow the phone's Projects.java and
// ModelClient.java line by line, with a few extra refusals for what Windows file names allow and Android's do not.
#include "haiku_sim.hpp"

#include <windows.h>

#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "util.hpp"

namespace haikusim {
namespace {

std::wstring W(const std::string& s) {
    return util::widen(s);
}
std::string U(const std::wstring& s) {
    return util::narrow(s);
}

// ------------------------------------------------------------------ CRC-32 and little-endian helpers
uint32_t crc32Of(const char* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ (uint8_t)data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
uint16_t le16(const std::string& s, size_t at) {
    return (uint16_t)((uint8_t)s[at] | ((uint8_t)s[at + 1] << 8));
}
uint32_t le32(const std::string& s, size_t at) {
    return (uint32_t)((uint8_t)s[at] | ((uint8_t)s[at + 1] << 8) | ((uint8_t)s[at + 2] << 16) |
                      ((uint32_t)(uint8_t)s[at + 3] << 24));
}
void put16(std::string& s, uint16_t v) {
    s.push_back((char)(v & 255));
    s.push_back((char)(v >> 8));
}
void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 255));
}

// ------------------------------------------------------------------ inflate (the canonical decoder of RFC 1951)
struct Bits {
    const uint8_t* data;
    size_t size, pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool failed = false;
    uint32_t take(int need) {
        while (count < need) {
            if (pos >= size) {
                failed = true;
                return 0;
            }
            buffer |= (uint32_t)data[pos++] << count;
            count += 8;
        }
        uint32_t v = buffer & ((1u << need) - 1);
        buffer >>= need;
        count -= need;
        return v;
    }
};
struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};
bool buildHuffman(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;  // over-subscribed
    }
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) offsets[len + 1] = offsets[len] + h.count[len];
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offsets[lengths[i]]++] = (uint16_t)i;
    return true;
}
int decodeSymbol(Bits& bits, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= (int)bits.take(1);
        if (bits.failed) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
const uint16_t kLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                              193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateCodes(Bits& bits, const Huffman& lit, const Huffman& dist, std::string& out, size_t limit) {
    for (;;) {
        int symbol = decodeSymbol(bits, lit);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= limit) return false;
            out.push_back((char)symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            const size_t len = kLengthBase[symbol] + bits.take(kLengthExtra[symbol]);
            const int d = decodeSymbol(bits, dist);
            if (d < 0 || d >= 30) return false;
            const size_t distance = kDistBase[d] + bits.take(kDistExtra[d]);
            if (bits.failed || distance > out.size() || out.size() + len > limit) return false;
            for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - distance]);
        }
    }
}

}  // namespace

bool inflateRaw(const std::string& in, size_t expected, std::string& out) {
    out.clear();
    out.reserve(expected);
    Bits bits{(const uint8_t*)in.data(), in.size()};
    int last;
    do {
        last = (int)bits.take(1);
        const int type = (int)bits.take(2);
        if (bits.failed) return false;
        if (type == 0) {
            bits.buffer = 0;
            bits.count = 0;
            if (bits.pos + 4 > bits.size) return false;
            const size_t len = bits.data[bits.pos] | (bits.data[bits.pos + 1] << 8);
            const size_t inv = bits.data[bits.pos + 2] | (bits.data[bits.pos + 3] << 8);
            bits.pos += 4;
            if ((len ^ 0xFFFF) != inv || bits.pos + len > bits.size || out.size() + len > expected) return false;
            out.append((const char*)bits.data + bits.pos, len);
            bits.pos += len;
        } else if (type == 1) {
            static Huffman fixedLit, fixedDist;
            static bool built = false;
            if (!built) {
                uint8_t lengths[288];
                int i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                buildHuffman(fixedLit, lengths, 288);
                for (i = 0; i < 30; ++i) lengths[i] = 5;
                buildHuffman(fixedDist, lengths, 30);
                built = true;
            }
            if (!inflateCodes(bits, fixedLit, fixedDist, out, expected)) return false;
        } else if (type == 2) {
            const int nlen = (int)bits.take(5) + 257, ndist = (int)bits.take(5) + 1, ncode = (int)bits.take(4) + 4;
            if (bits.failed || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)bits.take(3);
            Huffman codeLengths;
            if (bits.failed || !buildHuffman(codeLengths, lengths, 19)) return false;
            int index = 0;
            uint8_t all[320] = {};
            while (index < nlen + ndist) {
                int symbol = decodeSymbol(bits, codeLengths);
                if (symbol < 0) return false;
                if (symbol < 16) {
                    all[index++] = (uint8_t)symbol;
                } else {
                    int len = 0, repeat;
                    if (symbol == 16) {
                        if (index == 0) return false;
                        len = all[index - 1];
                        repeat = 3 + (int)bits.take(2);
                    } else if (symbol == 17) {
                        repeat = 3 + (int)bits.take(3);
                    } else {
                        repeat = 11 + (int)bits.take(7);
                    }
                    if (bits.failed || index + repeat > nlen + ndist) return false;
                    while (repeat--) all[index++] = (uint8_t)len;
                }
            }
            if (all[256] == 0) return false;  // no end-of-block code
            Huffman lit, dist;
            if (!buildHuffman(lit, all, nlen) || !buildHuffman(dist, all + nlen, ndist)) return false;
            if (!inflateCodes(bits, lit, dist, out, expected)) return false;
        } else {
            return false;
        }
    } while (!last);
    return out.size() == expected;
}

namespace {
bool safeEntryName(const std::string& name) {
    if (name.empty() || name.size() > 200 || name.find("..") != std::string::npos) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '/' || c == '-';
    });
}
}  // namespace

bool readZipFolder(const std::string& zip, const std::string& prefix, std::map<std::string, std::string>& out,
                   std::string& error) {
    out.clear();
    if (zip.size() < 22 || zip.size() > 64u * 1024 * 1024) {
        error = "不是有效的 APK（大小不合理）";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22, stop = zip.size() > 65557 ? zip.size() - 65557 : 0;; --i) {
        if (le32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == stop) break;
    }
    if (eocd == std::string::npos) {
        error = "不是有效的 APK（找不到目錄）";
        return false;
    }
    const size_t entries = le16(zip, eocd + 10), dirSize = le32(zip, eocd + 12), dirAt = le32(zip, eocd + 16);
    if (entries == 0xFFFF || dirAt == 0xFFFFFFFFu || dirAt + dirSize > zip.size()) {
        error = "不支援這個 APK 的壓縮格式";
        return false;
    }
    size_t at = dirAt, total = 0;
    for (size_t n = 0; n < entries; ++n) {
        if (at + 46 > zip.size() || le32(zip, at) != 0x02014b50) {
            error = "APK 的檔案目錄損壞";
            return false;
        }
        const uint16_t method = le16(zip, at + 10), nameLen = le16(zip, at + 28), extraLen = le16(zip, at + 30),
                       commentLen = le16(zip, at + 32);
        const uint32_t crc = le32(zip, at + 16), packed = le32(zip, at + 20), size = le32(zip, at + 24),
                       local = le32(zip, at + 42);
        if (at + 46 + nameLen > zip.size()) {
            error = "APK 的檔案目錄損壞";
            return false;
        }
        const std::string name = zip.substr(at + 46, nameLen);
        at += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.compare(0, prefix.size(), prefix) != 0 || name.back() == '/') continue;
        if (!safeEntryName(name) || size > 4u * 1024 * 1024 || (total += size) > 16u * 1024 * 1024) {
            error = "APK 裡有不合規的檔案：" + name;
            return false;
        }
        if (local + 30 > zip.size() || le32(zip, local) != 0x04034b50) {
            error = "APK 的檔案標頭損壞：" + name;
            return false;
        }
        const size_t dataAt = (size_t)local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
        if (dataAt + packed > zip.size()) {
            error = "APK 的檔案內容不完整：" + name;
            return false;
        }
        std::string body;
        if (method == 0) {
            if (packed != size) {
                error = "APK 的檔案大小不符：" + name;
                return false;
            }
            body.assign(zip, dataAt, size);
        } else if (method == 8) {
            if (!inflateRaw(zip.substr(dataAt, packed), size, body)) {
                error = "APK 的檔案無法解壓縮：" + name;
                return false;
            }
        } else {
            error = "APK 使用不支援的壓縮方式：" + name;
            return false;
        }
        if (crc32Of(body.data(), body.size()) != crc) {
            error = "APK 的檔案內容與檢查碼不符：" + name;
            return false;
        }
        out[name.substr(prefix.size())] = std::move(body);
    }
    return true;
}

bool writeZip(const std::wstring& path, const std::vector<ZipEntry>& entries, std::string& error) {
    std::string archive, directory;
    SYSTEMTIME now;
    GetLocalTime(&now);
    const uint16_t dosTime = (uint16_t)((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
    const uint16_t dosDate = (uint16_t)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
    for (const auto& entry : entries) {
        std::string data;
        if (!util::readFile(entry.file, data)) {
            error = "無法讀取 " + entry.name;
            return false;
        }
        const uint32_t crc = crc32Of(data.data(), data.size());
        const uint32_t offset = (uint32_t)archive.size();
        put32(archive, 0x04034b50);
        put16(archive, 20);
        put16(archive, 0x0800);  // UTF-8 names
        put16(archive, 0);       // stored
        put16(archive, dosTime);
        put16(archive, dosDate);
        put32(archive, crc);
        put32(archive, (uint32_t)data.size());
        put32(archive, (uint32_t)data.size());
        put16(archive, (uint16_t)entry.name.size());
        put16(archive, 0);
        archive += entry.name;
        archive += data;
        put32(directory, 0x02014b50);
        put16(directory, 20);
        put16(directory, 20);
        put16(directory, 0x0800);
        put16(directory, 0);
        put16(directory, dosTime);
        put16(directory, dosDate);
        put32(directory, crc);
        put32(directory, (uint32_t)data.size());
        put32(directory, (uint32_t)data.size());
        put16(directory, (uint16_t)entry.name.size());
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, offset);
        directory += entry.name;
    }
    const uint32_t dirAt = (uint32_t)archive.size();
    archive += directory;
    put32(archive, 0x06054b50);
    put16(archive, 0);
    put16(archive, 0);
    put16(archive, (uint16_t)entries.size());
    put16(archive, (uint16_t)entries.size());
    put32(archive, (uint32_t)directory.size());
    put32(archive, dirAt);
    put16(archive, 0);
    if (!util::writeFileAtomic(path, archive)) {
        error = "無法寫入匯出檔案";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ file system helpers
namespace {
bool isReparse(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool isDirectory(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool isRegularFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
long long toMs(const FILETIME& t) {
    ULARGE_INTEGER u;
    u.LowPart = t.dwLowDateTime;
    u.HighPart = t.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ull) / 10000ull);
}
long long modified(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) ? toMs(d.ftLastWriteTime) : 0;
}
long long sizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
bool makeDirs(const std::wstring& p) {
    return SHCreateDirectoryExW(nullptr, p.c_str(), nullptr) == ERROR_SUCCESS || isDirectory(p);
}
struct Entry {
    std::wstring name;
    bool dir = false, reparse = false;
};
std::vector<Entry> listFolder(const std::wstring& dir) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW d{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = d.cFileName;
        if (n == L"." || n == L"..") continue;
        out.push_back({n, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
    } while (out.size() < 4096 && FindNextFileW(h, &d));
    FindClose(h);
    return out;
}
void deleteTree(const std::wstring& dir) {
    for (const auto& e : listFolder(dir)) {
        const std::wstring p = dir + L"\\" + e.name;
        if (e.dir && !e.reparse) deleteTree(p);
        else if (e.dir) RemoveDirectoryW(p.c_str());
        else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    }
    RemoveDirectoryW(dir.c_str());
}
// Windows file names Android would accept but Windows would not treat as ordinary files.
bool reservedDeviceName(std::wstring segment) {
    const auto dot = segment.find(L'.');
    if (dot != std::wstring::npos) segment.resize(dot);
    while (!segment.empty() && segment.back() == L' ') segment.pop_back();
    for (auto& c : segment) c = (wchar_t)towupper(c);
    if (segment == L"CON" || segment == L"PRN" || segment == L"AUX" || segment == L"NUL") return true;
    return segment.size() == 4 && (segment.compare(0, 3, L"COM") == 0 || segment.compare(0, 3, L"LPT") == 0) &&
           segment[3] >= L'1' && segment[3] <= L'9';
}
const char kBadName[] = "專案名稱只能用文字、數字、空格、- 與 _，最多 40 字";
}  // namespace

// ------------------------------------------------------------------ ProjectStore
ProjectStore::ProjectStore(const std::wstring& dataDir)
    : root_(dataDir + L"\\projects"), checkpoints_(dataDir + L"\\checkpoints") {
    makeDirs(root_);
    makeDirs(checkpoints_);
}

bool ProjectStore::validName(const std::wstring& name) {
    if (name.empty() || name.size() > 40 || name.front() == L'.' || name.front() == L' ' || name.back() == L' ')
        return false;
    for (wchar_t c : name) {
        WORD type = 0;
        GetStringTypeW(CT_CTYPE1, &c, 1, &type);
        if (!((type & (C1_ALPHA | C1_DIGIT)) || c == L' ' || c == L'-' || c == L'_')) return false;
    }
    return !reservedDeviceName(name);
}

bool ProjectStore::projectDir(const std::string& name, std::wstring& dir, std::string& error) const {
    const auto w = W(name);
    if (!validName(w)) {
        error = kBadName;
        return false;
    }
    dir = root_ + L"\\" + w;
    return true;
}

bool ProjectStore::resolve(const std::string& name, const std::string& relative, std::wstring& target,
                           std::string& error) const {
    const auto rel = W(relative);
    if (rel.empty() || rel.size() > 240) {
        error = "檔案路徑不正確";
        return false;
    }
    if (rel.front() == L'/' || rel.find(L'\\') != std::wstring::npos || rel.find(L':') != std::wstring::npos ||
        rel.find(L'\0') != std::wstring::npos) {
        error = "只能使用專案內的相對路徑";
        return false;
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const auto slash = rel.find(L'/', start);
        parts.push_back(rel.substr(start, slash == std::wstring::npos ? slash : slash - start));
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    for (const auto& part : parts) {
        if (part.empty() || part == L"." || part == L"..") {
            error = "只能使用專案內的相對路徑";
            return false;
        }
        if (part.find_first_of(L"<>\"|?*") != std::wstring::npos ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32; }) || part.back() == L'.' ||
            part.back() == L' ' || reservedDeviceName(part)) {
            error = "檔案路徑不正確";
            return false;
        }
    }
    std::wstring lowered = parts[0];
    for (auto& c : lowered) c = (wchar_t)towlower(c);
    if (lowered == L".haiku") {
        error = "這是 Haiku 的內部資料夾";
        return false;
    }
    std::wstring base;
    if (!projectDir(name, base, error)) return false;
    target = base;
    for (const auto& part : parts) {
        target += L"\\" + part;
        // A link could point outside the project; the app never creates one, so any found is refused.
        if (isReparse(target)) {
            error = "不支援連結";
            return false;
        }
    }
    return true;
}

bool ProjectStore::files(const std::string& name, std::vector<FileInfo>& out, std::string& error) const {
    out.clear();
    std::wstring base;
    if (!projectDir(name, base, error)) return false;
    if (!isDirectory(base)) {
        error = "找不到專案";
        return false;
    }
    struct Walk {
        std::vector<FileInfo>& out;
        void go(const std::wstring& dir, const std::wstring& rel, int depth) {
            if (depth > 16) return;
            for (const auto& e : listFolder(dir)) {
                if (out.size() > kMaxFiles * 2) return;
                const std::wstring p = dir + L"\\" + e.name, r = rel.empty() ? e.name : rel + L"/" + e.name;
                if (e.dir) {
                    std::wstring lowered = e.name;
                    for (auto& c : lowered) c = (wchar_t)towlower(c);
                    if (e.reparse || (rel.empty() && lowered == L".haiku")) continue;
                    go(p, r, depth + 1);
                } else if (!e.reparse) {
                    out.push_back({U(r), sizeOf(p)});
                }
            }
        }
    } walk{out};
    walk.go(base, L"", 0);
    std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) { return a.path < b.path; });
    return true;
}

bool ProjectStore::list(std::vector<ProjectInfo>& out, std::string& error) const {
    out.clear();
    for (const auto& e : listFolder(root_)) {
        if (!e.dir || e.reparse || !validName(e.name)) continue;
        std::vector<FileInfo> files;
        if (!this->files(U(e.name), files, error)) return false;
        const std::wstring dir = root_ + L"\\" + e.name;
        long long updated = modified(dir);
        for (const auto& f : files)
            updated = std::max(updated, modified(dir + L"\\" + [&] {
                                   std::wstring w = W(f.path);
                                   std::replace(w.begin(), w.end(), L'/', L'\\');
                                   return w;
                               }()));
        out.push_back({U(e.name), (int)files.size(), updated});
    }
    std::sort(out.begin(), out.end(), [](const ProjectInfo& a, const ProjectInfo& b) { return a.updated > b.updated; });
    return true;
}

bool ProjectStore::create(const std::string& name, std::string& error) {
    std::wstring dir;
    if (!projectDir(name, dir, error)) return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = "已經有同名的專案";
        return false;
    }
    if (!makeDirs(dir)) {
        error = "無法建立專案資料夾";
        return false;
    }
    return write(name, "index.html",
                 "<!doctype html>\n<html lang=\"zh-Hant-TW\">\n<head>\n  <meta charset=\"utf-8\">\n"
                 "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>" +
                     name +
                     "</title>\n  <link rel=\"stylesheet\" href=\"style.css\">\n</head>\n<body>\n  <main>\n    <h1>" +
                     name +
                     "</h1>\n    <p id=\"status\">從對話告訴 Haiku 你想做什麼。</p>\n  </main>\n  <script "
                     "src=\"app.js\"></script>\n</body>\n</html>\n",
                 error) &&
           write(name, "style.css",
                 "body { margin: 0; font-family: system-ui, sans-serif; line-height: 1.6; }\n"
                 "main { max-width: 640px; margin: 0 auto; padding: 24px 20px; }\n",
                 error) &&
           write(name, "app.js", "// 頁面載入後執行。\ndocument.getElementById('status').dataset.ready = 'true';\n",
                 error);
}

bool ProjectStore::remove(const std::string& name, std::string& error) {
    std::wstring dir, saved;
    if (!projectDir(name, dir, error)) return false;
    if (!isDirectory(dir)) {
        error = "找不到專案";
        return false;
    }
    deleteTree(dir);
    if (checkpointsOf(name, saved, error)) deleteTree(saved);
    return true;
}

bool ProjectStore::readBytes(const std::string& name, const std::string& path, std::string& data,
                             std::string& error) const {
    std::wstring f;
    if (!resolve(name, path, f, error)) return false;
    if (!isRegularFile(f)) {
        error = "找不到檔案 " + path;
        return false;
    }
    if (sizeOf(f) > (long long)kMaxFileBytes) {
        error = "檔案超過 2 MiB";
        return false;
    }
    if (!util::readFile(f, data)) {
        error = "無法讀取檔案 " + path;
        return false;
    }
    return true;
}

namespace {
// Text goes through JSON, so bytes that are not UTF-8 become U+FFFD, as they do on the phone.
std::string asUtf8Text(const std::string& bytes) {
    if (bytes.empty()) return bytes;
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return std::string();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), n);
    return U(w);
}
}  // namespace

bool ProjectStore::read(const std::string& name, const std::string& path, std::string& text, std::string& error) const {
    std::string data;
    if (!readBytes(name, path, data, error)) return false;
    text = asUtf8Text(data);
    return true;
}

bool ProjectStore::write(const std::string& name, const std::string& path, const std::string& text,
                         std::string& error) {
    if (text.size() > kMaxFileBytes) {
        error = "檔案超過 2 MiB";
        return false;
    }
    std::wstring f;
    if (!resolve(name, path, f, error)) return false;
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::vector<FileInfo> existing;
        if (!files(name, existing, error)) return false;
        if (existing.size() >= kMaxFiles) {
            error = "每個專案最多 " + std::to_string(kMaxFiles) + " 個檔案";
            return false;
        }
    }
    if (!makeDirs(f.substr(0, f.find_last_of(L'\\'))) || !util::writeFileAtomic(f, text)) {
        error = "無法寫入檔案 " + path;
        return false;
    }
    return true;
}

bool ProjectStore::removeFile(const std::string& name, const std::string& path, std::string& error) {
    std::wstring f;
    if (!resolve(name, path, f, error)) return false;
    if (!isRegularFile(f)) {
        error = "找不到檔案 " + path;
        return false;
    }
    if (!DeleteFileW(f.c_str())) {
        error = "無法刪除檔案 " + path;
        return false;
    }
    return true;
}

namespace {
bool internalName(const std::string& file) {
    if (file.size() < 6 || file.size() > 25 || !util::endsWith(file, ".json")) return false;
    const auto stem = file.substr(0, file.size() - 5);
    return std::all_of(stem.begin(), stem.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}
}  // namespace

bool ProjectStore::readInternal(const std::string& name, const std::string& file, std::string& text,
                                std::string& error) const {
    std::wstring dir;
    if (!internalName(file)) {
        error = "內部檔名不正確";
        return false;
    }
    if (!projectDir(name, dir, error)) return false;
    std::string data;
    text = util::readFile(dir + L"\\.haiku\\" + W(file), data) ? asUtf8Text(data) : std::string();
    return true;
}

bool ProjectStore::writeInternal(const std::string& name, const std::string& file, const std::string& text,
                                 std::string& error) {
    std::wstring dir;
    if (!internalName(file)) {
        error = "內部檔名不正確";
        return false;
    }
    if (!projectDir(name, dir, error)) return false;
    if (isReparse(dir + L"\\.haiku") || !makeDirs(dir + L"\\.haiku") ||
        !util::writeFileAtomic(dir + L"\\.haiku\\" + W(file), text)) {
        error = "無法保存對話";
        return false;
    }
    return true;
}

bool ProjectStore::checkpointsOf(const std::string& name, std::wstring& dir, std::string& error) const {
    std::wstring project;
    if (!projectDir(name, project, error)) return false;
    // Folder names stay ASCII so any project name maps to a safe directory.
    static const char* hex = "0123456789abcdef";
    std::string h;
    for (unsigned char b : name) {
        h.push_back(hex[b >> 4]);
        h.push_back(hex[b & 15]);
    }
    dir = checkpoints_ + L"\\" + W(h);
    return true;
}

bool ProjectStore::checkpoints(const std::string& name, std::vector<std::string>& ids, std::string& error) const {
    ids.clear();
    std::wstring dir;
    if (!checkpointsOf(name, dir, error)) return false;
    for (const auto& e : listFolder(dir)) {
        const auto id = U(e.name);
        if (e.dir && !id.empty() && id.size() <= 19 &&
            std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
            ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return true;
}

namespace {
std::wstring backslashed(const std::string& rel) {
    std::wstring w = W(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}
void removeEmptyFolders(const std::wstring& dir, bool top) {
    for (const auto& e : listFolder(dir)) {
        if (!e.dir || e.reparse) continue;
        std::wstring lowered = e.name;
        for (auto& c : lowered) c = (wchar_t)towlower(c);
        if (top && lowered == L".haiku") continue;
        const std::wstring p = dir + L"\\" + e.name;
        removeEmptyFolders(p, false);
        RemoveDirectoryW(p.c_str());  // fails harmlessly when something is left in it
    }
}
}  // namespace

bool ProjectStore::checkpoint(const std::string& name, std::string& id, std::string& error) {
    std::wstring base, dir;
    if (!projectDir(name, base, error) || !checkpointsOf(name, dir, error)) return false;
    long long stamp = util::nowMs();
    while (GetFileAttributesW((dir + L"\\" + std::to_wstring(stamp)).c_str()) != INVALID_FILE_ATTRIBUTES) ++stamp;
    id = std::to_string(stamp);
    const std::wstring saved = dir + L"\\" + std::to_wstring(stamp);
    std::vector<FileInfo> current;
    if (!makeDirs(saved) || !files(name, current, error)) {
        if (error.empty()) error = "無法建立還原點";
        return false;
    }
    for (const auto& f : current) {
        const std::wstring target = saved + L"\\" + backslashed(f.path);
        if (!makeDirs(target.substr(0, target.find_last_of(L'\\'))) ||
            !CopyFileW((base + L"\\" + backslashed(f.path)).c_str(), target.c_str(), TRUE)) {
            error = "無法建立還原點";
            return false;
        }
    }
    std::vector<std::string> all;
    if (checkpoints(name, all, error))
        for (size_t i = 0; i + kMaxCheckpoints < all.size(); ++i) deleteTree(dir + L"\\" + W(all[i]));
    return true;
}

bool ProjectStore::restore(const std::string& name, const std::string& id, std::string& error) {
    std::wstring base, dir;
    if (!projectDir(name, base, error) || !checkpointsOf(name, dir, error)) return false;
    if (id.empty() || id.size() > 19 ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        error = "找不到這個還原點";
        return false;
    }
    const std::wstring saved = dir + L"\\" + W(id);
    if (!isDirectory(saved)) {
        error = "找不到這個還原點";
        return false;
    }
    std::vector<FileInfo> current;
    if (!files(name, current, error)) return false;
    for (const auto& f : current) DeleteFileW((base + L"\\" + backslashed(f.path)).c_str());
    struct Copy {
        const std::wstring &saved, &base;
        bool ok = true;
        void go(const std::wstring& dir, const std::wstring& rel) {
            for (const auto& e : listFolder(dir)) {
                const std::wstring p = dir + L"\\" + e.name, r = rel.empty() ? e.name : rel + L"\\" + e.name;
                if (e.dir && !e.reparse) go(p, r);
                else if (!e.dir) {
                    const std::wstring target = base + L"\\" + r;
                    ok = ok && makeDirs(target.substr(0, target.find_last_of(L'\\'))) &&
                         CopyFileW(p.c_str(), target.c_str(), FALSE);
                }
            }
        }
    } copy{saved, base};
    copy.go(saved, L"");
    removeEmptyFolders(base, true);
    if (!copy.ok) error = "還原時有檔案無法寫入";
    return copy.ok;
}

bool ProjectStore::exportZip(const std::string& name, const std::wstring& target, std::string& error) const {
    std::wstring base;
    std::vector<FileInfo> current;
    if (!projectDir(name, base, error) || !files(name, current, error)) return false;
    std::vector<ZipEntry> entries;
    for (const auto& f : current) entries.push_back({name + "/" + f.path, base + L"\\" + backslashed(f.path)});
    if (!makeDirs(target.substr(0, target.find_last_of(L'\\')))) {
        error = "無法在下載資料夾建立檔案";
        return false;
    }
    return writeZip(target, entries, error);
}

// ------------------------------------------------------------------ KeyVault
KeyVault::KeyVault(const std::wstring& dataDir) : dir_(dataDir + L"\\keys") {
    makeDirs(dir_);
}
bool KeyVault::has(const std::string& provider) const {
    return isRegularFile(dir_ + L"\\" + W(provider) + L".key");
}
bool KeyVault::put(const std::string& provider, const std::string& key) {
    DATA_BLOB in{(DWORD)key.size(), (BYTE*)key.data()}, entropy{14, (BYTE*)"cca-haiku-sim"}, out{};
    if (!CryptProtectData(&in, L"CCA Haiku", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    const bool ok =
        util::writeFileAtomic(dir_ + L"\\" + W(provider) + L".key", std::string((const char*)out.pbData, out.cbData));
    LocalFree(out.pbData);
    return ok;
}
bool KeyVault::get(const std::string& provider, std::string& key) const {
    std::string blob;
    if (!util::readFile(dir_ + L"\\" + W(provider) + L".key", blob) || blob.empty()) return false;
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()}, entropy{14, (BYTE*)"cca-haiku-sim"}, out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    key.assign((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
void KeyVault::clear(const std::string& provider) {
    DeleteFileW((dir_ + L"\\" + W(provider) + L".key").c_str());
}

// ------------------------------------------------------------------ ModelClient
ModelClient::ModelClient() {
    anthropic_ = "https://api.anthropic.com/v1";
    openai_ = "https://api.openai.com/v1";
    const auto a = util::narrow(util::getEnv(L"CCA_TEST_HAIKU_ANTHROPIC")),
               o = util::narrow(util::getEnv(L"CCA_TEST_HAIKU_OPENAI"));
    if (!a.empty()) anthropic_ = a;
    if (!o.empty()) openai_ = o;
}
bool ModelClient::knownProvider(const std::string& provider) {
    return provider == "anthropic" || provider == "openai";
}
bool ModelClient::models(const std::string& provider, const std::string& key, ModelResponse& out,
                         std::string& error) const {
    return call(provider, key, L"GET", "/models", nullptr, out, error);
}
bool ModelClient::send(const std::string& provider, const std::string& key, const std::string& body, ModelResponse& out,
                       std::string& error) const {
    if (body.empty() || body.size() > kMaxBody) {
        error = "要求太大";
        return false;
    }
    return call(provider, key, L"POST", provider == "anthropic" ? "/messages" : "/chat/completions", &body, out, error);
}
bool ModelClient::call(const std::string& provider, const std::string& key, const wchar_t* method,
                       const std::string& path, const std::string* body, ModelResponse& out, std::string& error) const {
    if (!knownProvider(provider)) {
        error = "不支援的供應商";
        return false;
    }
    if (key.empty()) {
        error = "還沒有設定這個供應商的 API 金鑰";
        return false;
    }
    const std::wstring url = W((provider == "anthropic" ? anthropic_ : openai_) + path);
    URL_COMPONENTS uc{sizeof uc};
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) ||
        (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP)) {
        error = "供應商網址不正確";
        return false;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    auto fail = [&](const char* what) {
        error = std::string(what) + "（錯誤碼 " + std::to_string(GetLastError()) + "）";
        return false;
    };
    HINTERNET session = WinHttpOpen(L"CCA-Haiku-Simulator/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return fail("無法開始網路連線");
    WinHttpSetTimeouts(session, 10000, 20000, 30000, 240000);
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength), target(uc.lpszUrlPath, uc.dwUrlPathLength);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    auto close = [&] {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    };
    if (!request) {
        const bool r = fail("無法連線到供應商");
        close();
        return r;
    }
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    std::wstring headers = L"content-type: application/json\r\n";
    if (provider == "anthropic") headers += L"x-api-key: " + W(key) + L"\r\nanthropic-version: 2023-06-01\r\n";
    else headers += L"Authorization: Bearer " + W(key) + L"\r\n";
    const DWORD length = body ? (DWORD)body->size() : 0;
    if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
                            length, length, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        const bool r = fail("無法連線到供應商");
        close();
        return r;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                        nullptr);
    std::string text;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (text.size() + available > kMaxBody) {
            error = "回應超過 8 MiB";
            close();
            return false;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        text.append(chunk, 0, read);
    }
    close();
    // A provider error may echo the request headers; the key never goes back to the page.
    out.status = (int)status;
    out.body = util::replaceAll(asUtf8Text(text), key, "[已隱藏]");
    return true;
}

// ------------------------------------------------------------------ Host
std::string contentType(const std::string& path) {
    static const std::map<std::string, std::string> types = {
        {"html", "text/html"},      {"htm", "text/html"},         {"css", "text/css"},      {"js", "text/javascript"},
        {"mjs", "text/javascript"}, {"json", "application/json"}, {"svg", "image/svg+xml"}, {"png", "image/png"},
        {"jpg", "image/jpeg"},      {"jpeg", "image/jpeg"},       {"gif", "image/gif"},     {"webp", "image/webp"},
        {"ico", "image/x-icon"},    {"txt", "text/plain"},        {"md", "text/plain"},     {"woff2", "font/woff2"},
        {"mp3", "audio/mpeg"},      {"wav", "audio/wav"}};
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const auto found = types.find(util::toLower(path.substr(dot + 1)));
    return found == types.end() ? "application/octet-stream" : found->second;
}

std::wstring defaultExportDir() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) out = path;
    CoTaskMemFree(path);
    if (out.empty()) out = util::joinPath(util::homeDir(), L"Downloads");
    return out + L"\\CCA Haiku";
}

Host::Host(const std::wstring& dataDir, const std::wstring& exportDir)
    : projects_(dataDir), vault_(dataDir), exportDir_(exportDir) {}

Reply Host::handle(const std::string& method, const Json& p) {
    Reply r;
    std::string error;
    auto fail = [&](const std::string& why) {
        r.ok = false;
        r.error = why.empty() ? "操作失敗" : why;
        return r;
    };
    auto done = [&](Json value) {
        r.ok = true;
        r.value = std::move(value);
        return r;
    };
    // getString() on the phone throws when a parameter is missing.
    auto need = [&](const char* key, std::string& into) {
        if (!p[key].isString()) {
            error = std::string("缺少參數 ") + key;
            return false;
        }
        into = p[key].str();
        return true;
    };
    const std::string project = p["project"].str();
    std::string a, b;
    if (method == "projects.list") {
        std::vector<ProjectInfo> list;
        if (!projects_.list(list, error)) return fail(error);
        Json out = Json::array();
        for (const auto& info : list)
            out.push(J({{"name", info.name}, {"files", info.files}, {"updated", info.updated}}));
        return done(out);
    }
    if (method == "projects.create") return need("name", a) && projects_.create(a, error) ? done(true) : fail(error);
    if (method == "projects.delete") return projects_.remove(project, error) ? done(true) : fail(error);
    if (method == "files.list") {
        std::vector<FileInfo> list;
        if (!projects_.files(project, list, error)) return fail(error);
        Json out = Json::array();
        for (const auto& f : list) out.push(J({{"path", f.path}, {"size", f.size}}));
        return done(out);
    }
    if (method == "files.read") return need("path", a) && projects_.read(project, a, b, error) ? done(b) : fail(error);
    if (method == "files.write") {
        std::string text;
        return need("path", a) && need("text", text) && projects_.write(project, a, text, error) ? done(true)
                                                                                                 : fail(error);
    }
    if (method == "files.delete")
        return need("path", a) && projects_.removeFile(project, a, error) ? done(true) : fail(error);
    if (method == "chat.read") return projects_.readInternal(project, "chat.json", a, error) ? done(a) : fail(error);
    if (method == "chat.write")
        return need("text", a) && projects_.writeInternal(project, "chat.json", a, error) ? done(true) : fail(error);
    if (method == "checkpoint.take") return projects_.checkpoint(project, a, error) ? done(a) : fail(error);
    if (method == "checkpoint.restore")
        return need("checkpoint", a) && projects_.restore(project, a, error) ? done(true) : fail(error);
    if (method == "keys.status")
        return done(J({{"anthropic", vault_.has("anthropic")}, {"openai", vault_.has("openai")}}));
    if (method == "keys.set") {
        std::string key;
        if (!need("provider", a) || !need("key", key)) return fail(error);
        key = util::trim(key);
        if (!ModelClient::knownProvider(a) || key.size() < 8 || key.size() > 400 ||
            std::any_of(key.begin(), key.end(), [](unsigned char c) { return c < 33 || c == 127; }))
            return fail("金鑰格式不正確");
        return vault_.put(a, key) ? done(true) : fail("無法保存金鑰");
    }
    if (method == "keys.clear") {
        if (!need("provider", a) || !ModelClient::knownProvider(a))
            return fail(error.empty() ? "不支援的供應商" : error);
        vault_.clear(a);
        return done(true);
    }
    if (method == "models.list" || method == "model.send") {
        std::string key, body;
        ModelResponse response;
        if (!need("provider", a)) return fail(error);
        if (!ModelClient::knownProvider(a)) return fail("不支援的供應商");
        vault_.get(a, key);
        const bool ok = method == "models.list" ? client_.models(a, key, response, error)
                                                : need("body", body) && client_.send(a, key, body, response, error);
        return ok ? done(J({{"status", response.status}, {"body", response.body}})) : fail(error);
    }
    if (method == "preview.open") {
        std::vector<FileInfo> list;
        return projects_.files(project, list, error) ? done(true) : fail(error);  // fails for a missing project
    }
    if (method == "export.zip") {
        const std::wstring file = exportDir_ + L"\\" + W(project) + L"-" + std::to_wstring(util::nowMs()) + L".zip";
        if (!ProjectStore::validName(W(project))) return fail(kBadName);
        return projects_.exportZip(project, file, error) ? done(U(file)) : fail(error);
    }
    return fail("不支援的操作 " + method);
}

std::string Host::replyScript(const std::string& id, const Reply& reply) {
    Json message = Json::object();
    message["id"] = id;
    message["ok"] = reply.ok;
    if (reply.ok) message["result"] = reply.value;
    else message["error"] = reply.error;
    // U+2028/2029 are valid in JSON but end a JavaScript string literal.
    std::string literal = Json(message.dump()).dump();
    literal = util::replaceAll(literal, "\xE2\x80\xA8", "\\u2028");
    literal = util::replaceAll(literal, "\xE2\x80\xA9", "\\u2029");
    return "window.Haiku.receive(" + literal + ")";
}

}  // namespace haikusim
