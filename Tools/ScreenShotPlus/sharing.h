#pragma once
#include "framework.h"
struct ImageTransfer {
    std::vector<BYTE> dib, dibV5, png;
};
bool BuildImageTransfer(Gdiplus::Bitmap& image, ImageTransfer& result);
bool CopyImageToClipboard(Gdiplus::Bitmap& image, HWND owner);
std::filesystem::path ImageShareCache();
bool SaveTemporaryImage(Gdiplus::Bitmap& image, const std::filesystem::path& cache,
    std::filesystem::path& file, std::wstring& error);
void CleanupTemporaryImages(const std::filesystem::path& cache);
