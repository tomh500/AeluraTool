#include "app.h"
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR args, int) {
    if (std::wstring(args) != L"--aelura-host") {
        MessageBoxW(nullptr, tr("screen.screenshotplus.001"), tr("screen.screenshotplus.002"), MB_OK | MB_ICONINFORMATION); return 1;
    }
    const HRESULT ole = OleInitialize(nullptr); if (FAILED(ole)) return 2;
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { OleUninitialize(); return 3; }
    int result = RunApp(instance, {}, true, L"AeluraTool.Capture.DisabledStartup");
    Gdiplus::GdiplusShutdown(token); OleUninitialize(); return result;
}