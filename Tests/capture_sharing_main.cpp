#include "sharing.h"
#include <iostream>
void RunImageSharingTests(std::filesystem::path const& root);
int main() {
    const auto ole = OleInitialize(nullptr); if (FAILED(ole)) return 1;
    ULONG_PTR token{}; Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { OleUninitialize(); return 1; }
    std::filesystem::path root; int result = 0;
    try {
        for (unsigned n = 0; n < 100; ++n) {
            auto candidate = std::filesystem::temp_directory_path() / (L"AeluraImageSharingTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(n));
            if (std::filesystem::create_directory(candidate)) { root = candidate; break; }
        }
        if (root.empty()) throw std::runtime_error("cannot create test sandbox");
        RunImageSharingTests(root); std::filesystem::remove_all(root);
    } catch (std::exception const& error) { std::cerr << "FAIL: " << error.what() << " (sandbox retained: " << root << ")\n"; result = 1; }
    Gdiplus::GdiplusShutdown(token); OleUninitialize(); return result;
}
