// The Windows console for the CLI: UTF-8 VT output, raw key input, and the size.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace term {

struct Key {
    enum Code { Char, Enter, Backspace, Delete, Left, Right, Up, Down, Home, End, PageUp, PageDown, Tab, Esc, Resize };
    Code code = Char;
    char32_t ch = 0;     // for Char; with ctrl it is the lower-case letter
    bool shift = false, ctrl = false, alt = false;
    bool pasted = false;  // an Enter that arrived in the middle of pasted text
};

// Opens the attached console with UTF-8, VT output and raw input. Redirected
// stdin/stdout are left alone; this fails when there is no attached console.
bool init(std::string* error);
void restore();

int cols();
int rows();
HANDLE input();
void write(const std::string& utf8);

// Everything waiting in the input buffer, as keys. Resizes come through as Key::Resize.
std::vector<Key> readKeys();

}  // namespace term
