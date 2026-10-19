#include "../Integration/FileHash.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
using namespace aelura::hash;
void require(bool ok, char const* what) { if (!ok) throw std::runtime_error(what); }
void write(fs::path const& path, std::string const& bytes) { std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); require(out.good(), "hash fixture write failed"); }
}
void run_hash_tests(fs::path const& root) {
    fs::create_directories(root);
    using namespace aelura::hash;
    const std::wstring empty = L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::wstring abc = L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    write(root / "empty", ""); write(root / L"中文.txt", "abc"); write(root / "same", "abc"); write(root / "other", "abd");
    Request request; request.first = root / "empty";
    auto result = compute(request, {});
    require(result.status == Status::Success && result.first == empty && result.bytes == 0, "empty SHA256 mismatch");
    request.first = root / L"中文.txt"; result = compute(request, {});
    require(result.status == Status::Success && result.first == abc && result.bytes == 3, "abc or Unicode SHA256 mismatch");
    std::string binary; binary.reserve(2560000);
    for (int i = 0; i < 10000; ++i) for (int j = 0; j < 256; ++j) binary += static_cast<char>(j);
    write(root / "binary", binary); request.first = root / "binary";
    uint64_t previous = 0; unsigned reports = 0;
    result = compute(request, [&](uint64_t done, uint64_t total) {
        require(done >= previous && done <= total && (total == 0 || total == binary.size()), "hash progress invalid");
        previous = done; ++reports; return true;
    });
    require(result.status == Status::Success && result.first == L"2350b445eb01b3dc3c68b3e42b5b5678349472e2cbf75539fd75172f931ad1ee" && reports > 5, "cross-block binary hash mismatch");
    result = compute(request, [](auto done, auto) { return done == 0; });
    require(result.status == Status::Cancelled && result.first.empty(), "mid-stream cancel published digest");
    result = compute(request, [](auto, auto) { return false; });
    require(result.status == Status::Cancelled, "pre-open cancellation failed");
    unsigned finalReports = 0;
    result = compute(request, [&](auto done, auto total) { if (total && done == total) ++finalReports; return finalReports < 3; });
    require(result.status == Status::Cancelled && result.first.empty(), "final-block cancellation lost");
    // Verify totals above 4 GiB without allocating or reading gigabytes of data.
    const auto sparsePath = root / "large-sparse";
    HANDLE sparse = CreateFileW(sparsePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    require(sparse != INVALID_HANDLE_VALUE, "sparse fixture open failed");
    DWORD returned{};
    LARGE_INTEGER largeSize{}; largeSize.QuadPart = (1ll << 32) + 123;
    const bool sparseReady = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
        SetFilePointerEx(sparse, largeSize, nullptr, FILE_BEGIN) && SetEndOfFile(sparse);
    CloseHandle(sparse);
    if (sparseReady) {
        request.first = sparsePath; uint64_t observedTotal = 0;
        result = compute(request, [&](auto done, auto total) { observedTotal = total; return done == 0; });
        require(observedTotal == static_cast<uint64_t>(largeSize.QuadPart) && result.status == Status::Cancelled, "64-bit progress truncated");
    } else std::cout << "LIMIT: sparse-file progress fixture unavailable\n";
    request.mode = Mode::Expected; request.first = root / "same"; request.expected = abc;
    std::transform(request.expected.begin(), request.expected.end(), request.expected.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    request.expected = L" \t" + request.expected + L"\r\n"; result = compute(request, {});
    require(result.status == Status::Success && result.matches, "expected normalization failed");
    request.expected = empty; result = compute(request, {}); require(result.status == Status::Success && !result.matches, "expected mismatch ignored");
    for (auto const& invalid : std::array<std::wstring, 5>{L"", L"abc", L"0x" + abc, std::wstring(64, L'g'), abc.substr(0, 32) + L" " + abc.substr(32)}) {
        request.expected = invalid; result = compute(request, {}); require(result.status == Status::InvalidExpected && result.first.empty(), "invalid expected accepted");
    }
    request.mode = Mode::Compare; request.second = root / L"中文.txt"; result = compute(request, {});
    require(result.status == Status::Success && result.matches && result.bytes == 6, "same files mismatch");
    request.second = root / "other"; result = compute(request, {}); require(result.status == Status::Success && !result.matches, "different files matched");
    request.first = root / "empty"; request.second = root / "empty"; result = compute(request, {});
    require(result.status == Status::Success && result.matches && result.bytes == 0, "empty comparison failed");
    request.first = root / "same"; request.second = root / "missing"; result = compute(request, {});
    require(result.status == Status::OpenFailed && result.first.empty() && result.second.empty(), "second open failure published partial result");
    request.mode = Mode::Calculate;
    for (auto const& invalid : {fs::path(), fs::path(L"relative-file"), fs::path(L"\\\\server\\share\\file"), fs::path(L"\\\\.\\NUL"), root / L"same:stream"}) {
        request.first = invalid; result = compute(request, {}); require(result.status == Status::InvalidPath, "nonlocal or device path accepted");
    }
    request.first = root; result = compute(request, {}); require(result.status == Status::UnsupportedFile, "directory accepted");
    request.first = root / "same";
    HANDLE lock = CreateFileW(request.first.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(lock != INVALID_HANDLE_VALUE, "writer lock fixture failed"); result = compute(request, {}); CloseHandle(lock);
    require(result.status == Status::OpenFailed && result.error == ERROR_SHARING_VIOLATION, "existing writer not rejected");
    bool protectedFile = false;
    result = compute(request, [&](auto, auto total) {
        if (!total || protectedFile) return true;
        HANDLE writer = CreateFileW(request.first.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        protectedFile = writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        return true;
    });
    require(result.status == Status::Success && protectedFile, "hash did not prevent concurrent writing");
    // Change metadata through an attributes-only handle, without touching user files.
    bool touched = false;
    result = compute(request, [&](auto done, auto) {
        if (done && !touched) {
            HANDLE attributes = CreateFileW(request.first.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (attributes != INVALID_HANDLE_VALUE) {
                FILETIME time{}; GetSystemTimeAsFileTime(&time); time.dwHighDateTime += 1;
                touched = SetFileTime(attributes, nullptr, nullptr, &time) != 0; CloseHandle(attributes);
            }
        }
        return true;
    });
    require(touched && result.status == Status::Changed && result.first.empty(), "file metadata change not rejected");
    if (CreateSymbolicLinkW((root / "link").c_str(), (root / "same").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        request.first = root / "link"; result = compute(request, {});
        require(result.status == Status::UnsupportedFile, "reparse point accepted");
        require(DeleteFileW(request.first.c_str()) != 0, "test symlink cleanup failed");
    } else std::cout << "LIMIT: file symlink fixture unavailable, error=" << GetLastError() << '\n';

    Session session; Request input; input.first = root / "same";
    auto work = session.begin(input); require(work && !session.begin(input), "parallel hash task allowed");
    auto valid = compute(input, {}); session.cancel(); require(session.state() == State::Cancelling && !session.begin(input), "cancel released task too soon");
    require(session.finish(work, valid) && session.result().status == Status::Cancelled && session.result().first.empty(), "cancel lost to late success");
    work = session.begin(input); session.invalidate(); require(!session.finish(work, valid) && session.state() == State::Idle, "stale input result published");
    work = session.begin(input); require(session.finish(work, valid) && session.result().first == abc, "valid task result lost");
    auto old = work; work = session.begin(input); require(!session.finish(old, valid) && session.busy(), "old completion released new task");
    session.finish(work, valid); session.invalidate(); require(session.state() == State::Idle && session.result().first.empty(), "input edit retained old digest");
    std::ifstream unchanged(root / "same", std::ios::binary); std::string contents{std::istreambuf_iterator<char>(unchanged), {}};
    require(contents == "abc", "hash modified source bytes");
    std::cout << "PASS: SHA256 vectors, binary chunks, comparison, validation, cancellation races, locks, file changes, session invalidation\n";
}
