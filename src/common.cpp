#include "common.h"

#include <cwctype>

namespace oa {

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(separator, start);
        if (end == std::wstring::npos) end = s.size();
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::wstring ConfigDir() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? home : L".";
    return base + L"\\.config\\objectarc";
}

std::wstring ExpandEnv(const std::wstring& s) {
    std::wstring in = s;
    if (in.size() >= 1 && in[0] == L'~' && (in.size() == 1 || in[1] == L'\\' || in[1] == L'/')) {
        in = L"%USERPROFILE%" + in.substr(1);
    }
    DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (n == 0) return in;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(in.c_str(), out.data(), n);
    out.resize(n - 1);
    return out;
}

bool SendFrameCommand(const std::wstring& command) {
    HWND frame = FindWindowW(kFrameWindowClass, nullptr);
    if (!frame) return false;
    std::wstring buf = command;
    COPYDATASTRUCT cds{};
    cds.dwData = kFrameCommandMagic;
    cds.cbData = (DWORD)((buf.size() + 1) * sizeof(wchar_t));
    cds.lpData = buf.data();
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(frame, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 1000,
                               &result) &&
           result;
}

bool FrameRunning() { return FindWindowW(kFrameWindowClass, nullptr) != nullptr; }

}  // namespace oa
