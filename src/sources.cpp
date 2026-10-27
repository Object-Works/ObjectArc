#include "sources.h"

#include "match.h"

#include <dwmapi.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <set>

namespace oa {

namespace {

// "WindowsCalculator" -> "Windows Calculator", so each part matches as a word.
std::wstring SplitCamel(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && std::iswupper(s[i]) && std::iswlower(s[i - 1])) out += L' ';
        out += s[i];
    }
    return out;
}

std::wstring Stem(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

// Apps folder ids of desktop apps start with a known folder GUID: "{6D80...}\Microsoft VS Code\Code.exe".
std::wstring ResolveAppPath(const std::wstring& id) {
    if (id.size() > 3 && id[1] == L':' && id[2] == L'\\') return id;
    size_t close = id.find(L"}\\");
    if (id.empty() || id[0] != L'{' || close == std::wstring::npos) return {};
    GUID folder;
    if (FAILED(CLSIDFromString(id.substr(0, close + 1).c_str(), &folder))) return {};
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &base))) return {};
    std::wstring path = std::wstring(base) + id.substr(close + 1);
    CoTaskMemFree(base);
    return path;
}

// Store apps: "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App" -> "windows calculator", so English
// words find apps whose display name is Japanese (電卓).
std::wstring PackageWords(const std::wstring& id) {
    size_t bang = id.find(L'!'), underscore = id.find(L'_');
    if (bang == std::wstring::npos || underscore == std::wstring::npos || underscore > bang) return {};
    std::wstring name = id.substr(0, underscore);
    if (size_t dot = name.find(L'.'); dot != std::wstring::npos) name = name.substr(dot + 1);
    std::replace(name.begin(), name.end(), L'.', L' ');
    return SplitCamel(name);
}

bool Hidden(const std::wstring& title) {
    const std::wstring t = ToLower(title);
    return t.find(L"uninstall") != std::wstring::npos || t.find(L"アンインストール") != std::wstring::npos;
}

void Finish(Item& item) {
    item.match = Normalize(item.title);
    item.keywords = Normalize(item.keywords);
}

bool IsTaskWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW) && (GetWindow(h, GW_OWNER) || (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))) {
        return false;
    }
    return GetWindowTextLengthW(h) > 0;
}

std::wstring ProcessPath(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = (DWORD)std::size(buf);
    std::wstring path = QueryFullProcessImageNameW(process, 0, buf, &size) ? std::wstring(buf, size) : L"";
    CloseHandle(process);
    return path;
}

long long Now() { return (long long)std::time(nullptr); }

}  // namespace

// ---------------------------------------------------------------------------
// Index
// ---------------------------------------------------------------------------

std::vector<Item> BuildIndex(const std::vector<std::wstring>& folders, int iconPx, const std::atomic<bool>& cancel) {
    std::vector<Item> out;
    std::set<std::wstring> seen;

    IShellItem* apps = nullptr;
    IEnumShellItems* items = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&apps))) &&
        SUCCEEDED(apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        IShellItem* it = nullptr;
        while (!cancel && items->Next(1, &it, nullptr) == S_OK) {
            PWSTR name = nullptr, id = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && !Hidden(name) && seen.insert(id).second) {
                Item a;
                a.kind = Kind::App;
                a.title = name;
                a.target = id;
                a.key = L"app:" + a.target;
                a.path = ResolveAppPath(a.target);
                a.keywords = a.path.empty() ? PackageWords(a.target) : SplitCamel(Stem(a.path));
                a.icon = IconFromShellItem(it, iconPx);
                Finish(a);
                out.push_back(std::move(a));
            }
            CoTaskMemFree(name);
            CoTaskMemFree(id);
            it->Release();
        }
    }
    if (items) items->Release();
    if (apps) apps->Release();

    static const std::set<std::wstring> kExtensions = {L".exe", L".lnk", L".bat", L".cmd", L".url",
                                                       L".ps1", L".msc", L".appref-ms"};
    namespace fs = std::filesystem;
    for (const auto& folder : folders) {
        std::error_code ec;
        fs::recursive_directory_iterator it(ExpandEnv(folder), fs::directory_options::skip_permission_denied, ec), end;
        int added = 0;
        for (; !ec && it != end && !cancel && added < 2000; it.increment(ec)) {
            if (it.depth() >= 2) it.disable_recursion_pending();
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc)) continue;
            const fs::path& p = it->path();
            if (!kExtensions.count(ToLower(p.extension().wstring()))) continue;
            Item f;
            f.kind = Kind::File;
            f.title = p.stem().wstring();
            f.subtitle = p.parent_path().wstring();
            f.target = f.path = p.wstring();
            f.key = L"file:" + f.target;
            f.keywords = SplitCamel(f.title);
            f.icon = IconFromPath(f.target, iconPx);
            Finish(f);
            out.push_back(std::move(f));
            ++added;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

std::vector<Item> ListWindows(HWND self, const std::vector<Item>& apps, int iconPx) {
    struct Ctx {
        HWND self;
        std::vector<HWND> list;
    } ctx{self, {}};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (h != c->self && IsTaskWindow(h)) c->list.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    static std::map<std::wstring, Icon> iconCache;  // by exe; windows come and go, exes rarely change
    std::vector<Item> out;
    for (HWND h : ctx.list) {
        wchar_t title[512];
        GetWindowTextW(h, title, 512);
        Item w;
        w.kind = Kind::Window;
        w.title = title;
        w.hwnd = h;
        const std::wstring exe = ProcessPath(h);
        const std::wstring exeLower = ToLower(exe);
        const Item* app = nullptr;
        if (exeLower.size() >= 24 && exeLower.ends_with(L"\\applicationframehost.exe")) {
            // Store apps all run inside the same frame host; the title names the app.
            for (const auto& a : apps) {
                if (a.kind == Kind::App && (w.title == a.title || w.title.ends_with(L"- " + a.title))) app = &a;
            }
        } else {
            for (const auto& a : apps) {
                if (!a.path.empty() && ToLower(a.path) == exeLower) app = &a;
            }
        }
        w.subtitle = app ? app->title : Stem(exe);
        w.keywords = w.subtitle;
        if (app && app->icon) {
            w.icon = app->icon;
        } else {
            auto cached = iconCache.find(exeLower);
            if (cached != iconCache.end() && cached->second) {
                w.icon = cached->second;
            } else {
                w.icon = IconFromWindow(h, exe, iconPx);
                if (!exe.empty()) iconCache[exeLower] = w.icon;
            }
        }
        w.glyph = glyph::kWindow;
        Finish(w);
        out.push_back(std::move(w));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

std::vector<Item> BuiltinCommands(bool frameCommands) {
    std::vector<Item> out;
    auto add = [&](const wchar_t* id, const wchar_t* title, const wchar_t* subtitle, wchar_t g, const wchar_t* words,
                   bool confirm = false) {
        Item c;
        c.kind = Kind::Command;
        c.title = title;
        c.subtitle = subtitle;
        c.target = id;
        c.key = std::wstring(L"cmd:") + id;
        c.glyph = g;
        c.keywords = words;
        c.confirm = confirm;
        Finish(c);
        out.push_back(std::move(c));
    };
    add(L"lock", L"ロック", L"画面をロック", glyph::kLock, L"lock screen");
    add(L"sleep", L"スリープ", L"PC をスリープ", glyph::kSleep, L"sleep suspend");
    add(L"shutdown", L"シャットダウン", L"PC の電源を切る", glyph::kPower, L"shutdown shut down power off", true);
    add(L"restart", L"再起動", L"PC を再起動", glyph::kRestart, L"restart reboot saikidou", true);
    add(L"signout", L"サインアウト", L"Windows からサインアウト", glyph::kSignOut, L"sign out log off logout", true);
    add(L"settings", L"Windows の設定", L"設定アプリを開く", glyph::kSettings, L"settings windows settei");
    add(L"recycle", L"ごみ箱を空にする", L"ごみ箱のファイルを完全に削除", glyph::kDelete, L"empty recycle bin trash");
    add(L"taskmgr", L"タスク マネージャー", L"実行中のアプリとプロセス", glyph::kTaskManager, L"task manager taskmgr");
    add(L"arc-config", L"ObjectArc の設定ファイルを開く", L"arc.conf", glyph::kEdit, L"objectarc config settings");
    add(L"arc-reload", L"ObjectArc の設定を再読み込み", L"アプリの一覧も読み直します", glyph::kRefresh,
        L"objectarc reload refresh");
    add(L"arc-quit", L"ObjectArc を終了", L"", glyph::kCancel, L"objectarc quit exit");

    if (frameCommands && FrameRunning()) {
        for (int i = 1; i <= 9; ++i) {
            const std::wstring n = std::to_wstring(i);
            Item c;
            c.kind = Kind::Command;
            c.title = L"ワークスペース " + n;
            c.subtitle = L"ObjectFrame";
            c.target = L"of:workspace " + n;
            c.glyph = glyph::kTiles;
            c.keywords = L"workspace ws" + n + L" " + n;
            Finish(c);
            out.push_back(std::move(c));
        }
        for (const wchar_t* layout : {L"dwindle", L"master", L"monocle", L"scroll"}) {
            Item c;
            c.kind = Kind::Command;
            c.title = std::wstring(L"レイアウト: ") + layout;
            c.subtitle = L"ObjectFrame";
            c.target = std::wstring(L"of:layout ") + layout;
            c.glyph = glyph::kTiles;
            c.keywords = std::wstring(L"layout ") + layout;
            Finish(c);
            out.push_back(std::move(c));
        }
        add(L"of:togglefloat", L"フローティングの切り替え", L"ObjectFrame · 直前のウィンドウ", glyph::kPin,
            L"float floating toggle");
        add(L"of:fullscreen", L"全画面の切り替え", L"ObjectFrame · 直前のウィンドウ", glyph::kFullscreen,
            L"fullscreen zenngamen");
        add(L"of:retile", L"再タイル", L"ObjectFrame", glyph::kTiles, L"retile tile");
        add(L"of:pause", L"ObjectFrame の一時停止 / 再開", L"ObjectFrame", glyph::kPause, L"pause resume objectframe");
        add(L"of:reload", L"ObjectFrame の設定を再読み込み", L"ObjectFrame", glyph::kRefresh, L"objectframe reload");
    }
    return out;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

void History::Load(const std::wstring& path) {
    path_ = path;
    entries_.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;
    wchar_t buf[2048];
    while (fgetws(buf, 2048, f)) {
        std::wstring line = buf;
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
        auto parts = Split(line, L'\t');
        if (parts.size() != 3 || parts[2].empty()) continue;
        Entry e;
        e.count = _wtoi(parts[0].c_str());
        e.last = _wtoi64(parts[1].c_str());
        entries_[parts[2]] = e;
    }
    fclose(f);
}

void History::Record(const std::wstring& key) {
    if (key.empty()) return;
    Entry& e = entries_[key];
    ++e.count;
    e.last = Now();
    Save();
}

int History::Boost(const std::wstring& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return 0;
    const long long age = Now() - it->second.last;
    return std::min(it->second.count, 20) * 10 + (age < 86400 ? 60 : age < 7 * 86400 ? 30 : 0);
}

std::vector<std::wstring> History::Recent(size_t count) const {
    std::vector<std::pair<long long, std::wstring>> list;
    for (const auto& [key, e] : entries_) list.push_back({e.last, key});
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < list.size() && i < count; ++i) out.push_back(list[i].second);
    return out;
}

void History::Save() const {
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path_.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    for (const auto& [key, e] : entries_) fwprintf(f, L"%d\t%lld\t%ls\n", e.count, e.last, key.c_str());
    fclose(f);
}

}  // namespace oa
