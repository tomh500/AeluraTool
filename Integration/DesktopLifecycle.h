#pragma once
#include <windows.h>
#include <shellapi.h>

namespace aelura::desktop {
inline constexpr UINT TrayMessage = WM_APP + 125;
// Owns the shell icon only. The window owns close policy and active file tasks.
class TrayIcon {
public:
    ~TrayIcon();
    void Initialize(HWND window);
    bool Ensure();
    void Remove();
    void Localize();
    UINT Menu();
    UINT TaskbarCreatedMessage() const { return taskbarCreated_; }
private:
    HWND window_{};
    HICON icon_{};
    UINT taskbarCreated_{};
    bool added_{};
};
}
