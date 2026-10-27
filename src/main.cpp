#include "common.h"
#include "config.h"
#include "launcher.h"
#include "version.h"

#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace {

void PrintToParentConsole(const std::wstring& text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    std::wstring line = text + L"\n";
    WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &written, nullptr);
    FreeConsole();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring configPath = oa::DefaultConfigPath();
    bool show = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"quit" || a == L"--quit") {
            if (HWND running = FindWindowW(oa::kWindowClass, nullptr)) PostMessageW(running, WM_CLOSE, 0, 0);
            return 0;
        }
        if (a == L"reload") {
            if (HWND running = FindWindowW(oa::kWindowClass, nullptr)) PostMessageW(running, WM_APP + 5, 0, 0);
            return 0;
        }
        if (a == L"show" || a == L"toggle") {
            show = true;
        } else if ((a == L"--config" || a == L"-c") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (a == L"--version" || a == L"-v") {
            PrintToParentConsole(L"ObjectArc " OA_VERSION_WSTRING);
            return 0;
        } else if (a == L"--help" || a == L"-h") {
            PrintToParentConsole(L"ObjectArc " OA_VERSION_WSTRING L" - keyboard launcher\n"
                                 L"  ObjectArc [--config <path>]  start in the background (opened with the hotkey)\n"
                                 L"  ObjectArc show               open the running launcher\n"
                                 L"  ObjectArc quit               close the running launcher\n"
                                 L"  ObjectArc reload             reload arc.conf in the running launcher\n"
                                 L"  ObjectArc --version          print the version");
            return 0;
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ObjectArc");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: `ObjectArc show` (or starting it again) opens the launcher there.
        if (HWND running = FindWindowW(oa::kWindowClass, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(running, WM_HOTKEY, 1, 0);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    const bool firstRun = !std::filesystem::exists(configPath, ec);
    oa::EnsureConfigFile(configPath);

    auto& launcher = oa::Launcher::Instance();
    if (!launcher.Init(instance, configPath, firstRun)) {
        MessageBoxW(nullptr, L"Failed to initialize ObjectArc.", L"ObjectArc", MB_ICONERROR);
        return 1;
    }
    if (show) {
        if (HWND self = FindWindowW(oa::kWindowClass, nullptr)) PostMessageW(self, WM_HOTKEY, 1, 0);
    }
    int code = launcher.Run();

    LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return code;
}
