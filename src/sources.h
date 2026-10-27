#pragma once

#include "config.h"
#include "icons.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace oa {

enum class Kind { App, File, Window, Command, Calc, Url, Path, Search, Shell };

// Glyphs from Segoe Fluent Icons / Segoe MDL2 Assets, for results without an icon of their own.
namespace glyph {
constexpr wchar_t kCalculator = 0xE8EF, kGlobe = 0xE774, kFolder = 0xE8B7, kTerminal = 0xE756, kLock = 0xE72E,
                  kPower = 0xE7E8, kRestart = 0xE777, kSleep = 0xE708, kSignOut = 0xF3B1, kSettings = 0xE713,
                  kDelete = 0xE74D, kTiles = 0xECA5, kPause = 0xE769, kFullscreen = 0xE740, kRefresh = 0xE72C,
                  kCancel = 0xE711, kSearch = 0xE721, kLink = 0xE71B, kEdit = 0xE70F, kTaskManager = 0xE9D9,
                  kWindow = 0xE737, kApp = 0xE71D, kPin = 0xE718;
}

struct Item {
    Kind kind = Kind::App;
    std::wstring title;
    std::wstring subtitle;
    std::wstring key;       // remembered in the launch history; empty = not remembered
    std::wstring target;    // app id, path, URL, command, or text to copy, depending on `kind`
    std::wstring path;      // a file behind an app, for "open file location" and "run as administrator"
    std::wstring keywords;  // normalized extra text to match, such as the exe name
    std::wstring match;     // normalized title
    HWND hwnd = nullptr;
    Icon icon;
    wchar_t glyph = 0;
    bool confirm = false;  // asks before running (shut down, restart, ...)
    int score = 0;
};

// Installed apps (the shell's Apps folder, the same list as Start) plus the configured folders.
// Built off the UI thread; `cancel` stops it early.
std::vector<Item> BuildIndex(const std::vector<std::wstring>& folders, int iconPx, const std::atomic<bool>& cancel);

// Top-level windows the taskbar would show, except `self`.
std::vector<Item> ListWindows(HWND self, const std::vector<Item>& apps, int iconPx);

std::vector<Item> BuiltinCommands(bool frameCommands);

class History {
public:
    void Load(const std::wstring& path);
    void Record(const std::wstring& key);
    int Boost(const std::wstring& key) const;
    // Most recently used first.
    std::vector<std::wstring> Recent(size_t count) const;

private:
    struct Entry {
        int count = 0;
        long long last = 0;  // seconds since the epoch
    };
    void Save() const;

    std::wstring path_;
    std::map<std::wstring, Entry> entries_;
};

}  // namespace oa
