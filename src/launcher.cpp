#include "launcher.h"

#include "match.h"
#include "resource.h"
#include "version.h"

#include <dwmapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>

namespace oa {

namespace {

constexpr UINT WM_APP_INDEXED = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_WARN = WM_APP + 3;
constexpr UINT WM_APP_WINKEY = WM_APP + 4;
constexpr UINT WM_APP_RELOAD = WM_APP + 5;  // `ObjectArc reload` and ObjectManager
constexpr WORD kMaskKey = 0xE8;  // an unassigned virtual key

// Touched only by the hook thread.
bool g_winDown = false;
bool g_winUsed = false;  // another key or a click happened while Win was held
HWND g_hookTarget = nullptr;
constexpr int kHotkeyId = 1;
constexpr UINT_PTR kFrameTimer = 1;
constexpr int kEditId = 100;
constexpr ULONGLONG kReindexMs = 5 * 60 * 1000;
constexpr size_t kMaxResults = 60;

// Layout in DIPs.
constexpr float kWidth = 480.0f;  // about two thirds of the previous 720 DIP window
constexpr float kSearchHeight = 58.0f;
constexpr float kRowHeight = 44.0f;
constexpr float kListPad = 6.0f;
constexpr float kFooterHeight = 38.0f;
constexpr float kIconSize = 28.0f;
constexpr float kEditLeft = 54.0f;
constexpr float kEditFont = 19.0f;

constexpr COLORREF kBackground = RGB(30, 30, 32);
constexpr COLORREF kText = RGB(240, 240, 242);
constexpr COLORREF kSubtext = RGB(150, 150, 156);
constexpr COLORREF kDim = RGB(112, 112, 118);
constexpr COLORREF kWhite = RGB(255, 255, 255);

// Not in older SDK headers.
constexpr DWORD kDwmUseDarkMode = 20, kDwmCornerPreference = 33, kDwmBorderColor = 34;
constexpr int kDwmRound = 2;
constexpr UINT kCueBanner = 0x1501;  // EM_SETCUEBANNER

enum TrayCommand : UINT { kTrayOpen = 1, kTrayConfig, kTrayReload, kTrayQuit };

D2D1_COLOR_F Color(COLORREF c, float a = 1.0f) {
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a};
}

float ListHeight(int rows) { return kListPad * 2 + rows * kRowHeight; }
float TotalHeight(int rows) {
    return rows == 0 ? kSearchHeight : kSearchHeight + 1 + ListHeight(rows) + 1 + kFooterHeight;
}

const wchar_t* KindLabel(Kind k) {
    switch (k) {
        case Kind::App: return L"アプリ";
        case Kind::File: return L"ファイル";
        case Kind::Window: return L"ウィンドウ";
        case Kind::Command: return L"コマンド";
        case Kind::Calc: return L"計算";
        case Kind::Url: return L"URL";
        case Kind::Path: return L"パス";
        case Kind::Search: return L"Web 検索";
        case Kind::Shell: return L"シェル";
    }
    return L"";
}

const wchar_t* ActionLabel(Kind k) {
    switch (k) {
        case Kind::Window: return L"切り替え";
        case Kind::Command:
        case Kind::Shell: return L"実行";
        case Kind::Calc: return L"コピー";
        case Kind::Search: return L"検索";
        default: return L"開く";
    }
}

COLORREF GlyphColor(Kind k, COLORREF accent) {
    switch (k) {
        case Kind::Calc: return RGB(255, 159, 10);
        case Kind::Url:
        case Kind::Search: return RGB(10, 132, 255);
        case Kind::Path: return RGB(230, 180, 40);
        case Kind::Shell: return RGB(84, 84, 90);
        case Kind::Window: return RGB(84, 84, 90);
        default: return accent;
    }
}

std::wstring UrlEncode(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), utf8.data(), n, nullptr, nullptr);
    std::wstring out;
    static const wchar_t* kHex = L"0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (wchar_t)c;
        } else if (c == ' ') {
            out += L'+';
        } else {
            out += L'%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

bool LooksLikeUrl(const std::wstring& q) {
    if (q.empty() || q.find_first_of(L" \t\\") != std::wstring::npos) return false;
    const std::wstring l = ToLower(q);
    if (l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0 || l.rfind(L"www.", 0) == 0) return true;
    if (l.rfind(L"localhost:", 0) == 0) return true;
    const std::wstring host = l.substr(0, l.find_first_of(L"/:?#"));
    const size_t dot = host.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return false;
    static const wchar_t* kTlds[] = {L"com", L"net", L"org", L"io", L"jp", L"dev", L"app", L"co", L"me", L"info",
                                     L"ai",  L"gg",  L"tv",  L"xyz", L"edu", L"gov", L"us", L"uk", L"de", L"so"};
    const std::wstring tld = host.substr(dot + 1);
    return std::any_of(std::begin(kTlds), std::end(kTlds), [&](const wchar_t* t) { return tld == t; });
}

bool LooksLikePath(const std::wstring& q) {
    if (q.size() >= 2 && std::iswalpha(q[0]) && q[1] == L':') return q.size() == 2 || q[2] == L'\\' || q[2] == L'/';
    return q.rfind(L"\\\\", 0) == 0 || q[0] == L'%' || q[0] == L'~';
}

Item PathItem(const std::wstring& path, const std::wstring& title, bool directory) {
    Item p;
    p.kind = Kind::Path;
    p.title = title;
    p.subtitle = title == path ? (directory ? L"フォルダー" : L"ファイル") : path;
    p.target = p.path = path;
    p.glyph = directory ? glyph::kFolder : glyph::kApp;
    p.keywords = directory ? L"\\" : L"";  // marks folders for Tab completion
    return p;
}

// The path itself if it exists, plus the entries of its folder that start with what follows the last '\'.
void AddPathResults(const std::wstring& query, std::vector<Item>& out) {
    std::wstring path = ExpandEnv(query);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() == 2) path += L'\\';
    const DWORD attr = GetFileAttributesW(path.c_str());
    std::wstring dir, prefix;
    if (attr != INVALID_FILE_ATTRIBUTES) {
        const bool directory = attr & FILE_ATTRIBUTE_DIRECTORY;
        Item self = PathItem(path, path, directory);
        self.icon = IconFromPath(path, 48);
        out.push_back(std::move(self));
        if (!directory || path.back() != L'\\') return;
        dir = path;
    } else {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        dir = path.substr(0, slash + 1);
        prefix = ToLower(path.substr(slash + 1));
        const DWORD dirAttr = GetFileAttributesW(dir.c_str());
        if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY)) return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<Item> entries;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!prefix.empty() && ToLower(name).rfind(prefix, 0) != 0) continue;
        entries.push_back(PathItem(dir + name, name, fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    } while (entries.size() < 200 && FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(entries.begin(), entries.end(), [](const Item& a, const Item& b) {
        const bool ad = a.glyph == glyph::kFolder, bd = b.glyph == glyph::kFolder;
        return ad != bd ? ad : ToLower(a.title) < ToLower(b.title);
    });
    if (entries.size() > 50) entries.resize(50);
    for (auto& e : entries) out.push_back(std::move(e));
}

void Reveal(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void FocusWindow(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

int IconPixelsForMonitors() {
    UINT maxDpi = 96;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            UINT x = 96, y = 96;
            if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y))) {
                *reinterpret_cast<UINT*>(lp) = std::max(*reinterpret_cast<UINT*>(lp), x);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&maxDpi));
    const int px = (int)std::ceil(kIconSize * maxDpi / 96.0f);
    return std::clamp((px + 15) / 16 * 16, 32, 96);
}

}  // namespace

Launcher& Launcher::Instance() {
    static Launcher instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Launcher::Init(HINSTANCE instance, const std::wstring& configPath, bool firstRun) {
    instance_ = instance;
    configPath_ = configPath;
    std::vector<std::wstring> warnings;
    config_ = LoadConfig(configPath_, warnings);
    const size_t slash = configPath_.find_last_of(L"\\/");
    history_.Load((slash == std::wstring::npos ? ConfigDir() : configPath_.substr(0, slash)) + L"\\history.txt");

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        return false;
    }
    auto format = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out,
                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", out.GetAddressOf()))) {
            return false;
        }
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(align);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite_->CreateEllipsisTrimmingSign(out.Get(), ellipsis.GetAddressOf());
        out->SetTrimming(&trimming, ellipsis.Get());
        return true;
    };
    // Windows 11 has Segoe Fluent Icons; Windows 10 the older MDL2 set with the same code points.
    const wchar_t* icons = L"Segoe MDL2 Assets";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(fonts.GetAddressOf(), FALSE))) {
        UINT32 i = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &i, &exists)) && exists) icons = L"Segoe Fluent Icons";
    }
    if (!format(L"Segoe UI", 14.5f, DWRITE_FONT_WEIGHT_NORMAL, titleFormat_) ||
        !format(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, subFormat_) ||
        !format(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, smallFormat_, DWRITE_TEXT_ALIGNMENT_TRAILING) ||
        !format(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, hintFormat_) ||
        !format(icons, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, glyphFormat_, DWRITE_TEXT_ALIGNMENT_CENTER) ||
        !format(icons, 18.0f, DWRITE_FONT_WEIGHT_NORMAL, searchGlyphFormat_, DWRITE_TEXT_ALIGNMENT_CENTER)) {
        return false;
    }

    backgroundBrush_ = CreateSolidBrush(kBackground);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ARC), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ARC), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!RegisterClassExW(&wc)) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"ObjectArc", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;
    BOOL dark = TRUE;
    int corner = kDwmRound;
    COLORREF border = RGB(64, 64, 68);
    DwmSetWindowAttribute(hwnd_, kDwmUseDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, kDwmCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));
    // Lets DWM draw its shadow around a borderless window.
    MARGINS margins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                            reinterpret_cast<HMENU>((INT_PTR)kEditId), instance, nullptr);
    if (!edit_) return false;
    SendMessageW(edit_, kCueBanner, TRUE, reinterpret_cast<LPARAM>(L"アプリ・ウィンドウ・コマンドを検索、計算、URL、パス..."));

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterKey();
    if (config_.trayIcon) AddTray();
    StartIndex();
    if (!warnings.empty()) {
        Notify(L"設定ファイルに問題があります", warnings.front());
    } else if (firstRun) {
        Notify(L"ObjectArc を起動しました", OpenKeysLabel(config_) + L" で開きます");
    }
    return true;
}

int Launcher::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    cancelIndex_ = true;
    if (indexer_.joinable()) indexer_.join();
    StopWinKeyHook();
    return (int)msg.wParam;
}

void Launcher::RegisterKey() {
    UnregisterHotKey(hwnd_, kHotkeyId);
    hotkeyOk_ = RegisterHotKey(hwnd_, kHotkeyId, config_.hotkey.modifiers | MOD_NOREPEAT, config_.hotkey.key) != FALSE;
    if (!hotkeyOk_) PostMessageW(hwnd_, WM_APP_WARN, 0, 0);
    if (config_.winKey) StartWinKeyHook();
    else StopWinKeyHook();
}

void Launcher::StartWinKeyHook() {
    if (winHook_.joinable()) return;
    g_hookTarget = hwnd_;
    winHook_ = std::thread([this] {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // creates the queue PostThreadMessage needs
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance_, 0);
        HHOOK mouse = SetWindowsHookExW(
            WH_MOUSE_LL,
            [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
                if (code == HC_ACTION && g_winDown && wp != WM_MOUSEMOVE) g_winUsed = true;  // Win+click
                return CallNextHookEx(nullptr, code, wp, lp);
            },
            instance_, 0);
        winHookThread_ = GetCurrentThreadId();
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        }
        if (mouse) UnhookWindowsHookEx(mouse);
        if (hook) UnhookWindowsHookEx(hook);
    });
}

void Launcher::StopWinKeyHook() {
    if (!winHook_.joinable()) return;
    while (!winHookThread_) Sleep(1);
    PostThreadMessageW(winHookThread_, WM_QUIT, 0, 0);
    winHook_.join();
    winHookThread_ = 0;
}

LRESULT CALLBACK Launcher::KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        const bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        if (k->flags & LLKHF_INJECTED) {
            // Synthesized combinations (ObjectPulse's Win+A, remote tools) aren't a lone Win press.
            if (g_winDown && !win) g_winUsed = true;
        } else if (win && down) {
            if (!g_winDown) {
                g_winDown = true;
                g_winUsed = false;
            }
        } else if (win) {
            const bool alone = g_winDown && !g_winUsed;
            g_winDown = false;
            if (alone) {
                // A key between Win down and up makes the shell treat it as a combination and skip Start.
                // The real release is swallowed and replayed after it, so the order is guaranteed.
                INPUT in[3] = {};
                for (auto& i : in) i.type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = kMaskKey;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                in[2].ki.wVk = (WORD)k->vkCode;
                in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                SendInput(3, in, sizeof(INPUT));
                PostMessageW(g_hookTarget, WM_APP_WINKEY, 0, 0);
                return 1;
            }
        } else if (down && g_winDown) {
            g_winUsed = true;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Launcher::Reload(bool announce) {
    std::vector<std::wstring> warnings;
    config_ = LoadConfig(configPath_, warnings);
    RegisterKey();
    if (config_.trayIcon && !trayAdded_) AddTray();
    if (!config_.trayIcon) RemoveTray();
    StartIndex();
    if (!warnings.empty()) Notify(L"設定ファイルに問題があります", warnings.front());
    else if (announce) Notify(L"設定を再読み込みしました", OpenKeysLabel(config_) + L" で開きます");
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

LRESULT CALLBACK Launcher::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Launcher& self = Instance();
    if (!self.hwnd_) self.hwnd_ = hwnd;
    return self.Handle(msg, wp, lp);
}

LRESULT Launcher::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && msg != 0) {
        trayAdded_ = false;
        if (config_.trayIcon) AddTray();
        return 0;
    }
    switch (msg) {
        case WM_HOTKEY:
            if (wp == kHotkeyId) Toggle();
            return 0;

        case WM_APP_WINKEY:
            Toggle();
            return 0;

        case WM_APP_INDEXED: {
            std::unique_ptr<std::vector<Item>> items(reinterpret_cast<std::vector<Item>*>(lp));
            if (indexer_.joinable()) indexer_.join();
            indexing_ = false;
            if (!items) return 0;
            index_ = std::move(*items);
            indexedAt_ = GetTickCount64();
            bitmaps_.clear();
            if (visible_) {
                const int keep = selected_;
                UpdateResults();
                selected_ = std::clamp(keep, 0, std::max((int)results_.size() - 1, 0));
                EnsureVisible();
            }
            return 0;
        }

        case WM_APP_WARN:
            Notify(L"ホットキーを登録できませんでした",
                   HotkeyLabel(config_.hotkey) + L" はほかのアプリが使っています。arc.conf の hotkey を変更してください");
            return 0;

        case WM_APP_TRAY:
            if (lp == WM_LBUTTONUP) Toggle();
            else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;

        case WM_APP_RELOAD:
            Reload(true);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == kEditId && HIWORD(wp) == EN_CHANGE) UpdateResults();
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kText);
            SetBkColor(dc, kBackground);
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && visible_) Hide();
            return 0;

        case WM_TIMER:
            if (wp == kFrameTimer) {
                KillTimer(hwnd_, kFrameTimer);
                SendFrameCommand(pendingFrameCommand_);
                pendingFrameCommand_.clear();
            }
            return 0;

        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Ignore the synthetic moves that follow scrolling and redraws.
            if (p.x == lastMouse_.x && p.y == lastMouse_.y) return 0;
            lastMouse_ = p;
            const int row = RowAt(p);
            if (row >= 0 && row != selected_) {
                selected_ = row;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const int row = RowAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (row >= 0) Activate(row);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int maxScroll = std::max((int)results_.size() - Rows(), 0);
            scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3, 0, maxScroll);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DPICHANGED:
            // Placement is computed from the monitor; the suggested rectangle is ignored.
            if ((float)LOWORD(wp) != dpi_) {
                dpi_ = (float)LOWORD(wp);
                Layout();
            }
            return 0;

        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            accent_ = Accent();
            if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd_, kHotkeyId);
            RemoveTray();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool Launcher::PreTranslate(const MSG& msg) {
    if (msg.hwnd != edit_ || msg.message != WM_KEYDOWN) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (msg.wParam) {
        case VK_DOWN: Move(1); return true;
        case VK_UP: Move(-1); return true;
        case VK_NEXT: Move(Rows()); return true;
        case VK_PRIOR: Move(-Rows()); return true;
        case VK_RETURN: Activate(selected_); return true;
        case VK_TAB:
            if (!shift && selected_ < (int)results_.size() && results_[selected_].kind == Kind::Path) Complete();
            else Move(shift ? -1 : 1);
            return true;
        case VK_ESCAPE:
            if (GetWindowTextLengthW(edit_) > 0) SetWindowTextW(edit_, L"");
            else Hide();
            return true;
        case 'N':
            if (!ctrl) return false;
            Move(1);
            return true;
        case 'P':
            if (!ctrl) return false;
            Move(-1);
            return true;
        case VK_BACK:
            if (!ctrl) return false;
            DeleteWord();  // the edit control would insert a box character instead
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void Launcher::Toggle() {
    if (visible_) Hide();
    else Show();
}

void Launcher::Show() {
    if (!indexing_ && GetTickCount64() - indexedAt_ > kReindexMs) StartIndex();
    previousForeground_ = GetForegroundWindow();
    POINT cursor;
    GetCursorPos(&cursor);
    monitor_ = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    dpi_ = (float)dx;
    accent_ = Accent();

    windows_ = config_.showWindows ? ListWindows(hwnd_, index_, IconPixelsForMonitors()) : std::vector<Item>{};
    commands_ = BuiltinCommands(config_.frameCommands);
    if (config_.keepQuery) {
        UpdateResults();
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    } else if (GetWindowTextLengthW(edit_) > 0) {
        SetWindowTextW(edit_, L"");  // updates the results through EN_CHANGE
    } else {
        UpdateResults();
    }
    Layout();
    visible_ = true;
    GetCursorPos(&lastMouse_);
    ScreenToClient(hwnd_, &lastMouse_);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
}

void Launcher::Hide() {
    if (!visible_) return;
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    windows_.clear();
}

float Launcher::Scale() const { return dpi_ / 96.0f * config_.scale / 100.0f; }

int Launcher::Rows() const {
    if (!config_.compact) return config_.rows;
    if (Query().empty()) return 0;
    return std::clamp((int)results_.size(), 1, config_.rows);
}

void Launcher::FitHeight() {
    RECT r;
    GetWindowRect(hwnd_, &r);
    const int w = r.right - r.left, h = (int)std::lround(TotalHeight(Rows()) * Scale());
    if (r.bottom - r.top == h) return;
    SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (rt_) rt_->Resize({(UINT32)w, (UINT32)h});
}

void Launcher::Layout() {
    const float s = Scale();
    const int w = (int)std::lround(kWidth * s), h = (int)std::lround(TotalHeight(Rows()) * s);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);
    const RECT& work = mi.rcWork;
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    const int x = work.left + (workW - w) / 2;
    // Placed for the full height, so a compact window grows downward without moving.
    const int fullH = (int)std::lround(TotalHeight(config_.rows) * s);
    const int y = work.top + std::max(0, std::min((int)(workH * 0.2f), workH - fullH));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    if (editFont_) DeleteObject(editFont_);
    const int fontPx = (int)std::lround(kEditFont * s);
    editFont_ = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    const int editH = (int)std::lround(fontPx * 1.45f);
    const int editX = (int)std::lround(kEditLeft * s);
    MoveWindow(edit_, editX, ((int)std::lround(kSearchHeight * s) - editH) / 2, w - editX - (int)std::lround(20 * s),
               editH, TRUE);

    if (rt_) {
        rt_->Resize({(UINT32)w, (UINT32)h});
        rt_->SetDpi(96.0f * s, 96.0f * s);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

void Launcher::StartIndex() {
    if (indexing_) return;
    if (indexer_.joinable()) indexer_.join();
    indexing_ = true;
    cancelIndex_ = false;
    indexer_ = std::thread([this, folders = config_.folders, px = IconPixelsForMonitors(), hwnd = hwnd_] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* items = new std::vector<Item>(BuildIndex(folders, px, cancelIndex_));
        CoUninitialize();
        if (cancelIndex_ || !PostMessageW(hwnd, WM_APP_INDEXED, 0, reinterpret_cast<LPARAM>(items))) delete items;
    });
}

std::wstring Launcher::Query() const {
    const int n = GetWindowTextLengthW(edit_);
    std::wstring text(n + 1, L'\0');
    GetWindowTextW(edit_, text.data(), n + 1);
    text.resize(n);
    return Trim(text);
}

void Launcher::UpdateResults() {
    results_.clear();
    selected_ = 0;
    scroll_ = 0;
    const std::wstring query = Query();
    const std::wstring nq = Normalize(query);

    if (nq.empty() && config_.compact) {
        // Just the search bar.
    } else if (nq.empty()) {
        // Recently used first, then the open windows.
        for (const auto& key : history_.Recent(config_.rows)) {
            for (const auto* list : {&index_, &commands_}) {
                auto it = std::find_if(list->begin(), list->end(), [&](const Item& i) { return i.key == key; });
                if (it != list->end()) {
                    results_.push_back(*it);
                    break;
                }
            }
        }
        for (const auto& w : windows_) results_.push_back(w);
        if (results_.empty()) results_ = commands_;
    } else if (nq[0] == L'>') {
        const std::wstring command = Trim(query.substr(1));
        if (!command.empty()) {
            Item s;
            s.kind = Kind::Shell;
            s.title = command;
            s.subtitle = L"コマンド プロンプトで実行";
            s.target = command;
            s.glyph = glyph::kTerminal;
            results_.push_back(std::move(s));
        }
    } else {
        if (auto value = Calculate(nq)) {
            Item c;
            c.kind = Kind::Calc;
            c.title = L"= " + FormatNumber(*value);
            c.subtitle = query;
            c.target = FormatNumber(*value);
            c.glyph = glyph::kCalculator;
            results_.push_back(std::move(c));
        }
        if (LooksLikePath(query)) AddPathResults(query, results_);
        if (LooksLikeUrl(query)) {
            Item u;
            u.kind = Kind::Url;
            u.title = query;
            u.subtitle = L"ブラウザーで開く";
            u.target = query.find(L"://") == std::wstring::npos ? L"https://" + query : query;
            u.glyph = glyph::kLink;
            results_.push_back(std::move(u));
        }
        const size_t pinned = results_.size();

        auto consider = [&](const Item& item, int minimum) {
            int score = MatchScore(nq, item.match);
            if (!item.keywords.empty()) score = std::max(score, MatchScore(nq, item.keywords) * 9 / 10);
            if (score < minimum) return;
            Item r = item;
            r.score = score + history_.Boost(item.key);
            results_.push_back(std::move(r));
        };
        for (const auto& i : index_) consider(i, 1);
        for (const auto& w : windows_) consider(w, 1);
        // Scattered-letter matches make commands noisy.
        for (const auto& c : commands_) consider(c, 600);
        std::stable_sort(results_.begin() + pinned, results_.end(),
                         [](const Item& a, const Item& b) { return a.score > b.score; });
        if (results_.size() > kMaxResults) results_.resize(kMaxResults);

        Item web;
        web.kind = Kind::Search;
        web.title = query;
        web.subtitle = config_.searchName + L" で検索";
        std::wstring url = config_.searchUrl;
        url.replace(url.find(L"%s"), 2, UrlEncode(query));
        web.target = url;
        web.glyph = glyph::kGlobe;
        results_.push_back(std::move(web));
    }
    if (config_.compact && visible_) FitHeight();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Launcher::Move(int delta) {
    if (results_.empty()) return;
    const int n = (int)results_.size();
    if (std::abs(delta) == 1) selected_ = (selected_ + delta + n) % n;  // arrows wrap around
    else selected_ = std::clamp(selected_ + delta, 0, n - 1);
    EnsureVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Launcher::EnsureVisible() {
    if (selected_ < scroll_) scroll_ = selected_;
    const int rows = Rows();
    if (selected_ >= scroll_ + rows) scroll_ = selected_ - rows + 1;
    scroll_ = std::clamp(scroll_, 0, std::max((int)results_.size() - rows, 0));
}

int Launcher::RowAt(POINT p) const {
    const float s = Scale();
    const float y = p.y / s - (kSearchHeight + 1 + kListPad);
    if (y < 0 || y >= Rows() * kRowHeight) return -1;
    const int row = scroll_ + (int)(y / kRowHeight);
    return row < (int)results_.size() ? row : -1;
}

void Launcher::Activate(int index) {
    if (index < 0 || index >= (int)results_.size()) return;
    const Item item = results_[index];  // hiding and running may rebuild the results
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    history_.Record(item.key);
    Hide();
    Execute(item, shift, ctrl);
}

void Launcher::Execute(const Item& item, bool admin, bool reveal) {
    auto open = [](const std::wstring& target, const wchar_t* verb = nullptr, const wchar_t* args = nullptr) {
        ShellExecuteW(nullptr, verb, target.c_str(), args, nullptr, SW_SHOWNORMAL);
    };
    switch (item.kind) {
        case Kind::App:
            if (reveal && !item.path.empty()) Reveal(item.path);
            else if (admin && !item.path.empty()) open(item.path, L"runas");
            else open(L"shell:AppsFolder\\" + item.target);
            break;
        case Kind::File:
        case Kind::Path:
            if (reveal) Reveal(item.target);
            else open(item.target, admin ? L"runas" : nullptr);
            break;
        case Kind::Window: FocusWindow(item.hwnd); break;
        case Kind::Command: RunCommand(item); break;
        case Kind::Calc: CopyText(hwnd_, item.target); break;
        case Kind::Url:
        case Kind::Search: open(item.target); break;
        case Kind::Shell: open(L"cmd.exe", admin ? L"runas" : nullptr, (L"/k " + item.target).c_str()); break;
    }
}

void Launcher::RunCommand(const Item& item) {
    const std::wstring& id = item.target;
    if (item.confirm && MessageBoxW(nullptr, (item.title + L"しますか?").c_str(), L"ObjectArc",
                                    MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) != IDOK) {
        return;
    }
    if (id.rfind(L"of:", 0) == 0) {
        // Window commands act on the focused window, so give focus back first.
        if (previousForeground_ && IsWindow(previousForeground_)) SetForegroundWindow(previousForeground_);
        pendingFrameCommand_ = id.substr(3);
        SetTimer(hwnd_, kFrameTimer, 100, nullptr);
    } else if (id == L"lock") {
        LockWorkStation();
    } else if (id == L"sleep") {
        SetSuspendState(FALSE, FALSE, FALSE);
    } else if (id == L"shutdown" || id == L"restart") {
        ShellExecuteW(nullptr, L"open", L"shutdown.exe", id == L"shutdown" ? L"/s /t 0" : L"/r /t 0", nullptr, SW_HIDE);
    } else if (id == L"signout") {
        ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED);
    } else if (id == L"settings") {
        ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == L"recycle") {
        SHEmptyRecycleBinW(nullptr, nullptr, 0);  // the shell asks for confirmation itself
    } else if (id == L"taskmgr") {
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == L"arc-config") {
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + configPath_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    } else if (id == L"arc-reload") {
        Reload(true);
    } else if (id == L"arc-quit") {
        DestroyWindow(hwnd_);
    }
}

void Launcher::Complete() {
    const Item& item = results_[selected_];
    std::wstring text = item.target;
    if (item.keywords == L"\\" && text.back() != L'\\') text += L'\\';
    SetWindowTextW(edit_, text.c_str());
    SendMessageW(edit_, EM_SETSEL, text.size(), text.size());
}

void Launcher::DeleteWord() {
    DWORD start = 0, end = 0;
    SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const int n = GetWindowTextLengthW(edit_);
    std::wstring text(n + 1, L'\0');
    GetWindowTextW(edit_, text.data(), n + 1);
    text.resize(n);
    if (start == end) {
        while (start > 0 && std::iswspace(text[start - 1])) --start;
        while (start > 0 && !std::iswspace(text[start - 1]) && text[start - 1] != L'\\' && text[start - 1] != L'/') {
            --start;
        }
        if (start == end && start > 0) --start;  // a lone separator
    }
    text.erase(start, end - start);
    SetWindowTextW(edit_, text.c_str());
    SendMessageW(edit_, EM_SETSEL, start, start);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool Launcher::CreateDeviceResources() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const float dpi = 96.0f * Scale();
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    const auto hwndProps =
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, rt_.GetAddressOf()))) return false;
    if (FAILED(rt_->CreateSolidColorBrush(Color(kText), brush_.GetAddressOf()))) {
        rt_.Reset();
        return false;
    }
    bitmaps_.clear();
    return true;
}

ID2D1Bitmap* Launcher::Bitmap(const Icon& icon) {
    if (!icon || icon->pixels.empty()) return nullptr;
    auto it = bitmaps_.find(icon.get());
    if (it != bitmaps_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bmp;
    const auto props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU(icon->width, icon->height), icon->pixels.data(), icon->width * 4, props,
                                 bmp.GetAddressOf()))) {
        return nullptr;
    }
    return (bitmaps_[icon.get()] = bmp).Get();
}

void Launcher::DrawLabel(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                         COLORREF color, float alpha, float* width) {
    if (width) *width = 0;
    if (text.empty() || rect.right <= rect.left) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, rect.right - rect.left,
                                         rect.bottom - rect.top, layout.GetAddressOf()))) {
        return;
    }
    if (width) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        *width = std::min(m.widthIncludingTrailingWhitespace, rect.right - rect.left);
    }
    brush_->SetColor(Color(color, alpha));
    rt_->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Launcher::Paint() {
    if (!CreateDeviceResources()) return;
    const D2D1_SIZE_F size = rt_->GetSize();
    const float W = size.width, H = size.height;
    rt_->BeginDraw();
    rt_->Clear(Color(kBackground));

    // Search box
    DrawLabel(std::wstring(1, glyph::kSearch), searchGlyphFormat_.Get(), {16, 0, 46, kSearchHeight}, kSubtext);
    const int rows = Rows();
    if (rows == 0) return EndFrame();
    brush_->SetColor(Color(kWhite, 0.07f));
    rt_->FillRectangle({0, kSearchHeight, W, kSearchHeight + 1}, brush_.Get());

    // Results
    const float top = kSearchHeight + 1 + kListPad;
    const int last = std::min((int)results_.size(), scroll_ + rows);
    for (int i = scroll_; i < last; ++i) {
        const Item& item = results_[i];
        const float y = top + (i - scroll_) * kRowHeight;
        if (i == selected_) {
            brush_->SetColor(Color(kWhite, 0.085f));
            rt_->FillRoundedRectangle({{8, y, W - 8, y + kRowHeight}, 8, 8}, brush_.Get());
        }
        const D2D1_RECT_F icon{22, y + (kRowHeight - kIconSize) / 2, 22 + kIconSize, y + (kRowHeight + kIconSize) / 2};
        if (ID2D1Bitmap* bmp = Bitmap(item.icon)) {
            rt_->DrawBitmap(bmp, icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            brush_->SetColor(Color(GlyphColor(item.kind, accent_)));
            rt_->FillRoundedRectangle({icon, 7, 7}, brush_.Get());
            DrawLabel(std::wstring(1, item.glyph ? item.glyph : glyph::kApp), glyphFormat_.Get(), icon, kWhite);
        }

        float accessoryWidth = 0;
        const std::wstring accessory = KindLabel(item.kind);
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_->CreateTextLayout(accessory.c_str(), (UINT32)accessory.size(), smallFormat_.Get(),
                                                    200, kRowHeight, layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                accessoryWidth = m.width;
            }
        }
        DrawLabel(accessory, smallFormat_.Get(), {W - 24 - accessoryWidth - 4, y, W - 24, y + kRowHeight}, kDim);

        const float textLeft = 22 + kIconSize + 14, textRight = W - 24 - accessoryWidth - 20;
        float titleWidth = 0;
        DrawLabel(item.title, titleFormat_.Get(), {textLeft, y, textRight, y + kRowHeight}, kText, 1.0f, &titleWidth);
        DrawLabel(item.subtitle, subFormat_.Get(), {textLeft + titleWidth + 10, y, textRight, y + kRowHeight},
                  kSubtext);
    }
    if (results_.empty()) {
        DrawLabel(L"入力して検索", subFormat_.Get(), {24, top, W - 24, top + kRowHeight}, kDim);
    }
    // Scroll indicator
    const int n = (int)results_.size();
    if (n > rows) {
        const float track = rows * kRowHeight;
        const float thumb = std::max(track * rows / n, 18.0f);
        const float y = top + (track - thumb) * scroll_ / (n - rows);
        brush_->SetColor(Color(kWhite, 0.16f));
        rt_->FillRoundedRectangle({{W - 5, y, W - 2, y + thumb}, 1.5f, 1.5f}, brush_.Get());
    }

    // Footer
    const float fy = H - kFooterHeight;
    brush_->SetColor(Color(kWhite, 0.07f));
    rt_->FillRectangle({0, fy - 1, W, fy}, brush_.Get());
    brush_->SetColor(Color(kWhite, 0.025f));
    rt_->FillRectangle({0, fy, W, H}, brush_.Get());
    DrawLabel(L"ObjectArc  ·  " + OpenKeysLabel(config_) + (indexing_ && index_.empty() ? L"  ·  読み込み中..." : L""),
              hintFormat_.Get(), {20, fy, W / 2, H}, kDim);
    if (selected_ < n) {
        const Item& item = results_[selected_];
        const std::wstring action = std::wstring(ActionLabel(item.kind)) + L"  ↵";
        float actionWidth = 0;
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_->CreateTextLayout(action.c_str(), (UINT32)action.size(), hintFormat_.Get(), 300,
                                                    kFooterHeight, layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                actionWidth = m.width;
            }
        }
        DrawLabel(action, hintFormat_.Get(), {W - 20 - actionWidth - 2, fy, W - 20, H}, kText);
        std::wstring extra;
        if ((item.kind == Kind::App && !item.path.empty()) || item.kind == Kind::File || item.kind == Kind::Path) {
            extra = L"Ctrl+↵ 場所を開く    Shift+↵ 管理者として実行";
        }
        if (item.kind == Kind::Path) extra = L"Tab 補完    " + extra;
        if (!extra.empty()) {
            ComPtr<IDWriteTextLayout> layout;
            float extraWidth = 0;
            if (SUCCEEDED(dwrite_->CreateTextLayout(extra.c_str(), (UINT32)extra.size(), hintFormat_.Get(), 500,
                                                    kFooterHeight, layout.GetAddressOf()))) {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                extraWidth = m.width;
            }
            const float right = W - 20 - actionWidth - 24;
            DrawLabel(extra, hintFormat_.Get(), {right - extraWidth - 2, fy, right, H}, kDim);
        }
    }

    EndFrame();
}

void Launcher::EndFrame() {
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
        bitmaps_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Tray and settings
// ---------------------------------------------------------------------------

void Launcher::AddTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_ARC), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    const std::wstring tip = L"ObjectArc (" + OpenKeysLabel(config_) + L")";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void Launcher::RemoveTray() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void Launcher::Notify(const std::wstring& title, const std::wstring& text) {
    if (!trayAdded_) {
        // Without a tray icon there's nowhere for a balloon; only problems are worth a dialog.
        if (title.find(L"問題") != std::wstring::npos || title.find(L"できません") != std::wstring::npos) {
            MessageBoxW(nullptr, (title + L"\n" + text).c_str(), L"ObjectArc", MB_ICONWARNING | MB_TOPMOST);
        }
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Launcher::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayOpen, (L"開く\t" + OpenKeysLabel(config_)).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayConfig, L"設定ファイルを開く");
    AppendMenuW(menu, MF_STRING, kTrayReload, L"設定を再読み込み");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayQuit, L"ObjectArc を終了");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (cmd) {
        case kTrayOpen: Show(); break;
        case kTrayConfig:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + configPath_ + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
            break;
        case kTrayReload: Reload(true); break;
        case kTrayQuit: DestroyWindow(hwnd_); break;
    }
}

COLORREF Launcher::Accent() const {
    if (!config_.accentFromSystem) return config_.accent;
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) != ERROR_SUCCESS) {
        return config_.accent;
    }
    int r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
    // Some accents are too dark to read on the dark background; lift them toward white.
    const int luma = (r * 299 + g * 587 + b * 114) / 1000;
    if (luma < 110) {
        const float t = (110 - luma) / 110.0f;
        r += (int)((255 - r) * t);
        g += (int)((255 - g) * t);
        b += (int)((255 - b) * t);
    }
    return RGB(r, g, b);
}

}  // namespace oa
