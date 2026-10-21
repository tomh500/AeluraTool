#include "FileShellActions.h"
#include "FileHash.h"
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/implements.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <map>
#include <type_traits>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
namespace aelura::files {
using Microsoft::WRL::ComPtr;
namespace {
struct OleScope { HRESULT result{OleInitialize(nullptr)}; ~OleScope() { if (SUCCEEDED(result)) OleUninitialize(); } };
bool within(Path const& value, Path const& parent) {
    auto path = value.lexically_normal().wstring(), root = parent.lexically_normal().wstring();
    while (root.size() > 3 && root.back() == L'\\') root.pop_back();
    return path.size() >= root.size() && CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        (path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\');
}
bool reparseAncestor(Path path) {
    for (; !path.empty(); path = path.parent_path()) {
        auto attributes = GetFileAttributesW(path.c_str()); if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        if (path == path.root_path() || path == path.parent_path()) break;
    }
    return false;
}
HRESULT effect(IDataObject* object, wchar_t const* format, DWORD value) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)); if (!memory) return E_OUTOFMEMORY;
    auto data = GlobalLock(memory); if (!data) { GlobalFree(memory); return E_OUTOFMEMORY; }
    std::memcpy(data, &value, sizeof(value)); GlobalUnlock(memory);
    FORMATETC type{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(format)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{}; medium.tymed = TYMED_HGLOBAL; medium.hGlobal = memory;
    auto hr = object->SetData(&type, &medium, TRUE); if (FAILED(hr)) ReleaseStgMedium(&medium); return hr;
}
std::vector<Path> dropped(HGLOBAL memory) {
    std::vector<Path> result; auto size = GlobalSize(memory); if (size < sizeof(DROPFILES) + 2) return result;
    auto bytes = static_cast<char const*>(GlobalLock(memory)); if (!bytes) return result;
    DROPFILES header{}; std::memcpy(&header, bytes, sizeof(header));
    auto parse = [&](auto const* text, size_t count) {
        size_t at = 0;
        while (at < count && text[at]) {
            auto end = at; while (end < count && text[end]) ++end;
            if (end == count || end - at > 32767) { result.clear(); return; }
            using Character = std::remove_cv_t<std::remove_reference_t<decltype(*text)>>;
            if constexpr (std::is_same_v<Character, wchar_t>) result.emplace_back(std::wstring(text + at, text + end));
            else { auto length = MultiByteToWideChar(CP_ACP, 0, text + at, static_cast<int>(end - at), nullptr, 0); std::wstring value(length, L'\0'); if (length) MultiByteToWideChar(CP_ACP, 0, text + at, static_cast<int>(end - at), value.data(), length); result.emplace_back(value); }
            at = end + 1;
        }
        if (at >= count) result.clear(); // Require a bounded double-NUL list terminator.
    };
    if (header.pFiles >= sizeof(DROPFILES) && header.pFiles < size) {
        if (header.fWide && header.pFiles % sizeof(wchar_t) == 0) parse(reinterpret_cast<wchar_t const*>(bytes + header.pFiles), (size - header.pFiles) / sizeof(wchar_t));
        else if (!header.fWide) parse(bytes + header.pFiles, size - header.pFiles);
    }
    GlobalUnlock(memory); return result;
}
std::wstring itemPath(IShellItem* item) {
    PWSTR text{}; if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) return {};
    std::wstring value(text); CoTaskMemFree(text); return value;
}
std::string utf8(Path const& path) {
    auto value = path.wstring(); auto length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(length, '\0'); if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), length, nullptr, nullptr); return text;
}
struct Job {
    std::mutex mutex; State status; std::atomic<bool> stop{false}; std::thread worker; std::vector<Path> movedClipboard;
    ~Job() { stop = true; if (worker.joinable()) worker.join(); }
};
Job& job() { static Job instance; return instance; }
std::atomic<unsigned> dialogs{0};
class Sink final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink> {
public:
    std::vector<Path> changed; HRESULT error{S_OK};
    std::map<Path, Path> copies;
    HRESULT check() { return job().stop ? E_ABORT : S_OK; }
    HRESULT post(HRESULT hr, IShellItem* created) { if (FAILED(hr) && SUCCEEDED(error)) error = hr; auto path = itemPath(created); if (!path.empty()) changed.emplace_back(path); return check(); }
    IFACEMETHODIMP StartOperations() override { return check(); }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { if (FAILED(hr)) error = hr; return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return check(); }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override { return post(hr, item); }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return check(); }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override { return post(hr, item); }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return check(); }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* source, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override { auto old = itemPath(source), target = itemPath(item); if (SUCCEEDED(hr) && !old.empty() && !target.empty()) copies.emplace(old, target); return post(hr, item); }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return check(); }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hr, IShellItem*) override { return post(hr, nullptr); }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return check(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr, IShellItem* item) override { return post(hr, item); }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT complete) override { std::lock_guard lock(job().mutex); job().status.total = total; job().status.completed = complete; return check(); }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};
struct ReadGuard { HANDLE handle{INVALID_HANDLE_VALUE}; ~ReadGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
struct Manifest { Path source; std::vector<Path> files, directories; std::vector<std::unique_ptr<ReadGuard>> guards; };
HRESULT manifest(Path const& source, Manifest& result) {
    result.source = source;
    auto remember = [&](Path const& file) {
        auto guard = std::make_unique<ReadGuard>();
        guard->handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (guard->handle == INVALID_HANDLE_VALUE) return false;
        result.files.push_back(file); result.guards.push_back(std::move(guard)); return true;
    };
    if (!(GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) return remember(source) ? S_OK : E_ACCESSDENIED;
    result.directories.push_back(source); std::error_code error;
    for (std::filesystem::recursive_directory_iterator at(source, error), end; at != end; at.increment(error)) {
        if (error || job().stop) return job().stop ? E_ABORT : E_ACCESSDENIED;
        auto flags = GetFileAttributesW(at->path().c_str()); if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return E_ACCESSDENIED;
        if (flags & FILE_ATTRIBUTE_DIRECTORY) result.directories.push_back(at->path()); else if (!remember(at->path())) return E_ACCESSDENIED;
    }
    return error ? E_ACCESSDENIED : S_OK;
}
bool sameVolume(Path const& first, Path const& second) {
    wchar_t a[MAX_PATH]{}, b[MAX_PATH]{}, va[MAX_PATH]{}, vb[MAX_PATH]{};
    return GetVolumePathNameW(first.c_str(), a, MAX_PATH) && GetVolumePathNameW(second.c_str(), b, MAX_PATH) &&
        GetVolumeNameForVolumeMountPointW(a, va, MAX_PATH) && GetVolumeNameForVolumeMountPointW(b, vb, MAX_PATH) && !_wcsicmp(va, vb);
}
HRESULT verify(Manifest const& source, Path const& destination) {
    for (auto const& directory : source.directories) {
        auto target = directory == source.source ? destination : destination / directory.lexically_relative(source.source);
        auto attributes = GetFileAttributesW(target.c_str()); if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return E_FAIL;
    }
    for (auto const& file : source.files) {
        if (job().stop) return E_ABORT;
        auto target = file == source.source ? destination : destination / file.lexically_relative(source.source);
        auto result = aelura::hash::compute({aelura::hash::Mode::Compare, file, target, {}}, [](auto, auto) { return !job().stop.load(); });
        if (result.status == aelura::hash::Status::Cancelled) return E_ABORT;
        if (result.status != aelura::hash::Status::Success || !result.matches) return E_FAIL;
    }
    if (!source.directories.empty()) {
        size_t files = 0, directories = 1; std::error_code error;
        for (std::filesystem::recursive_directory_iterator at(source.source, error), end; at != end; at.increment(error)) {
            if (error || job().stop) return job().stop ? E_ABORT : E_FAIL;
            auto attributes = GetFileAttributesW(at->path().c_str()); if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return E_FAIL;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) ++directories; else ++files;
        }
        if (error || files != source.files.size() || directories != source.directories.size()) return E_FAIL;
    }
    return S_OK;
}
HRESULT recovery(Request const& request, Path& path) {
    auto directory = request.recoveryFolder;
    if (directory.empty()) { PWSTR local{}; auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local); if (FAILED(hr)) return hr; directory = Path(local) / L"AeluraTool" / L"FileOperations"; CoTaskMemFree(local); }
    std::error_code error; std::filesystem::create_directories(directory, error); if (error) return E_ACCESSDENIED;
    wchar_t temporary[MAX_PATH]{}; if (!GetTempFileNameW(directory.c_str(), L"AEL", 0, temporary)) return HRESULT_FROM_WIN32(GetLastError());
    path = temporary; std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "AeluraTool verified move recovery\nDestination\t" << utf8(request.destination) << "\n";
    for (auto const& source : request.sources) output << "Source\t" << utf8(source) << "\n";
    output.flush(); if (!output.good()) return E_FAIL;
    { std::lock_guard lock(job().mutex); job().status.recovery = path; }
    return S_OK;
}
HRESULT shortcut(Request const& request, std::vector<Path>& changed) {
    ComPtr<IShellLinkW> link; auto hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)); if (FAILED(hr)) return hr;
    if (FAILED(hr = link->SetPath(request.sources[0].c_str()))) return hr;
    link->SetWorkingDirectory(request.sources[0].parent_path().c_str());
    ComPtr<IPersistFile> file; if (FAILED(hr = link.As(&file))) return hr;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(request.destination.c_str(), L"AEL", 0, temporary)) return HRESULT_FROM_WIN32(GetLastError());
    hr = file->Save(temporary, TRUE);
    if (SUCCEEDED(hr)) {
        auto name = request.sources[0].filename().wstring();
        for (unsigned i = 0; i < 1000; ++i) {
            if (job().stop) { hr = E_ABORT; break; }
            auto target = request.destination / (name + (i ? L" (" + std::to_wstring(i + 1) + L")" : L"") + L".lnk");
            if (MoveFileExW(temporary, target.c_str(), MOVEFILE_WRITE_THROUGH)) { changed.push_back(target); hr = S_OK; break; }
            hr = HRESULT_FROM_WIN32(GetLastError()); if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    DeleteFileW(temporary); return hr;
}
HRESULT perform(Request const& request, std::vector<Path>& changed, bool& aborted) {
    auto hr = preflight(request, [] { return job().stop.load(); }); if (FAILED(hr)) return hr;
    if (request.kind == Kind::Shortcut) return shortcut(request, changed);
    const bool verifiedMove = request.kind == Kind::Paste && request.move && (request.verifyMove || std::any_of(request.sources.begin(), request.sources.end(), [&](auto const& source) { return !sameVolume(source, request.destination); }));
    std::vector<Manifest> originals; Path journal;
    if (verifiedMove) {
        if (FAILED(hr = recovery(request, journal))) return hr;
        for (auto const& source : request.sources) { originals.emplace_back(); if (FAILED(hr = manifest(source, originals.back()))) return hr; }
    }
    auto sink = Microsoft::WRL::Make<Sink>(); ComPtr<IFileOperation> operation;
    if (FAILED(hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation)))) return hr;
    DWORD flags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_WANTNUKEWARNING | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE;
    if (request.kind == Kind::Recycle) flags |= FOFX_RECYCLEONDELETE;
    if (!request.interactive) flags |= FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    if (FAILED(hr = operation->SetOperationFlags(flags)) || FAILED(hr = operation->SetOwnerWindow(request.owner))) return hr;
    DWORD cookie{}; if (FAILED(hr = operation->Advise(sink.Get(), &cookie))) return hr;
    ComPtr<IShellItem> destination;
    if (request.kind == Kind::Paste && FAILED(hr = SHCreateItemFromParsingName(request.destination.c_str(), nullptr, IID_PPV_ARGS(&destination)))) return hr;
    for (auto const& source : request.sources) {
        ComPtr<IShellItem> item; if (FAILED(hr = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item)))) return hr;
        if (request.kind == Kind::Rename) hr = operation->RenameItem(item.Get(), request.name.c_str(), nullptr);
        else if (request.kind == Kind::Recycle) hr = operation->DeleteItem(item.Get(), nullptr);
        else if (request.move && !verifiedMove) hr = operation->MoveItem(item.Get(), destination.Get(), nullptr, nullptr);
        else hr = operation->CopyItem(item.Get(), destination.Get(), nullptr, nullptr);
        if (FAILED(hr)) return hr;
    }
    hr = operation->PerformOperations(); BOOL stopped{}; operation->GetAnyOperationsAborted(&stopped); aborted = stopped != FALSE;
    operation->Unadvise(cookie); changed = std::move(sink->changed);
    if (request.kind != Kind::Paste || request.move) changed.insert(changed.end(), request.sources.begin(), request.sources.end());
    if (SUCCEEDED(hr) && FAILED(sink->error)) hr = sink->error;
    if (verifiedMove) {
        { std::ofstream output(journal, std::ios::app); for (auto const& [source, target] : sink->copies) output << "Copied\t" << utf8(source) << "\t" << utf8(target) << "\n"; output.flush(); if (!output.good() && SUCCEEDED(hr)) hr = E_FAIL; }
        if (FAILED(hr) || aborted || job().stop) return job().stop ? E_ABORT : hr;
        for (auto const& original : originals) {
            auto target = sink->copies.find(original.source); if (target == sink->copies.end()) return E_FAIL;
            if (FAILED(hr = verify(original, target->second))) return hr;
        }
        // Only after all copied content is verified do we remove the originals.
        // Recycling retains an additional recovery copy; failure leaves both sides.
        ComPtr<IFileOperation> remove;
        if (FAILED(hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&remove)))) return hr;
        if (FAILED(hr = remove->SetOperationFlags(flags | FOFX_RECYCLEONDELETE)) || FAILED(hr = remove->SetOwnerWindow(request.owner))) return hr;
        auto removal = Microsoft::WRL::Make<Sink>(); DWORD removalCookie{}; remove->Advise(removal.Get(), &removalCookie);
        for (auto const& source : request.sources) { ComPtr<IShellItem> item; if (FAILED(hr = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item))) || FAILED(hr = remove->DeleteItem(item.Get(), nullptr))) return hr; }
        hr = remove->PerformOperations(); BOOL stopped{}; remove->GetAnyOperationsAborted(&stopped); aborted = stopped != FALSE; remove->Unadvise(removalCookie);
        if (SUCCEEDED(hr) && FAILED(removal->error)) hr = removal->error;
        if (SUCCEEDED(hr) && !aborted && !job().stop) { DeleteFileW(journal.c_str()); std::lock_guard lock(job().mutex); job().status.recovery.clear(); }
    }
    return hr;
}
}
bool validName(std::wstring const& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') return false;
    for (auto c : name) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    auto stem = name.substr(0, name.find(L'.')); std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
    return stem != L"CON" && stem != L"PRN" && stem != L"AUX" && stem != L"NUL" &&
        !(stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) && stem.back() >= L'1' && stem.back() <= L'9');
}
bool protectedPath(Path const& path) {
    if (!path.is_absolute() || path.lexically_normal() == path.root_path()) return true;
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH); if (*windows && within(path, windows)) return true;
    for (auto id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramData}) {
        PWSTR text{}; if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &text))) { bool blocked = within(path, text); CoTaskMemFree(text); if (blocked) return true; }
    }
    return false;
}
HRESULT preflight(Request const& request, std::function<bool()> const& cancelled) {
    if (request.sources.empty() || (request.kind != Kind::Paste && request.sources.size() != 1)) return E_INVALIDARG;
    if (request.kind == Kind::Rename && !validName(request.name)) return E_INVALIDARG;
    if (request.kind == Kind::Paste || request.kind == Kind::Shortcut) {
        auto attributes = GetFileAttributesW(request.destination.c_str());
        if (!request.destination.is_absolute() || attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return E_INVALIDARG;
        if (protectedPath(request.destination)) return E_ACCESSDENIED;
        if (reparseAncestor(request.destination)) return E_ACCESSDENIED;
    }
    for (auto const& source : request.sources) {
        if (cancelled && cancelled()) return E_ABORT;
        auto attributes = GetFileAttributesW(source.c_str());
        if (!source.is_absolute() || attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if (request.kind != Kind::Shortcut && reparseAncestor(source.parent_path())) return E_ACCESSDENIED;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && request.kind != Kind::Shortcut) return E_ACCESSDENIED;
        if ((request.kind == Kind::Rename || request.kind == Kind::Recycle || request.move) && protectedPath(source)) return E_ACCESSDENIED;
        if (request.kind == Kind::Paste && ((attributes & FILE_ATTRIBUTE_DIRECTORY) && within(request.destination, source))) return E_INVALIDARG;
        if (request.kind == Kind::Paste && request.move && source.parent_path().lexically_normal() == request.destination.lexically_normal()) return E_INVALIDARG;
        auto originalName = source.filename().wstring();
        if (request.kind == Kind::Rename && CompareStringOrdinal(originalName.c_str(), -1, request.name.c_str(), -1, TRUE) != CSTR_EQUAL && GetFileAttributesW((source.parent_path() / request.name).c_str()) != INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        // Shell copy/move/delete must never recursively traverse junctions or links.
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && request.kind != Kind::Shortcut) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || source.lexically_normal() == source.root_path()) return E_ACCESSDENIED;
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator at(source, std::filesystem::directory_options::none, error), end; at != end; at.increment(error)) {
                if (error) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
                if (cancelled && cancelled()) return E_ABORT;
                auto flags = GetFileAttributesW(at->path().c_str());
                if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return E_ACCESSDENIED;
            }
            if (error) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
    }
    return S_OK;
}
HRESULT dataObject(Path const& source, bool cut, IDataObject** result) {
    *result = nullptr; PIDLIST_ABSOLUTE item{}; auto hr = SHParseDisplayName(source.c_str(), nullptr, &item, 0, nullptr); if (FAILED(hr)) return hr;
    ComPtr<IShellFolder> parent; PCUITEMID_CHILD child{};
    hr = SHBindToParent(item, IID_PPV_ARGS(&parent), &child);
    ComPtr<IDataObject> data; if (SUCCEEDED(hr)) hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr, &data);
    CoTaskMemFree(item); if (SUCCEEDED(hr)) hr = effect(data.Get(), CFSTR_PREFERREDDROPEFFECT, cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
    if (SUCCEEDED(hr)) *result = data.Detach(); return hr;
}
Transfer readTransfer(IDataObject* object) {
    Transfer transfer; if (!object) return transfer;
    FORMATETC type{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
    if (FAILED(object->GetData(&type, &medium))) return transfer;
    if (medium.tymed == TYMED_HGLOBAL) transfer.paths = dropped(medium.hGlobal);
    ReleaseStgMedium(&medium); type.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    if (SUCCEEDED(object->GetData(&type, &medium))) {
        if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(DWORD)) { auto value = static_cast<DWORD*>(GlobalLock(medium.hGlobal)); if (value) { transfer.move = (*value & DROPEFFECT_MOVE) != 0; GlobalUnlock(medium.hGlobal); } }
        ReleaseStgMedium(&medium);
    }
    return transfer;
}
HRESULT clipboard(Path const& source, bool cut) {
    OleScope apartment; if (FAILED(apartment.result)) return apartment.result;
    ComPtr<IDataObject> data; auto hr = dataObject(source, cut, &data); if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = OleSetClipboard(data.Get()))) hr = OleFlushClipboard(); return hr;
}
Request pasteRequest(Path const& destination, HWND owner) {
    Request request; request.kind = Kind::Paste; request.destination = destination; request.owner = owner;
    OleScope apartment; if (FAILED(apartment.result)) return request;
    ComPtr<IDataObject> data;
    if (SUCCEEDED(OleGetClipboard(&data))) {
        auto transfer = readTransfer(data.Get()); request.sources = std::move(transfer.paths); request.move = transfer.move;
    }
    return request;
}
bool properties(Path const& path, HWND owner) {
    SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_INVOKEIDLIST; info.hwnd = owner; info.lpVerb = L"properties"; info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}
State state() { std::lock_guard lock(job().mutex); return job().status; }
bool active() { return dialogs != 0 || state().busy; }
void dialog(bool open) { if (open) ++dialogs; else --dialogs; }
void cancel() { job().stop = true; }
void completeClipboardMove(HWND owner) {
    std::lock_guard lock(job().mutex); if (job().movedClipboard.empty() || !OpenClipboard(owner)) return;
    auto effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    auto memory = GetClipboardData(effectFormat); auto value = memory ? static_cast<DWORD*>(GlobalLock(memory)) : nullptr;
    bool cut = value && GlobalSize(memory) >= sizeof(DWORD) && (*value & DROPEFFECT_MOVE); if (value) GlobalUnlock(memory);
    auto paths = dropped(GetClipboardData(CF_HDROP));
    bool same = cut && paths == job().movedClipboard;
    if (same) EmptyClipboard(); CloseClipboard(); job().movedClipboard.clear();
}
bool start(Request request) {
    auto& work = job();
    { std::lock_guard lock(work.mutex); if (work.status.busy || dialogs != 0) return false; work.status.busy = true; work.status.failed = work.status.cancelled = false; work.status.completed = work.status.total = 0; work.status.recovery.clear(); ++work.status.revision; }
    if (work.worker.joinable()) work.worker.join(); work.stop = false;
    try {
        work.worker = std::thread([request = std::move(request)] {
            HRESULT initialized = OleInitialize(nullptr), hr = initialized; bool aborted = false; std::vector<Path> changed;
            try { if (SUCCEEDED(initialized)) hr = perform(request, changed, aborted); }
            catch (...) { hr = E_FAIL; }
            try { if (request.index && !changed.empty()) request.index->updatePaths(std::move(changed)); } catch (...) { hr = E_OUTOFMEMORY; }
            if (SUCCEEDED(initialized)) OleUninitialize();
            auto& work = job(); std::lock_guard lock(work.mutex); work.status.busy = false;
            work.status.cancelled = aborted || work.stop || hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
            work.status.failed = FAILED(hr) && !work.status.cancelled; ++work.status.revision;
            if (!work.status.cancelled && !work.status.failed && request.kind == Kind::Paste && request.move) work.movedClipboard = request.sources;
        });
    } catch (...) { std::lock_guard lock(work.mutex); work.status.busy = false; work.status.failed = true; ++work.status.revision; return false; }
    return true;
}
}
