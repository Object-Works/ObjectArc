#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>

namespace oa {

namespace {

const wchar_t kDefaultConfig[] = LR"CFG(# ObjectArc 設定ファイル
# 書式: key = value     '#' 以降はコメント
# 編集後はトレイアイコンを右クリック →「設定を再読み込み」か、ObjectArc で「再読み込み」を実行すると反映されます。

# ---- ウィンドウ ----
win_key = true            # Win キーを単独で押して離すと開く (スタートメニューの代わり。Win+E などの組み合わせはそのまま使えます)
hotkey = alt+space        # 開く / 閉じるキー (ctrl / alt / shift / win と、space・a-z・0-9・f1-f12 などの組み合わせ)
rows = 8                  # 一度に表示する候補の数 (4-12)
compact = false           # true = 開いたときは検索バーだけ。入力すると候補の数だけ下に広がる
scale = 100               # 大きさ (%, 50-200)
keep_query = false        # 開き直したときに前回の入力を残す
tray_icon = true          # 通知領域にアイコンを表示

# ---- 候補 ----
show_windows = true       # 開いているウィンドウを候補に出す
frame_commands = true     # ObjectFrame が起動中なら、ワークスペースやレイアウトの操作を候補に出す
# folder = D:\Tools       # アプリ一覧に加えて、このフォルダーの .exe / .lnk / .bat などを候補に出す (複数行可)
search_name = Google      # 何にも当てはまらないときの Web 検索
search_url = https://www.google.com/search?q=%s

# ---- 見た目 ----
accent = auto             # auto = Windows のアクセントカラー / 0xRRGGBB
)CFG";

bool ParseBool(const std::wstring& v, bool& out) {
    auto l = ToLower(v);
    if (l == L"true" || l == L"yes" || l == L"on" || l == L"1") out = true;
    else if (l == L"false" || l == L"no" || l == L"off" || l == L"0") out = false;
    else return false;
    return true;
}

bool ParseColor(const std::wstring& v, COLORREF& out) {
    std::wstring s = v;
    if (s.rfind(L"0x", 0) == 0 || s.rfind(L"0X", 0) == 0) s = s.substr(2);
    else if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() != 6 || s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return false;
    unsigned long rgb = std::wcstoul(s.c_str(), nullptr, 16);
    out = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return true;
}

bool ParseInt(const std::wstring& v, int lo, int hi, int& out) {
    if (v.empty() || v.find_first_not_of(L"-0123456789") != std::wstring::npos) return false;
    out = std::clamp(_wtoi(v.c_str()), lo, hi);
    return true;
}

struct KeyName {
    const wchar_t* name;
    UINT vk;
};

const KeyName kKeys[] = {
    {L"space", VK_SPACE}, {L"enter", VK_RETURN}, {L"tab", VK_TAB},       {L"esc", VK_ESCAPE},
    {L"insert", VK_INSERT}, {L"delete", VK_DELETE}, {L"home", VK_HOME},   {L"end", VK_END},
    {L"pageup", VK_PRIOR}, {L"pagedown", VK_NEXT},  {L"`", VK_OEM_3},     {L";", VK_OEM_1},
    {L",", VK_OEM_COMMA},  {L".", VK_OEM_PERIOD},   {L"/", VK_OEM_2},
};

}  // namespace

std::wstring DefaultConfigPath() { return ConfigDir() + L"\\arc.conf"; }

bool EnsureConfigFile(const std::wstring& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return false;
    fputws(kDefaultConfig, f);
    fclose(f);
    return true;
}

bool ParseHotkey(const std::wstring& text, Hotkey& out) {
    Hotkey h{0, 0};
    for (auto& raw : Split(ToLower(text), L'+')) {
        const std::wstring part = Trim(raw);
        if (part == L"ctrl" || part == L"control") h.modifiers |= MOD_CONTROL;
        else if (part == L"alt") h.modifiers |= MOD_ALT;
        else if (part == L"shift") h.modifiers |= MOD_SHIFT;
        else if (part == L"win" || part == L"super") h.modifiers |= MOD_WIN;
        else if (h.key) return false;
        else if (part.size() == 1 && std::iswalnum(part[0])) h.key = (UINT)std::towupper(part[0]);
        else if (part.size() >= 2 && part[0] == L'f' && part.find_first_not_of(L"0123456789", 1) == std::wstring::npos) {
            const int n = _wtoi(part.c_str() + 1);
            if (n < 1 || n > 24) return false;
            h.key = VK_F1 + n - 1;
        } else {
            for (const auto& k : kKeys) {
                if (part == k.name) h.key = k.vk;
            }
            if (!h.key) return false;
        }
    }
    if (!h.key) return false;
    out = h;
    return true;
}

std::wstring HotkeyLabel(const Hotkey& h) {
    std::wstring s;
    if (h.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (h.modifiers & MOD_ALT) s += L"Alt+";
    if (h.modifiers & MOD_SHIFT) s += L"Shift+";
    if (h.modifiers & MOD_WIN) s += L"Win+";
    if ((h.key >= L'A' && h.key <= L'Z') || (h.key >= L'0' && h.key <= L'9')) return s + (wchar_t)h.key;
    if (h.key >= VK_F1 && h.key <= VK_F24) return s + L"F" + std::to_wstring(h.key - VK_F1 + 1);
    if (h.key == VK_SPACE) return s + L"Space";
    for (const auto& k : kKeys) {
        if (k.vk == h.key) return s + k.name;
    }
    return s + L"?";
}

std::wstring OpenKeysLabel(const Config& config) {
    return (config.winKey ? L"Win / " : L"") + HotkeyLabel(config.hotkey);
}

Config LoadConfig(const std::wstring& path, std::vector<std::wstring>& warnings) {
    Config cfg;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return cfg;
    wchar_t buf[1024];
    int lineNo = 0;
    while (fgetws(buf, 1024, f)) {
        ++lineNo;
        std::wstring line = buf;
        // URLs may contain '#', so only a '#' at the start or after a space starts a comment.
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == L'#' && (i == 0 || std::iswspace(line[i - 1]))) {
                line.resize(i);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            warnings.push_back(L"line " + std::to_wstring(lineNo) + L": expected key = value");
            continue;
        }
        std::wstring k = ToLower(Trim(line.substr(0, eq)));
        std::wstring v = Trim(line.substr(eq + 1));
        bool ok = true;
        if (k == L"hotkey") ok = ParseHotkey(v, cfg.hotkey);
        else if (k == L"win_key") ok = ParseBool(v, cfg.winKey);
        else if (k == L"rows") ok = ParseInt(v, 4, 12, cfg.rows);
        else if (k == L"compact") ok = ParseBool(v, cfg.compact);
        else if (k == L"scale") ok = ParseInt(v, 50, 200, cfg.scale);
        else if (k == L"keep_query") ok = ParseBool(v, cfg.keepQuery);
        else if (k == L"tray_icon") ok = ParseBool(v, cfg.trayIcon);
        else if (k == L"show_windows") ok = ParseBool(v, cfg.showWindows);
        else if (k == L"frame_commands") ok = ParseBool(v, cfg.frameCommands);
        else if (k == L"folder") {
            ok = !v.empty();
            if (ok) cfg.folders.push_back(v);
        } else if (k == L"search_name") {
            ok = !v.empty();
            if (ok) cfg.searchName = v;
        } else if (k == L"search_url") {
            ok = v.find(L"%s") != std::wstring::npos;
            if (ok) cfg.searchUrl = v;
        } else if (k == L"accent") {
            cfg.accentFromSystem = ToLower(v) == L"auto";
            if (!cfg.accentFromSystem) ok = ParseColor(v, cfg.accent);
        } else {
            warnings.push_back(L"line " + std::to_wstring(lineNo) + L": unknown key '" + k + L"'");
            continue;
        }
        if (!ok) warnings.push_back(L"line " + std::to_wstring(lineNo) + L": invalid value for " + k);
    }
    fclose(f);
    return cfg;
}

}  // namespace oa
