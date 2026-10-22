#include "I18n.h"
#include "FileMigration.h"
#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <fstream>
#include <array>
#include <vector>
#include <stdexcept>
#include <cwctype>
#pragma comment(lib, "Shell32.lib")
namespace aelura::migration {
using aelura::i18n::tr;
namespace {
namespace fs = std::filesystem;
struct Entry { fs::path relative; bool directory; uintmax_t size; fs::file_time_type time; };
std::wstring folded(fs::path const& p) { auto s = p.lexically_normal().wstring(); for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
bool inside(fs::path const& child, fs::path const& parent) {
    auto a = folded(child), b = folded(parent); if (a == b) return true;
    if (b.back() != L'\\') b += L'\\'; return a.compare(0, b.size(), b) == 0;
}
void require(bool ok, std::string const& message) { if (!ok) throw std::runtime_error(message); }
bool present(fs::path const& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void move(fs::path const& from, fs::path const& to, std::error_code& ec) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) ec.clear();
    else ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
}
void move(fs::path const& from, fs::path const& to) {
    std::error_code ec; move(from, to, ec); if (ec) throw fs::filesystem_error(aelura::i18n::utf8("migration.path_error"), from, to, ec);
}
struct ReadLocks {
    std::vector<HANDLE> handles;
    void clear() { for (auto h : handles) CloseHandle(h); handles.clear(); }
    ~ReadLocks() { clear(); }
    void add(fs::path const& p) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        require(h != INVALID_HANDLE_VALUE, aelura::i18n::utf8("migration.file_busy")); handles.push_back(h);
    }
};
void plain(fs::path const& p) {
    const auto attrs = GetFileAttributesW(p.c_str());
    require(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT), aelura::i18n::utf8("migration.reparse"));
}
std::vector<Entry> inventory(fs::path const& root) {
    std::vector<Entry> entries;
    auto add = [&](fs::path const& p, fs::path const& rel) {
        plain(p); bool dir = fs::is_directory(p); require(dir || fs::is_regular_file(p), aelura::i18n::utf8("migration.unsupported"));
        entries.push_back({rel, dir, dir ? 0 : fs::file_size(p), fs::last_write_time(p)});
    };
    add(root, {});
    if (fs::is_directory(root)) for (auto const& entry : fs::recursive_directory_iterator(root)) add(entry.path(), entry.path().lexically_relative(root));
    return entries;
}
bool unchanged(fs::path const& root, std::vector<Entry> const& entries) {
    auto now = inventory(root); if (now.size() != entries.size()) return false;
    for (auto const& e : entries) {
        auto p = e.relative.empty() ? root : root / e.relative;
        if (!present(p) || fs::is_directory(p) != e.directory || fs::last_write_time(p) != e.time || (!e.directory && fs::file_size(p) != e.size)) return false;
    }
    return true;
}
bool link(fs::path const& origin, fs::path const& target, bool directory) {
    if (!directory) return CreateSymbolicLinkW(origin.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    // Directory junctions work without administrator rights on local NTFS volumes.
    if (!CreateDirectoryW(origin.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(origin.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(origin.c_str()); return false; }
    struct Junction { DWORD tag; WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength; wchar_t buffer[16384]; } data{};
    std::wstring substitute = L"\\??\\" + target.wstring(), display = target.wstring();
    bool ok = substitute.size() + display.size() + 2 < std::size(data.buffer);
    if (ok) {
        data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.substituteLength = static_cast<WORD>(substitute.size() * 2);
        data.printOffset = data.substituteLength + 2; data.printLength = static_cast<WORD>(display.size() * 2);
        memcpy(data.buffer, substitute.c_str(), (substitute.size() + 1) * 2);
        memcpy(reinterpret_cast<char*>(data.buffer) + data.printOffset, display.c_str(), (display.size() + 1) * 2);
        data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + 2);
        DWORD written{}; ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, nullptr, 0, &written, nullptr) != 0;
    }
    CloseHandle(h); if (!ok) RemoveDirectoryW(origin.c_str()); return ok;
}
fs::path unique(fs::path const& parent, std::wstring const& prefix) {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error(aelura::i18n::utf8("migration.id_error"));
    wchar_t id[40]{}; StringFromGUID2(guid, id, 40); return parent / (prefix + id);
}
void journal(fs::path const& path, Plan const& p, fs::path const& backup, fs::path const& stage) {
    std::wstring text = tr("migration.journal_start") + p.source.wstring() + tr("migration.journal_target") + p.destination.wstring() +
        tr("migration.journal_backup") + backup.wstring() + tr("migration.journal_staging") + stage.wstring() + tr("migration.journal_help");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(h != INVALID_HANDLE_VALUE, aelura::i18n::utf8("migration.journal_create")); DWORD count{}; wchar_t bom = 0xfeff;
    bool ok = WriteFile(h, &bom, 2, &count, nullptr) && WriteFile(h, text.data(), static_cast<DWORD>(text.size() * 2), &count, nullptr) && count == text.size() * 2 && FlushFileBuffers(h);
    CloseHandle(h); require(ok, aelura::i18n::utf8("migration.journal_write"));
}
}
Plan inspect(fs::path source, fs::path folder) {
    require(!source.empty() && !folder.empty(), aelura::i18n::utf8("migration.choose_paths"));
    source = fs::absolute(source).lexically_normal(); folder = fs::absolute(folder).lexically_normal();
    if (source.filename().empty()) source = source.parent_path();
    for (auto p = source; !p.empty(); p = p.parent_path()) { plain(p); if (p == p.root_path()) break; }
    for (auto p = folder; !p.empty(); p = p.parent_path()) { plain(p); if (p == p.root_path()) break; }
    source = fs::canonical(source); folder = fs::canonical(folder);
    require(source.is_absolute() && !source.filename().empty() && source != source.root_path() && fs::is_directory(folder), aelura::i18n::utf8("migration.local_path"));
    require(source.wstring().rfind(L"\\\\", 0) != 0 && folder.wstring().rfind(L"\\\\", 0) != 0, aelura::i18n::utf8("migration.network"));
    wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
    require(!inside(source, windows), aelura::i18n::utf8("migration.windows"));
    for (auto known : {&FOLDERID_Profile, &FOLDERID_UserProfiles, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData, &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData}) {
        PWSTR path{}; if (SUCCEEDED(SHGetKnownFolderPath(*known, 0, nullptr, &path))) { bool root = folded(source) == folded(path); CoTaskMemFree(path); require(!root, aelura::i18n::utf8("migration.protected")); }
    }
    Plan plan{source, folder / source.filename(), fs::is_directory(source)};
    require(!inside(plan.destination, source) && !inside(source, plan.destination), aelura::i18n::utf8("migration.nested"));
    require(!present(plan.destination), aelura::i18n::utf8("migration.exists"));
    wchar_t a[32768]{}, b[32768]{};
    require(GetVolumePathNameW(source.c_str(), a, 32768) && GetVolumePathNameW(folder.c_str(), b, 32768), aelura::i18n::utf8("migration.volume"));
    plan.sameVolume = folded(a) == folded(b);
    for (auto const& e : inventory(source)) plan.bytes += e.size;
    if (!plan.sameVolume) require(fs::space(folder).available >= plan.bytes, aelura::i18n::utf8("migration.space"));
    return plan;
}
Result execute(Plan const& requested, Progress const& progress, bool copyAlways) {
    fs::path backup, stage, recovery; Plan p; bool renamed = false, committed = false, linked = false, stagingOwned = false;
    try {
        p = inspect(requested.source, requested.destination.parent_path());
        require(p.destination == requested.destination, aelura::i18n::utf8("migration.changed_target"));
        auto entries = inventory(p.source);
        ReadLocks locks;
        backup = unique(p.source.parent_path(), L".aelura-backup-"); stage = unique(p.destination.parent_path(), L".aelura-copy-");
        recovery = unique(p.source.parent_path(), L".aelura-recovery-"); recovery += L".txt";
        auto probe = unique(p.source.parent_path(), L".aelura-link-");
        require(link(probe, p.source, p.directory), aelura::i18n::utf8("migration.link_permission"));
        require(p.directory ? RemoveDirectoryW(probe.c_str()) != 0 : DeleteFileW(probe.c_str()) != 0, aelura::i18n::utf8("migration.probe_cleanup"));
        journal(recovery, p, backup, stage);
        require(progress(0, p.bytes), aelura::i18n::utf8("migration.cancelled"));
        if (p.sameVolume && !copyAlways) {
            move(p.source, p.destination); committed = true;
        } else {
            for (auto const& entry : entries) if (!entry.directory) locks.add(entry.relative.empty() ? p.source : p.source / entry.relative);
            if (p.directory) fs::create_directory(stage);
            else { HANDLE h = CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr); require(h != INVALID_HANDLE_VALUE, aelura::i18n::utf8("migration.staging")); CloseHandle(h); }
            stagingOwned = true; uintmax_t done = 0; std::vector<char> a(1024 * 1024), b(1024 * 1024);
            for (auto const& e : entries) {
                auto src = e.relative.empty() ? p.source : p.source / e.relative; auto dst = e.relative.empty() ? stage : stage / e.relative;
                if (e.directory) { fs::create_directories(dst); continue; }
                require(progress(done, p.bytes), aelura::i18n::utf8("migration.cancelled"));
                struct CopyState { Progress const* callback; uintmax_t done, total; } state{&progress, done, p.bytes};
                auto update = [](LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID value)->DWORD {
                    auto& s = *static_cast<CopyState*>(value);
                    return (*s.callback)(s.done + static_cast<uintmax_t>(transferred.QuadPart), s.total) ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
                };
                require(CopyFileExW(src.c_str(), dst.c_str(), update, &state, nullptr, e.relative.empty() ? 0 : COPY_FILE_FAIL_IF_EXISTS) != 0,
                    aelura::i18n::utf8("migration.copy_failed"));
                std::ifstream input(src, std::ios::binary), output(dst, std::ios::binary);
                require(input.good() && output.good(), aelura::i18n::utf8("migration.verify_open"));
                while (input) {
                    input.read(a.data(), a.size()); auto count = input.gcount(); output.read(b.data(), count);
                    require(output.gcount() == count && std::equal(a.begin(), a.begin() + count, b.begin()), aelura::i18n::utf8("migration.verify_mismatch"));
                    require(progress(done + static_cast<uintmax_t>(input.tellg() < 0 ? e.size : static_cast<uintmax_t>(input.tellg())), p.bytes), aelura::i18n::utf8("migration.cancelled"));
                }
                require(input.eof() && !input.bad() && output.peek() == EOF, aelura::i18n::utf8("migration.verify_read")); done += e.size;
            }
            require(unchanged(p.source, entries), aelura::i18n::utf8("migration.changed_source"));
            locks.clear();
            move(p.source, backup); renamed = true;
            for (auto const& entry : entries) if (!entry.directory) locks.add(entry.relative.empty() ? backup : backup / entry.relative);
            require(unchanged(backup, entries), aelura::i18n::utf8("migration.changed_backup"));
            move(stage, p.destination); stagingOwned = false; committed = true;
        }
        require(link(p.source, p.destination, p.directory), aelura::i18n::utf8("migration.link_failed")); linked = true;
        if (renamed) {
            if (!unchanged(backup, entries)) return {true, tr("migration.keep_backup") + recovery.wstring(), recovery};
            std::error_code cleanup; fs::remove_all(backup, cleanup);
            if (cleanup) return {true, tr("migration.cleanup_failed") + recovery.wstring(), recovery};
        }
        fs::remove(recovery); progress(p.bytes, p.bytes); return {true, tr("migration.done"), {}};
    } catch (std::exception const& ex) {
        std::error_code ec; bool restored = true;
        if (!linked) {
            if (renamed) { move(backup, p.source, ec); restored = !ec; }
            else if (committed) { move(p.destination, p.source, ec); restored = !ec; committed = !restored; }
            if (restored && renamed && committed) fs::remove_all(p.destination, ec);
            if (stagingOwned) fs::remove_all(stage, ec);
        }
        if (restored && !ec && !linked && !recovery.empty()) fs::remove(recovery, ec);
        int size = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0); std::wstring message(size > 0 ? size : 1, L'\0');
        if (size > 0) MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, message.data(), size); if (!message.empty()) message.pop_back();
        if ((!restored || linked || ec) && !recovery.empty()) message += tr("migration.recovery_prefix") + recovery.wstring();
        return {linked, message, (!restored || linked || ec) ? recovery : fs::path{}};
    }
}
}
