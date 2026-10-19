#include "sharing.h"
#include "dragdrop.h"
#include <stdexcept>
#include <iostream>
using namespace std;
using namespace Gdiplus;
void RunImageSharingTests(filesystem::path const& root) {
    auto check = [](bool value, char const* why) { if (!value) throw runtime_error(why); };
    filesystem::create_directories(root);
    Bitmap image(3, 2, PixelFormat32bppARGB);
    image.SetPixel(0, 0, Color(255, 210, 30, 10)); image.SetPixel(1, 0, Color(128, 40, 80, 120));
    image.SetPixel(2, 0, Color(0, 0, 0, 0)); image.SetPixel(0, 1, Color(255, 7, 11, 13));
    image.SetPixel(1, 1, Color(255, 21, 22, 23)); image.SetPixel(2, 1, Color(255, 31, 32, 33));
    ImageTransfer transfer; check(BuildImageTransfer(image, transfer), "build in-memory image formats");
    auto v5 = reinterpret_cast<BITMAPV5HEADER const*>(transfer.dibV5.data());
    check(v5->bV5Width == 3 && v5->bV5Height == 2 && v5->bV5AlphaMask == 0xff000000, "DIBV5 dimensions and alpha mask");
    auto pixels = transfer.dibV5.data() + sizeof(BITMAPV5HEADER);
    check(pixels[0] == 13 && pixels[1] == 11 && pixels[2] == 7 && pixels[3] == 255, "DIB bottom-up BGRA row order");
    check(pixels[12 + 4 + 3] == 128, "DIBV5 preserves alpha");
    auto flat = transfer.dib.data() + sizeof(BITMAPINFOHEADER);
    check(flat[12 + 8] == 255 && flat[12 + 9] == 255 && flat[12 + 10] == 255, "legacy DIB composites transparency onto white");
    IStream* stream = nullptr; check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "PNG stream");
    ULONG written{}; check(SUCCEEDED(stream->Write(transfer.png.data(), static_cast<ULONG>(transfer.png.size()), &written)) && written == transfer.png.size(), "PNG bytes");
    LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    {
        Bitmap decoded(stream); check(decoded.GetLastStatus() == Ok && decoded.GetWidth() == 3 && decoded.GetHeight() == 2, "clipboard PNG decodes");
        Color actual; decoded.GetPixel(1, 0, &actual); check(actual.GetA() == 128 && actual.GetR() == 40, "PNG preserves semitransparent pixels");
    }
    stream->Release();
    image.SetPixel(0, 1, Color(255, 99, 98, 97)); check(pixels[0] == 13, "clipboard snapshot survives later bitmap changes");
    const auto cache = root / L"share-cache";
    filesystem::path first, second, recent; wstring error;
    check(SaveTemporaryImage(image, cache, first, error), "unsaved image creates a temporary drag file");
    check(SaveTemporaryImage(image, cache, second, error) && first != second, "sharing never overwrites an existing image");
    check(SaveTemporaryImage(image, cache, recent, error), "recent sharing file");
    {
        Bitmap decoded(first.c_str()); check(decoded.GetLastStatus() == Ok && decoded.GetWidth() == 3 && decoded.GetHeight() == 2, "temporary drag PNG has original dimensions");
    }
    IDataObject* data = nullptr; check(SUCCEEDED(CreateFileDataObject(first, &data)), "temporary PNG uses normal file-sharing data object");
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
    check(SUCCEEDED(data->GetData(&format, &medium)), "drag target obtains CF_HDROP for an unsaved image");
    wchar_t path[32768]{}; DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0, path, 32768);
    check(filesystem::path(path) == first, "drag target receives the exact temporary PNG");
    ReleaseStgMedium(&medium); data->Release();
    check(filesystem::exists(first), "drop source release keeps the file available to asynchronous receivers");
    const auto old = filesystem::file_time_type::clock::now() - chrono::hours(25);
    filesystem::last_write_time(first.parent_path(), old); filesystem::last_write_time(second.parent_path(), old);
    ofstream(second.parent_path() / L"unrelated.txt") << "keep";
    filesystem::last_write_time(second.parent_path(), old);
    CleanupTemporaryImages(cache);
    check(!filesystem::exists(first.parent_path()), "expired owned temporary image is cleaned");
    check(filesystem::exists(second) && filesystem::exists(second.parent_path() / L"unrelated.txt"), "cache cleanup preserves directories containing unrelated files");
    check(filesystem::exists(recent), "recent image survives cache maintenance");
    const auto blocked = root / L"not-a-directory"; ofstream(blocked) << "keep";
    filesystem::path rejected; check(!SaveTemporaryImage(image, blocked, rejected, error) && filesystem::is_regular_file(blocked), "unavailable cache reports failure without overwriting files");
    cout << "PASS: image PNG/DIB/DIBV5 pixels, alpha and snapshot; unsaved drag CF_HDROP, unique cache, receiver lifetime and safe expiry\n";
}
