#include "sharing.h"
#include "storage.h"
#include <limits>
#include <cstdint>
using namespace std;
using namespace Gdiplus;
namespace {
bool Ordinary(const filesystem::path& path, bool directory) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}
HGLOBAL Memory(const vector<BYTE>& bytes) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size()); if (!memory) return nullptr;
    auto target = GlobalLock(memory); if (!target) { GlobalFree(memory); return nullptr; }
    memcpy(target, bytes.data(), bytes.size()); GlobalUnlock(memory); return memory;
}
}
bool BuildImageTransfer(Bitmap& image, ImageTransfer& result) {
    const UINT width = image.GetWidth(), height = image.GetHeight();
    const uint64_t size = uint64_t(width) * height * 4;
    if (!width || !height || width > INT_MAX || height > INT_MAX || size > UINT_MAX ||
        size > numeric_limits<size_t>::max() - sizeof(BITMAPV5HEADER)) return false;
    ImageTransfer transfer;
    try { transfer.dib.resize(sizeof(BITMAPINFOHEADER) + static_cast<size_t>(size)); transfer.dibV5.resize(sizeof(BITMAPV5HEADER) + static_cast<size_t>(size)); }
    catch (bad_alloc const&) { return false; }
    auto& v5 = *reinterpret_cast<BITMAPV5HEADER*>(transfer.dibV5.data());
    v5.bV5Size = sizeof(v5); v5.bV5Width = static_cast<LONG>(width); v5.bV5Height = static_cast<LONG>(height);
    v5.bV5Planes = 1; v5.bV5BitCount = 32; v5.bV5Compression = BI_BITFIELDS; v5.bV5SizeImage = static_cast<DWORD>(size);
    v5.bV5RedMask = 0x00ff0000; v5.bV5GreenMask = 0x0000ff00; v5.bV5BlueMask = 0x000000ff; v5.bV5AlphaMask = 0xff000000; v5.bV5CSType = LCS_sRGB;
    auto& dib = *reinterpret_cast<BITMAPINFOHEADER*>(transfer.dib.data());
    dib.biSize = sizeof(dib); dib.biWidth = v5.bV5Width; dib.biHeight = v5.bV5Height;
    dib.biPlanes = 1; dib.biBitCount = 32; dib.biCompression = BI_RGB; dib.biSizeImage = static_cast<DWORD>(size);
    Rect bounds(0, 0, static_cast<INT>(width), static_cast<INT>(height)); BitmapData pixels{};
    if (image.LockBits(&bounds, ImageLockModeRead, PixelFormat32bppARGB, &pixels) != Ok) return false;
    const size_t row = size_t(width) * 4;
    for (UINT y = 0; y < height; ++y) {
        const auto source = static_cast<BYTE const*>(pixels.Scan0) + ptrdiff_t(y) * pixels.Stride;
        auto alpha = transfer.dibV5.data() + sizeof(v5) + size_t(height - y - 1) * row;
        auto flat = transfer.dib.data() + sizeof(dib) + size_t(height - y - 1) * row;
        memcpy(alpha, source, row);
        for (size_t x = 0; x < row; x += 4) {
            const unsigned a = source[x + 3];
            for (unsigned c = 0; c < 3; ++c) flat[x + c] = static_cast<BYTE>((source[x + c] * a + 255 * (255 - a) + 127) / 255);
            flat[x + 3] = 255;
        }
    }
    image.UnlockBits(&pixels);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    STATSTG stat{}; HGLOBAL encoded = nullptr;
    bool ok = image.Save(stream, &png, nullptr) == Ok && SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
        stat.cbSize.QuadPart && stat.cbSize.QuadPart <= numeric_limits<size_t>::max() && SUCCEEDED(GetHGlobalFromStream(stream, &encoded));
    if (ok) {
        auto bytes = static_cast<BYTE const*>(GlobalLock(encoded));
        if (!bytes) ok = false;
        else { try { transfer.png.assign(bytes, bytes + static_cast<size_t>(stat.cbSize.QuadPart)); } catch (bad_alloc const&) { ok = false; } GlobalUnlock(encoded); }
    }
    stream->Release(); if (!ok) return false;
    result = move(transfer); return true;
}
bool CopyImageToClipboard(Bitmap& image, HWND owner) {
    ImageTransfer transfer; if (!owner || !BuildImageTransfer(image, transfer)) return false;
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    array<UINT, 3> formats{CF_DIBV5, CF_DIB, pngFormat};
    array<HGLOBAL, 3> blocks{Memory(transfer.dibV5), Memory(transfer.dib), Memory(transfer.png)};
    auto release = [&] { for (auto memory : blocks) if (memory) GlobalFree(memory); };
    if (!pngFormat || any_of(blocks.begin(), blocks.end(), [](auto block) { return !block; })) { release(); return false; }
    bool opened = false;
    for (int attempt = 0; attempt < 6 && !(opened = OpenClipboard(owner) != FALSE); ++attempt) Sleep(10);
    if (!opened) { release(); return false; }
    bool copied = false;
    if (EmptyClipboard()) for (size_t i = 0; i < blocks.size(); ++i)
        if (SetClipboardData(formats[i], blocks[i])) { blocks[i] = nullptr; copied = true; }
    CloseClipboard(); release(); return copied;
}
filesystem::path ImageShareCache() {
    error_code ec; auto temp = filesystem::temp_directory_path(ec);
    return ec ? filesystem::path{} : temp / L"ScreenShotPlus-ImageShare-v1";
}
bool SaveTemporaryImage(Bitmap& image, const filesystem::path& cache, filesystem::path& file, wstring& error) {
    error_code ec;
    if (cache.empty() || (!filesystem::create_directory(cache, ec) && !Ordinary(cache, true)) || ec) {
        error = tr("screen.share.temp_failed"); return false;
    }
    GUID id{}; wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40)) { error = tr("screen.share.temp_failed"); return false; }
    const auto folder = cache / name;
    if (!filesystem::create_directory(folder, ec) || ec) { error = tr("screen.share.temp_failed"); return false; }
    if (!SavePng(image, folder, file, error)) { filesystem::remove(folder, ec); return false; }
    // Keep the file for asynchronous drop targets even after the preview or process closes.
    return true;
}
void CleanupTemporaryImages(const filesystem::path& cache) {
    try {
    if (cache.empty() || !Ordinary(cache, true)) return;
    error_code ec; filesystem::directory_iterator entries(cache, ec);
    const auto cutoff = filesystem::file_time_type::clock::now() - chrono::hours(24);
    for (const auto& entry : entries) {
        const auto folder = entry.path(); const auto name = folder.filename().wstring(); GUID id{};
        if (name.size() != 38 || FAILED(CLSIDFromString(name.c_str(), &id)) || !Ordinary(folder, true)) continue;
        const auto time = filesystem::last_write_time(folder, ec); if (ec || time > cutoff) { ec.clear(); continue; }
        filesystem::directory_iterator children(folder, ec); if (ec) { ec.clear(); continue; }
        vector<filesystem::path> owned; bool safe = true;
        for (const auto& child : children) {
            const auto path = child.path(); const auto filename = path.filename().wstring();
            if (!Ordinary(path, false) || !filename.starts_with(L"Screenshot_") || path.extension() != L".png") { safe = false; break; }
            owned.push_back(path);
        }
        if (!safe) continue;
        for (const auto& path : owned) filesystem::remove(path, ec);
        filesystem::remove(folder, ec);
    }
    } catch (filesystem::filesystem_error const&) { /* Cache maintenance must never block capturing. */ }
}
