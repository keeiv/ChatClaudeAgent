// A minimal replacement executable to acknowledge startup without loading user data or models.
#include <windows.h>

#include <string>

int wmain(int argc, wchar_t** argv) {
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(L"CCA_UPDATE_PROBE_FILE", path, 32768);
    if (length && length < 32768) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, "new-started", 11, &written, nullptr);
            CloseHandle(file);
        }
    }
    if (GetEnvironmentVariableW(L"CCA_UPDATE_FAIL_START", nullptr, 0)) return 1;
    if (argc == 3 && std::wstring(argv[1]) == L"--cca-update-started") {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (ready) {
            SetEvent(ready);
            CloseHandle(ready);
        }
    }
    return 0;
}
