#include "FileHash.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cwctype>
#include <limits>
#include <utility>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace aelura::hash {
namespace {
struct Failure { Status status; uint32_t error; };
[[noreturn]] void fail(Status status, DWORD error = GetLastError()) { throw Failure{status, error}; }
void crypto(NTSTATUS status) { if (status < 0) throw Failure{Status::CryptoFailed, static_cast<uint32_t>(status)}; }
struct File {
    HANDLE handle{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION before{};
    uint64_t size{};
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void open(std::filesystem::path const& path) {
        auto value = path.wstring();
        if (value.empty() || value.find(L'\0') != std::wstring::npos) fail(Status::InvalidPath, ERROR_INVALID_NAME);
        if (value.size() < 3 || !iswalpha(value[0]) || value[1] != L':' || value[2] != L'\\') fail(Status::InvalidPath, ERROR_INVALID_NAME);
        std::array<wchar_t, 32768> full{};
        auto length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (!length || length >= full.size()) fail(Status::InvalidPath, ERROR_INVALID_NAME);
        value.assign(full.data(), length);
        // No UNC paths, device namespaces, alternate streams or network drives.
        if (value.size() < 3 || !iswalpha(value[0]) || value[1] != L':' || value[2] != L'\\' || value.find(L':', 2) != std::wstring::npos)
            fail(Status::InvalidPath, ERROR_INVALID_NAME);
        const auto drive = GetDriveTypeW(value.substr(0, 3).c_str());
        if (drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR) fail(Status::InvalidPath, ERROR_BAD_PATHNAME);
        handle = CreateFileW(value.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) fail(Status::OpenFailed);
        if (GetFileType(handle) != FILE_TYPE_DISK) fail(Status::UnsupportedFile, ERROR_NOT_SUPPORTED);
        if (!GetFileInformationByHandle(handle, &before)) fail(Status::ReadFailed);
        if (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) fail(Status::UnsupportedFile, ERROR_NOT_SUPPORTED);
        // A local-looking path can traverse a parent directory link onto a network
        // share. Check the opened file too, rather than trusting the input drive.
        length = GetFinalPathNameByHandleW(handle, full.data(), static_cast<DWORD>(full.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= full.size()) fail(Status::InvalidPath, ERROR_BAD_PATHNAME);
        value.assign(full.data(), length);
        if (value.compare(0, 4, L"\\\\?\\") == 0) value.erase(0, 4);
        if (value.size() < 3 || !iswalpha(value[0]) || value[1] != L':' || value[2] != L'\\' ||
            GetDriveTypeW(value.substr(0, 3).c_str()) == DRIVE_REMOTE) fail(Status::InvalidPath, ERROR_BAD_PATHNAME);
        size = (static_cast<uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
    }
    void verify() const {
        BY_HANDLE_FILE_INFORMATION after{};
        if (!GetFileInformationByHandle(handle, &after)) fail(Status::ReadFailed);
        if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber || before.nFileIndexHigh != after.nFileIndexHigh ||
            before.nFileIndexLow != after.nFileIndexLow || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
            CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0) fail(Status::Changed, ERROR_FILE_INVALID);
    }
};
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Digest {
    BCRYPT_HASH_HANDLE handle{};
    ~Digest() { if (handle) BCryptDestroyHash(handle); }
};
void report(Progress const& progress, uint64_t done, uint64_t total) {
    if (progress && !progress(done, total)) fail(Status::Cancelled, ERROR_CANCELLED);
}
std::wstring digest(File const& file, Algorithm const& algorithm, Progress const& progress, uint64_t& done, uint64_t total) {
    Digest hash;
    crypto(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buffer(1024 * 1024);
    uint64_t read = 0;
    while (true) {
        report(progress, done, total);
        DWORD count{};
        if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) fail(Status::ReadFailed);
        if (!count) break;
        if (read > file.size || count > file.size - read) fail(Status::Changed, ERROR_FILE_INVALID);
        crypto(BCryptHashData(hash.handle, buffer.data(), count, 0));
        read += count; done += count;
        report(progress, done, total);
    }
    if (read != file.size) fail(Status::Changed, ERROR_FILE_INVALID);
    file.verify();
    std::array<unsigned char, 32> bytes{};
    crypto(BCryptFinishHash(hash.handle, bytes.data(), static_cast<ULONG>(bytes.size()), 0));
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring value; value.reserve(64);
    for (auto byte : bytes) { value += hex[byte >> 4]; value += hex[byte & 15]; }
    return value;
}
}

bool normalizeExpected(std::wstring value, std::wstring& normalized) {
    normalized.clear();
    auto start = value.find_first_not_of(L" \t\r\n\v\f");
    if (start == std::wstring::npos) return false;
    value = value.substr(start, value.find_last_not_of(L" \t\r\n\v\f") - start + 1);
    if (value.size() != 64) return false;
    for (auto& c : value) {
        if (c >= L'A' && c <= L'F') c += L'a' - L'A';
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    normalized = std::move(value); return true;
}
Result compute(Request const& request, Progress const& progress) {
    Result result;
    try {
        std::wstring expected;
        if (request.mode == Mode::Expected && !normalizeExpected(request.expected, expected)) fail(Status::InvalidExpected, ERROR_INVALID_DATA);
        report(progress, 0, 0);
        File first, second; first.open(request.first);
        if (request.mode == Mode::Compare) second.open(request.second);
        if (first.size > (std::numeric_limits<uint64_t>::max)() - second.size) fail(Status::ReadFailed, ERROR_ARITHMETIC_OVERFLOW);
        const auto total = first.size + second.size;
        report(progress, 0, total);
        Algorithm algorithm; crypto(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        result.first = digest(first, algorithm, progress, result.bytes, total);
        if (request.mode == Mode::Compare) result.second = digest(second, algorithm, progress, result.bytes, total);
        first.verify(); if (request.mode == Mode::Compare) second.verify();
        report(progress, result.bytes, total);
        result.matches = request.mode == Mode::Expected ? result.first == expected : request.mode == Mode::Compare && result.first == result.second;
    } catch (Failure const& failure) {
        result.status = failure.status; result.error = failure.error; result.first.clear(); result.second.clear(); result.matches = false;
    } catch (...) {
        result = {}; result.status = Status::ReadFailed; result.error = ERROR_UNHANDLED_EXCEPTION;
    }
    return result;
}
std::shared_ptr<Work> Session::begin(Request request) {
    if (busy()) return {};
    auto work = std::make_shared<Work>(); work->generation = ++generation_; work->request = std::move(request);
    result_ = {}; state_ = State::Running; work_ = work; return work;
}
void Session::invalidate() {
    ++generation_; result_ = {};
    if (!busy()) { state_ = State::Idle; work_.reset(); }
}
void Session::cancel() {
    if (busy()) { work_->cancel = true; state_ = State::Cancelling; result_ = {}; }
}
bool Session::finish(std::shared_ptr<Work> const& work, Result result) {
    if (work_ != work || !busy()) return false;
    if (work->generation != generation_) { result_ = {}; state_ = State::Idle; work_.reset(); return false; }
    if (work->cancel) { result = {}; result.status = Status::Cancelled; result.error = ERROR_CANCELLED; }
    result_ = std::move(result); state_ = State::Finished; return true;
}
}
