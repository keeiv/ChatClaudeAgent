#include "term.hpp"

#include "util.hpp"

namespace term {

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;
DWORD g_inMode = 0, g_outMode = 0;
UINT g_inCp = 0, g_outCp = 0;
bool g_ready = false;
wchar_t g_highSurrogate = 0;

bool isKeyRecord(const INPUT_RECORD& r) { return r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown; }

}  // namespace

bool init(std::string* error) {
    // The console itself rather than stdin/stdout, so a redirected stream does not matter
    // (the same way full-screen programs such as vim behave).
    g_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE || !GetConsoleMode(g_in, &g_inMode) ||
        !GetConsoleMode(g_out, &g_outMode)) {
        if (error) *error = "ChatClaudeAgent CLI 需要在終端機（例如 Windows Terminal）裡執行。";
        return false;
    }
    g_inCp = GetConsoleCP();
    g_outCp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    if (!SetConsoleMode(g_out, g_outMode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (error) *error = "這個終端機不支援 VT 色碼，請改用 Windows Terminal。";
        return false;
    }
    // Raw keys: no line editing, no echo, and Ctrl+C arrives as a key instead of a signal.
    // Quick-edit selection keeps working because ENABLE_EXTENDED_FLAGS keeps its bit as is.
    DWORD in = (g_inMode & ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    SetConsoleMode(g_in, in);
    g_ready = true;
    return true;
}

void restore() {
    if (!g_ready) return;
    write("\x1b[0m\x1b[?25h");
    SetConsoleMode(g_in, g_inMode);
    SetConsoleMode(g_out, g_outMode);
    SetConsoleCP(g_inCp);
    SetConsoleOutputCP(g_outCp);
    g_ready = false;
}

int cols() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return 80;
    return info.srWindow.Right - info.srWindow.Left + 1;
}

int rows() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return 24;
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

HANDLE input() { return g_in; }

void write(const std::string& utf8) {
    std::wstring w = util::widen(utf8);
    const wchar_t* p = w.data();
    size_t left = w.size();
    while (left > 0) {
        DWORD n = 0;
        DWORD chunk = (DWORD)(left > 16384 ? 16384 : left);
        if (!WriteConsoleW(g_out, p, chunk, &n, nullptr) || n == 0) break;
        p += n;
        left -= n;
    }
}

std::vector<Key> readKeys() {
    std::vector<Key> keys;
    DWORD avail = 0;
    if (!GetNumberOfConsoleInputEvents(g_in, &avail) || avail == 0) return keys;
    std::vector<INPUT_RECORD> recs(avail);
    DWORD got = 0;
    if (!ReadConsoleInputW(g_in, recs.data(), avail, &got)) return keys;
    recs.resize(got);
    for (size_t i = 0; i < recs.size(); ++i) {
        const INPUT_RECORD& r = recs[i];
        if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            Key k;
            k.code = Key::Resize;
            keys.push_back(k);
            continue;
        }
        if (!isKeyRecord(r)) continue;
        const KEY_EVENT_RECORD& e = r.Event.KeyEvent;
        Key k;
        DWORD st = e.dwControlKeyState;
        k.shift = (st & SHIFT_PRESSED) != 0;
        k.ctrl = (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        k.alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        // AltGr shows up as Ctrl+Alt with a printable character: that is typing, not a shortcut.
        bool altGr = k.ctrl && k.alt && e.uChar.UnicodeChar >= 0x20;
        if (altGr) k.ctrl = k.alt = false;
        wchar_t uc = e.uChar.UnicodeChar;
        switch (e.wVirtualKeyCode) {
        case VK_RETURN: {
            k.code = Key::Enter;
            // More typed text right behind an Enter in the same read is a paste.
            for (size_t j = i + 1; j < recs.size(); ++j) {
                if (isKeyRecord(recs[j]) && recs[j].Event.KeyEvent.uChar.UnicodeChar >= 0x20) { k.pasted = true; break; }
                if (isKeyRecord(recs[j])) break;
            }
            break;
        }
        case VK_BACK: k.code = Key::Backspace; break;
        case VK_DELETE: k.code = Key::Delete; break;
        case VK_LEFT: k.code = Key::Left; break;
        case VK_RIGHT: k.code = Key::Right; break;
        case VK_UP: k.code = Key::Up; break;
        case VK_DOWN: k.code = Key::Down; break;
        case VK_HOME: k.code = Key::Home; break;
        case VK_END: k.code = Key::End; break;
        case VK_PRIOR: k.code = Key::PageUp; break;
        case VK_NEXT: k.code = Key::PageDown; break;
        case VK_TAB: k.code = Key::Tab; break;
        case VK_ESCAPE: k.code = Key::Esc; break;
        default:
            if (k.ctrl && !k.alt && e.wVirtualKeyCode >= 'A' && e.wVirtualKeyCode <= 'Z') {
                k.code = Key::Char;
                k.ch = (char32_t)(e.wVirtualKeyCode - 'A' + 'a');
                break;
            }
            if (uc == 0) continue;  // a lone modifier
            if (uc >= 0xD800 && uc <= 0xDBFF) {
                g_highSurrogate = uc;
                continue;
            }
            k.code = Key::Char;
            if (uc >= 0xDC00 && uc <= 0xDFFF) {
                if (!g_highSurrogate) continue;
                k.ch = 0x10000 + (((char32_t)g_highSurrogate - 0xD800) << 10) + ((char32_t)uc - 0xDC00);
                g_highSurrogate = 0;
            } else {
                if (uc < 0x20) continue;  // other control characters
                k.ch = uc;
            }
            k.ctrl = k.alt = false;
            break;
        }
        int repeat = e.wRepeatCount > 0 ? e.wRepeatCount : 1;
        for (int n = 0; n < repeat; ++n) keys.push_back(k);
    }
    return keys;
}

}  // namespace term
