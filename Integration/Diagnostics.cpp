#include "Diagnostics.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <winrt/base.h>
namespace aelura::diagnostics {
void failure(std::string_view operation, HRESULT code, std::wstring_view message) noexcept {
    try {
        static std::mutex mutex; std::lock_guard lock(mutex);
        PWSTR folder{}; if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) return;
        std::filesystem::path directory(folder); CoTaskMemFree(folder); directory /= L"AeluraTool";
        std::filesystem::create_directories(directory); auto path = directory / L"diagnostics.log";
        std::error_code error;
        if (std::filesystem::file_size(path, error) > 1048576 && !error) {
            auto previous = directory / L"diagnostics.previous.log";
            MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        SYSTEMTIME time{}; GetLocalTime(&time);
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << time.wYear << '-' << time.wMonth << '-' << time.wDay << ' ' << time.wHour << ':' << time.wMinute << ':' << time.wSecond
             << ' ' << operation << " 0x" << std::hex << unsigned(code) << ' ' << winrt::to_string(message) << '\n';
    } catch (...) {}
}
}
