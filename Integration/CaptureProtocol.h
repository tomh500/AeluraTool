#pragma once
#include <windows.h>

namespace Aelura::Capture {
inline constexpr UINT Message = WM_APP + 120;
inline constexpr wchar_t WindowClass[] = L"AeluraTool.ScreenShotPlus.Panel";
inline constexpr wchar_t ReadyProperty[] = L"ScreenShotPlus.Ready";
inline constexpr WPARAM Start = 1;
inline constexpr WPARAM Settings = 2;
inline constexpr WPARAM Preview = 3;
inline constexpr WPARAM Shutdown = 4;
inline constexpr WPARAM CanCapture = 5;
inline constexpr WPARAM SetLanguage = 6;
inline constexpr LRESULT Accepted = 1;
inline constexpr LRESULT Busy = 2;
}
