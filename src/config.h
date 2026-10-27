#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace oa {

struct Hotkey {
    UINT modifiers = MOD_ALT;
    UINT key = VK_SPACE;
};

struct Config {
    // Window
    Hotkey hotkey;
    bool winKey = true;  // pressing and releasing Win alone also opens it, instead of Start
    int rows = 8;          // visible result rows
    bool compact = false;  // only the search bar until something is typed
    int scale = 100;
    bool keepQuery = false;
    bool trayIcon = true;

    // Results
    bool showWindows = true;
    bool frameCommands = true;
    std::vector<std::wstring> folders;  // extra folders whose programs and shortcuts are indexed
    std::wstring searchName = L"Google";
    std::wstring searchUrl = L"https://www.google.com/search?q=%s";

    // Look
    bool accentFromSystem = true;
    COLORREF accent = RGB(0x7a, 0xa2, 0xf7);
};

std::wstring DefaultConfigPath();
bool EnsureConfigFile(const std::wstring& path);
Config LoadConfig(const std::wstring& path, std::vector<std::wstring>& warnings);
bool ParseHotkey(const std::wstring& text, Hotkey& out);
std::wstring HotkeyLabel(const Hotkey& hotkey);
std::wstring OpenKeysLabel(const Config& config);  // "Win / Alt+Space"

}  // namespace oa
