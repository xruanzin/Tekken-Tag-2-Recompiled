#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance; (void)previous; (void)show;

    wchar_t directory[32768]{};
    wchar_t launcher[32768]{};
    wchar_t command[32768]{};

    const DWORD length = GetModuleFileNameW(nullptr, directory, static_cast<DWORD>(std::size(directory)));
    if (length == 0 || length >= std::size(directory)) return 1;

    wchar_t* slash = wcsrchr(directory, L'\\');
    if (slash) *slash = 0;

    swprintf_s(launcher, L"%ls\\Tekken Tag Tournament 2.exe", directory);
    swprintf_s(command, L"\"%ls\" --play%ls%ls", launcher,
               arguments && *arguments ? L" " : L"",
               arguments ? arguments : L"");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(launcher, command, nullptr, nullptr, FALSE, 0, nullptr,
                        directory, &startup, &process)) {
        MessageBoxW(nullptr,
                    L"Tekken Tag Tournament 2.exe was not found next to this file.",
                    L"Tekken Tag Tournament 2",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
