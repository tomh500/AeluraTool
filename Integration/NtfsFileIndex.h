#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aelura::filesearch::ntfs {
struct Record { uint64_t parent{}; std::wstring name; DWORD attributes{}; };
using Records = std::unordered_map<uint64_t, Record>;
// Reject truncated, unsupported, or malformed records before publishing any paths.
inline bool readRecords(char const* data, size_t length, Records& records) {
    size_t at = 0;
    while (at < length) {
        if (length - at < offsetof(USN_RECORD_V2, FileName)) return false;
        USN_RECORD_V2 record{};
        std::memcpy(&record, data + at, offsetof(USN_RECORD_V2, FileName));
        if (record.MajorVersion != 2 || record.RecordLength < offsetof(USN_RECORD_V2, FileName) ||
            record.RecordLength > length - at || record.FileNameOffset < offsetof(USN_RECORD_V2, FileName) ||
            record.FileNameLength == 0 || record.FileNameLength % sizeof(wchar_t) ||
            record.FileNameOffset > record.RecordLength || record.FileNameLength > record.RecordLength - record.FileNameOffset) return false;
        std::wstring name(record.FileNameLength / sizeof(wchar_t), L'\0');
        std::memcpy(name.data(), data + at + record.FileNameOffset, record.FileNameLength);
        if (name.find_first_of(L"\\/\0", 0, 3) != std::wstring::npos || name == L"..") return false;
        records[record.FileReferenceNumber] = {record.ParentFileReferenceNumber, std::move(name), record.FileAttributes};
        at += record.RecordLength;
    }
    return true;
}
inline std::wstring resolve(uint64_t id, uint64_t rootId, std::wstring const& root, Records const& records, size_t depth = 0) {
    if (id == rootId) return root;
    if (depth >= 256) return {}; // Corrupt parent cycles cannot recurse indefinitely.
    auto at = records.find(id); if (at == records.end()) return {};
    auto const& value = at->second;
    auto parent = records.find(value.parent);
    if (value.parent != rootId && (parent == records.end() || !(parent->second.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (parent->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    auto path = resolve(value.parent, rootId, root, records, depth + 1);
    if (path.empty()) return {};
    if (path.back() != L'\\') path += L'\\';
    return path + value.name;
}
struct Handle { HANDLE value{INVALID_HANDLE_VALUE}; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
// Native NTFS metadata enumeration. No elevation or external service is requested.
// Return false without emitting results when unavailable, allowing ordinary traversal.
inline bool enumerate(std::filesystem::path const& root, std::function<void(std::wstring, bool)> const& emit,
    std::function<bool()> const& cancelled) {
    auto text = root.wstring();
    if (text.size() != 3 || text[1] != L':' || text[2] != L'\\') return false;
    wchar_t format[32]{};
    if (!GetVolumeInformationW(text.c_str(), nullptr, 0, nullptr, nullptr, nullptr, format, 32) || wcscmp(format, L"NTFS")) return false;
    Handle volume; auto device = L"\\\\.\\" + text.substr(0, 2);
    volume.value = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume.value == INVALID_HANDLE_VALUE) return false;
    Handle directory; directory.value = CreateFileW(text.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (directory.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value, &info)) return false;
    const uint64_t rootId = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    MFT_ENUM_DATA_V0 request{}; request.HighUsn = MAXLONGLONG;
    std::vector<char> buffer(1024 * 1024); Records records;
    for (;;) {
        if (cancelled()) return true;
        DWORD bytes{};
        if (!DeviceIoControl(volume.value, FSCTL_ENUM_USN_DATA, &request, sizeof(request), buffer.data(),
            static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return false;
        }
        if (bytes < sizeof(uint64_t) || !readRecords(buffer.data() + sizeof(uint64_t), bytes - sizeof(uint64_t), records)) return false;
        uint64_t next{}; std::memcpy(&next, buffer.data(), sizeof(next));
        if (next <= request.StartFileReferenceNumber) return false;
        request.StartFileReferenceNumber = next;
    }
    for (auto const& [id, value] : records) {
        if (cancelled()) break;
        if (id == rootId) continue;
        auto path = resolve(id, rootId, text, records);
        if (!path.empty()) emit(std::move(path), (value.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    return true;
}
}
