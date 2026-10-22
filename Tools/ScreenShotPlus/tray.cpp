#include "tray.h"
bool RegisterTrayIcon(TrayIcon& tray, NotifyIconCall notify) {
    // Shell_NotifyIcon only documents BOOL. LastError is a diagnostic hint.
    const auto attempt = [&](DWORD operation) {
        SetLastError(ERROR_SUCCESS);
        const bool ok = notify(operation, &tray.data) != FALSE;
        tray.diagnosticError = ok ? ERROR_SUCCESS : GetLastError();
        return ok;
    };
    const DWORD operation = tray.registered ? NIM_MODIFY : NIM_ADD;
    tray.registered = attempt(operation);
    if (!tray.registered) tray.registered = attempt(operation == NIM_ADD ? NIM_MODIFY : NIM_ADD);
    if (!tray.registered) { tray.modern = false; return false; }
    tray.data.uVersion = NOTIFYICON_VERSION_4;
    tray.modern = notify(NIM_SETVERSION, &tray.data) != FALSE;
    return true;
}
DWORD CurrentProcessIntegrity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return MAXDWORD;
    DWORD bytes = 0, level = MAXDWORD;
    alignas(TOKEN_MANDATORY_LABEL) std::array<BYTE, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> buffer{};
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
        if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
            level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    }
    CloseHandle(token); return level;
}
std::wstring TrayFailureMessage(const TrayIcon& tray) {
    if (CurrentProcessIntegrity() < SECURITY_MANDATORY_MEDIUM_RID)
        return tr("screen.tray.001");
    return tr("screen.tray.002") + std::to_wstring(tray.diagnosticError) + tr("screen.tray.003");
}
void RemoveTrayIcon(TrayIcon& tray) {
    Shell_NotifyIconW(NIM_DELETE, &tray.data); tray.registered = tray.modern = false;
}
bool TrayEventOpensPanel(LPARAM event) {
    const UINT message = LOWORD(event);
    return message == NIN_SELECT || message == NIN_KEYSELECT || message == WM_CONTEXTMENU ||
        message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_LBUTTONDBLCLK;
}
