#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
struct ITaskDefinition;
namespace aelura::privileges {
bool elevated();
std::wstring userSid();
std::filesystem::path executable();
struct Launch { HANDLE process{}, ready{}; DWORD error{}; };
// The caller owns returned handles. Cancellation leaves the current process intact.
Launch restart(HWND owner);
Launch configureStartup(HWND owner, bool enabled);
bool startupEnabled();
bool setStartup(bool enabled);
HRESULT startupDefinition(ITaskDefinition** result); // Creates a definition, never registers it.
// Process only our fixed helper/restart arguments, before acquiring the singleton.
bool prepareLaunch(bool& startup);
bool requestAutomatic(HWND owner, std::filesystem::path const& config, bool startup = false);
}
