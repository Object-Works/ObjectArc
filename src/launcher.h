#pragma once

#include "config.h"
#include "sources.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace oa {

using Microsoft::WRL::ComPtr;

// The search window: a text box over a list of results, opened with a global hotkey.
class Launcher {
public:
    static Launcher& Instance();

    bool Init(HINSTANCE instance, const std::wstring& configPath, bool firstRun);
    int Run();

private:
    Launcher() = default;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool PreTranslate(const MSG& msg);  // keys typed into the text box that drive the list

    // Visibility
    void Show();
    void Hide();
    void Toggle();
    void Layout();
    void FitHeight();     // compact mode: grows and shrinks with the results, keeping the top in place
    int Rows() const;     // result rows currently shown; 0 = just the search bar
    float Scale() const;  // DIPs to pixels, including the configured scale

    // Results
    void StartIndex();
    void UpdateResults();
    std::wstring Query() const;
    void Move(int delta);
    void EnsureVisible();
    int RowAt(POINT clientPx) const;
    void Activate(int index);
    void Execute(const Item& item, bool admin, bool reveal);
    void RunCommand(const Item& item);
    void Complete();  // Tab on a path: puts it into the text box
    void DeleteWord();

    // Drawing
    bool CreateDeviceResources();
    void Paint();
    void EndFrame();  // finishes a Paint, dropping the device resources if the device was lost
    ID2D1Bitmap* Bitmap(const Icon& icon);
    void DrawLabel(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, COLORREF color,
                  float alpha = 1.0f, float* width = nullptr);

    // Tray and settings
    void AddTray();
    void RemoveTray();
    void ShowTrayMenu();
    void Notify(const std::wstring& title, const std::wstring& text);
    void RegisterKey();
    // The Win key alone can't be a RegisterHotKey hotkey, so a low-level keyboard hook watches it
    // on its own thread (a busy UI thread would otherwise delay every keystroke system-wide).
    void StartWinKeyHook();
    void StopWinKeyHook();
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp);
    void Reload(bool announce);
    COLORREF Accent() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    std::wstring configPath_;
    Config config_;
    History history_;
    bool trayAdded_ = false;
    bool hotkeyOk_ = false;
    std::thread winHook_;
    std::atomic<DWORD> winHookThread_{0};

    // Index, built on a worker thread
    std::vector<Item> index_;
    std::thread indexer_;
    std::atomic<bool> indexing_{false};
    std::atomic<bool> cancelIndex_{false};
    ULONGLONG indexedAt_ = 0;

    // Current search
    bool visible_ = false;
    std::vector<Item> windows_;   // snapshot taken when the launcher opens
    std::vector<Item> commands_;
    std::vector<Item> results_;
    int selected_ = 0;
    int scroll_ = 0;
    POINT lastMouse_{-1, -1};
    std::wstring pendingFrameCommand_;
    HWND previousForeground_ = nullptr;

    // Placement
    HMONITOR monitor_ = nullptr;
    float dpi_ = 96.0f;
    COLORREF accent_ = RGB(0x7a, 0xa2, 0xf7);

    // Direct2D
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> titleFormat_, subFormat_, smallFormat_, glyphFormat_, searchGlyphFormat_, hintFormat_;
    std::map<const IconPixels*, ComPtr<ID2D1Bitmap>> bitmaps_;
};

}  // namespace oa
