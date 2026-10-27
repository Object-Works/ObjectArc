#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace oa {

constexpr const wchar_t* kWindowClass = L"ObjectArcWnd";
// ObjectFrame's own window, which accepts the same commands as `ofwm dispatch`.
constexpr const wchar_t* kFrameWindowClass = L"ObjectFrameWM";
constexpr ULONG_PTR kFrameCommandMagic = 0x4F46574D;  // 'OFWM'

std::wstring Trim(const std::wstring& s);
std::wstring ToLower(std::wstring s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t separator);
std::wstring ConfigDir();  // %USERPROFILE%\.config\objectarc
std::wstring ExpandEnv(const std::wstring& s);

bool SendFrameCommand(const std::wstring& command);
bool FrameRunning();

}  // namespace oa
