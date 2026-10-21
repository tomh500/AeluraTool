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
struct Change { uint64_t id{}; Record value; DWORD reason{}; };
inline bool readChanges(char const* data, size_t length, std::vector<Change>& changes) {
    Records validated;
    if (!readRecords(data, length, validated)) return false;
    std::vector<Change> staged;
    for (size_t at = 0; at < length;) {
        USN_RECORD_V2 record{}; std::memcpy(&record, data + at, offsetof(USN_RECORD_V2, FileName));
        std::wstring name(record.FileNameLength / sizeof(wchar_t), L'\0');
        std::memcpy(name.data(), data + at + record.FileNameOffset, record.FileNameLength);
        staged.push_back({record.FileReferenceNumber, {record.ParentFileReferenceNumber, std::move(name), record.FileAttributes}, record.Reason});
        at += record.RecordLength;
    }
    changes.insert(changes.end(), std::make_move_iterator(staged.begin()), std::make_move_iterator(staged.end()));
    return true;
}
inline bool journalAvailable(std::filesystem::path const& root) {
    auto text = root.wstring(); if (text.size() != 3 || text[1] != L':' || text[2] != L'\\') return false;
    Handle volume; volume.value = CreateFileW((L"\\\\.\\" + text.substr(0, 2)).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    USN_JOURNAL_DATA_V0 data{}; DWORD bytes{};
    return volume.value != INVALID_HANDLE_VALUE && DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr);
}
struct Journal {
    std::wstring root; uint64_t rootId{}, journalId{}; USN next{}; Records records; bool active{};
    bool continuous(USN_JOURNAL_DATA_V0 const& current) const {
        return current.UsnJournalID == journalId && next >= current.FirstUsn && next >= current.LowestValidUsn && next <= current.NextUsn;
    }
    // Include both sides of a rename; a directory update also replaces descendant paths.
    void apply(std::vector<Change> const& changes, std::vector<std::filesystem::path>& paths) {
        for (auto const& change : changes) {
            if (change.id == rootId) continue;
            auto old = resolve(change.id, rootId, root, records);
            if (!old.empty()) paths.emplace_back(std::move(old));
            auto parent = resolve(change.value.parent, rootId, root, records);
            auto parentRecord = records.find(change.value.parent);
            const bool safeParent = change.value.parent == rootId || (parentRecord != records.end() &&
                (parentRecord->second.attributes & FILE_ATTRIBUTE_DIRECTORY) && !(parentRecord->second.attributes & FILE_ATTRIBUTE_REPARSE_POINT));
            if (safeParent && !parent.empty()) paths.emplace_back(std::filesystem::path(parent) / change.value.name);
            if (change.reason & USN_REASON_FILE_DELETE) records.erase(change.id);
            else if ((change.reason & USN_REASON_RENAME_NEW_NAME) || !(change.reason & USN_REASON_RENAME_OLD_NAME)) records[change.id] = change.value;
        }
        for (auto const& change : changes) {
            auto path = resolve(change.id, rootId, root, records);
            if (change.id != rootId && !path.empty()) paths.emplace_back(std::move(path));
        }
    }
    bool poll(std::vector<std::filesystem::path>& paths, std::function<bool()> const& cancelled) {
        if (!active) return false;
        Handle volume; volume.value = CreateFileW((L"\\\\.\\" + root.substr(0, 2)).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (volume.value == INVALID_HANDLE_VALUE) return false;
        USN_JOURNAL_DATA_V0 current{}; DWORD bytes{};
        if (!DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &current, sizeof(current), &bytes, nullptr) ||
            !continuous(current)) return false;
        std::vector<char> buffer(1024 * 1024);
        // Bounded work per idle tick; never wait for future filesystem activity.
        for (unsigned reads = 0; reads < 32 && next < current.NextUsn && !cancelled(); ++reads) {
            READ_USN_JOURNAL_DATA_V0 request{}; request.StartUsn = next; request.UsnJournalID = journalId;
            request.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME |
                USN_REASON_RENAME_NEW_NAME | USN_REASON_HARD_LINK_CHANGE | USN_REASON_REPARSE_POINT_CHANGE;
            if (!DeviceIoControl(volume.value, FSCTL_READ_USN_JOURNAL, &request, sizeof(request), buffer.data(),
                static_cast<DWORD>(buffer.size()), &bytes, nullptr) || bytes < sizeof(USN)) return false;
            USN cursor{}; std::memcpy(&cursor, buffer.data(), sizeof(cursor));
            std::vector<Change> changes;
            if (cursor <= next || !readChanges(buffer.data() + sizeof(USN), bytes - sizeof(USN), changes)) return false;
            apply(changes, paths); next = cursor;
        }
        return true;
    }
};
// Native NTFS metadata enumeration. No elevation or external service is requested.
// Return false without emitting results when unavailable, allowing ordinary traversal.
inline bool enumerate(std::filesystem::path const& root, std::function<void(std::wstring, bool)> const& emit,
    std::function<bool()> const& cancelled, Journal* journal = nullptr) {
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
    USN_JOURNAL_DATA_V0 checkpoint{}; DWORD checkpointBytes{};
    const bool track = journal && DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
        &checkpoint, sizeof(checkpoint), &checkpointBytes, nullptr);
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
    if (track) {
        journal->root = text; journal->rootId = rootId; journal->journalId = checkpoint.UsnJournalID;
        journal->next = checkpoint.NextUsn; journal->records = std::move(records); journal->active = true;
        // Replay changes during enumeration before exposing the graph. Freeze the target to avoid chasing a busy disk forever.
        USN_JOURNAL_DATA_V0 end{}; DWORD bytes{};
        if (!DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &end, sizeof(end), &bytes, nullptr)) return false;
        while (journal->next < end.NextUsn && !cancelled()) {
            std::vector<std::filesystem::path> ignored;
            if (!journal->poll(ignored, cancelled)) { journal->active = false; return false; }
        }
        records = std::move(journal->records);
    }
    for (auto const& [id, value] : records) {
        if (cancelled()) break;
        if (id == rootId) continue;
        auto path = resolve(id, rootId, text, records);
        if (!path.empty()) emit(std::move(path), (value.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    if (track) journal->records = std::move(records);
    return true;
}
}
