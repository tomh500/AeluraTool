#include "DesktopLifecycle.h"
#include "I18n.h"
#pragma comment(lib, "Shell32.lib")
namespace aelura::desktop {
TrayIcon::~TrayIcon() { Remove(); if (icon_) DestroyIcon(icon_); }
void TrayIcon::Initialize(HWND window) {
    window_ = window; taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
}
bool TrayIcon::Ensure() {
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1;
    // An Explorer restart drops existing icons; use MODIFY as a liveness check.
    if (added_ && Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
    added_ = false;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = TrayMessage; data.hIcon = icon_;
    wcsncpy_s(data.szTip, i18n::tr("app.title"), _TRUNCATE);
    if (!icon_ || !Shell_NotifyIconW(NIM_ADD, &data)) return false;
    added_ = true; data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) { Remove(); return false; }
    return true;
}
void TrayIcon::Remove() {
    if (!added_) return;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data); added_ = false;
}
void TrayIcon::Localize() {
    if (!added_) return;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, i18n::tr("app.title"), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &data);
}
UINT TrayIcon::Menu() {
    HMENU menu = CreatePopupMenu(); if (!menu) return 0;
    AppendMenuW(menu, MF_STRING, 1, i18n::tr("tray.open"));
    AppendMenuW(menu, MF_STRING, 2, i18n::tr("close.exit"));
    SetMenuDefaultItem(menu, 1, FALSE);
    POINT position{}; GetCursorPos(&position); SetForegroundWindow(window_);
    auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0); DestroyMenu(menu); return command;
}
}
