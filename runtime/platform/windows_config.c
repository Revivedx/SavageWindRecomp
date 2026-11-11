// "Savage Wind Configuration.exe" (Windows; build/savagewind-config.exe in a build): starts savagewind.exe from its own
// folder with --config, the configuration screen (runtime/ui/launcher.mm: Play, Settings, the game files, Quit), and
// passes its own arguments on. A plain start of savagewind.exe goes straight into the game once the game files are set
// up. This is its own small program, not savagewind.exe under a second name: the game code library imports from
// savagewind.exe by that name. A GUI program with the same icon (runtime/platform/windows.rc), no console.
#include <wchar.h>
#include <windows.h>

// The command line after the program's own name (as CommandLineToArgvW splits it: quoted, or up to a space)
static const wchar_t *args_after_name(const wchar_t *c) {
    if (*c == L'"') {
        for (c++; *c && *c != L'"'; c++) {}
        if (*c) c++;
    } else
        while (*c && *c != L' ' && *c != L'\t') c++;
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}

int main(void) {
    wchar_t exe[4096];
    DWORD n = GetModuleFileNameW(NULL, exe, sizeof exe / sizeof *exe);
    wchar_t *slash = n && n < sizeof exe / sizeof *exe ? wcsrchr(exe, L'\\') : NULL;
    if (!slash) return 1;
    // An update cut off halfway (a power cut, runtime/platform/windows_update.c): put the files back before anything loads them
    wcscpy(slash + 1, L"update\\journal.txt");
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
        wcscpy(slash + 1, L"SW Update.exe");
        wchar_t recover[4200];
        swprintf(recover, sizeof recover / sizeof *recover, L"\"%ls\" --recover", exe);
        STARTUPINFOW rsi = {sizeof rsi};
        PROCESS_INFORMATION rpi;
        if (CreateProcessW(exe, recover, NULL, NULL, FALSE, 0, NULL, NULL, &rsi, &rpi)) {
            WaitForSingleObject(rpi.hProcess, INFINITE);
            CloseHandle(rpi.hThread);
            CloseHandle(rpi.hProcess);
        }
    }
    wcscpy(slash + 1, L"savagewind.exe");
    const wchar_t *rest = args_after_name(GetCommandLineW());
    size_t cap = wcslen(exe) + wcslen(rest) + 32;
    wchar_t *line = HeapAlloc(GetProcessHeap(), 0, cap * sizeof *line);
    if (!line) return 1;
    swprintf(line, cap, L"\"%ls\" --config%ls%ls", exe, *rest ? L" " : L"", rest);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    AllowSetForegroundWindow(ASFW_ANY);  // (its window comes up in front, as this one would have)
    // (the working folder stays this one's: savagewind.exe finds its data folder from it, runtime/paths.c)
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        wchar_t msg[4400];
        swprintf(msg, sizeof msg / sizeof *msg, L"Savage Wind Configuration can't start\n%ls\n\nIt has to be in the same "
                 L"folder as savagewind.exe.", exe);
        MessageBoxW(NULL, msg, L"Savage Wind", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
