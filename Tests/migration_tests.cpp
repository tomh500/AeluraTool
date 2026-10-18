#include "../Integration/FileMigration.h"
#include "../Integration/HotkeyManager.h"
#include "../Integration/ToolCatalog.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
void check(bool ok, char const* what) { if (!ok) throw std::runtime_error(what); }
void write(fs::path const& p, std::string const& value = "content") { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << value; }
std::string read(fs::path const& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main() {
    auto root = fs::temp_directory_path() / (L"AeluraTests-" + std::to_wstring(GetCurrentProcessId()));
    try {
        fs::create_directories(root / "dest");
        auto rejects = [&](fs::path const& src, fs::path const& dst) { try { aelura::migration::inspect(src, dst); return false; } catch (...) { return true; } };
        auto run = [&](fs::path const& src, bool copy = false) { return aelura::migration::execute(aelura::migration::inspect(src, root / "dest"), [](auto, auto) { return true; }, copy); };
        write(root / L"中文文件.txt"); auto r = run(root / L"中文文件.txt");
        if (r.success) { check(read(root / L"中文文件.txt") == "content", "file link unreadable"); fs::remove(root / L"中文文件.txt"); }
        else check(read(root / L"中文文件.txt") == "content", "failed file move lost source");
        write(root / "tree" / "nested" / "a.bin", std::string(2300000, 'x')); fs::create_directories(root / "tree" / "empty");
        r = run(root / "tree", true); check(r.success, "verified directory copy failed");
        check(read(root / "tree" / "nested" / "a.bin") == std::string(2300000, 'x'), "copy bytes differ"); check(fs::is_directory(root / "tree" / "empty"), "empty directory lost");
        check(rejects(root / "tree", root / "dest"), "reparse source accepted");
        // Remove only the junction itself before recursively cleaning the test sandbox.
        check(RemoveDirectoryW((root / "tree").c_str()) != 0, "junction cleanup failed");
        write(root / "quick" / "a"); r = run(root / "quick"); check(r.success && read(root / "quick" / "a") == "content", "same volume move failed"); RemoveDirectoryW((root / "quick").c_str());
        fs::create_directory(root / "emptydir"); r = run(root / "emptydir", true); check(r.success && fs::is_empty(root / "emptydir"), "empty root failed"); RemoveDirectoryW((root / "emptydir").c_str());
        write(root / "same" / "a"); check(rejects(root / "same", root / "same"), "nested destination accepted"); check(rejects(root / "same", root), "same path accepted");
        fs::create_directory(root / "dest" / "same"); check(rejects(root / "same", root / "dest"), "existing destination accepted");
        check(rejects(root.root_path(), root / "dest"), "drive root accepted"); check(rejects(root / "missing", root / "dest"), "missing source accepted");
        write(root / "cancel" / "data", std::string(4000000, 'z'));
        r = aelura::migration::execute(aelura::migration::inspect(root / "cancel", root / "dest"), [](auto done, auto) { return done == 0; }, true);
        check(!r.success && read(root / "cancel" / "data") == std::string(4000000, 'z') && !fs::exists(root / "dest" / "cancel"), "cancel rollback failed");
        write(root / "changed" / "data"); bool changed = false;
        r = aelura::migration::execute(aelura::migration::inspect(root / "changed", root / "dest"), [&](auto done, auto) { if (done && !changed) { write(root / "changed" / "new"); changed = true; } return true; }, true);
        check(!r.success && fs::exists(root / "changed" / "new"), "source modification was ignored");
        write(root / "locked" / "a"); HANDLE lock = CreateFileW((root / "locked" / "a").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        r = run(root / "locked", true); CloseHandle(lock); check(!r.success && fs::exists(root / "locked" / "a"), "locked source was lost");
        check(aelura::catalog::matches(L"文件管理 FreeMove", L"文件 freemove"), "search not case insensitive");
        HWND window = CreateWindowExW(0, L"STATIC", L"test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        aelura::hotkeys::Manager manager; manager.load(window, root / "keys.ini"); std::wstring error;
        if (!manager.save(2, MOD_CONTROL | MOD_ALT, VK_F22, true, error)) { std::wcerr << error << L" Win32=" << GetLastError() << L" hwnd=" << window << L"\n"; throw std::runtime_error("binding failed"); }
        check(!manager.save(3, MOD_CONTROL | MOD_ALT, VK_F22, true, error), "duplicate binding accepted");
        check(manager.bindings[2].registered && !manager.bindings[3].registered, "binding rollback failed");
        check(manager.save(2, MOD_CONTROL | MOD_ALT, VK_F22, false, error), "disable failed");
        check(RegisterHotKey(window, 999, MOD_CONTROL | MOD_ALT, VK_F22), "disabled binding not released"); UnregisterHotKey(window, 999);
        manager.config = root / "missing-parent" / "not-directory" / "keys.ini"; write(root / "missing-parent");
        check(!manager.save(2, MOD_CONTROL | MOD_ALT, VK_F23, true, error), "persistence failure ignored");
        check(RegisterHotKey(window, 998, MOD_CONTROL | MOD_ALT, VK_F23), "failed persistence leaked hotkey"); UnregisterHotKey(window, 998);
        manager.release(); DestroyWindow(window);
        fs::remove_all(root); std::cout << "PASS: migration, cancellation, source changes, empty directories, conflicts, persistence rollback, search\n"; return 0;
    } catch (std::exception const& e) { std::cerr << "FAIL: " << e.what() << " (sandbox: " << root << ")\n"; return 1; }
}
