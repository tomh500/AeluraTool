#include "../Integration/FileMigration.h"
#include "../Integration/HotkeyManager.h"
#include "../Integration/ToolCatalog.h"
#include "../Integration/I18n.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
void check(bool ok, char const* what) { if (!ok) throw std::runtime_error(what); }
void write(fs::path const& p, std::string const& value = "content") { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << value; }
std::string read(fs::path const& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
void run_migration_tests(fs::path const& root) {
        check(aelura::i18n::resolve(0, L"zh-CN") == 0 && aelura::i18n::resolve(0, L"zh-TW") == 0, "Chinese locale detection failed");
        check(aelura::i18n::resolve(0, L"ja-JP") == 1 && aelura::i18n::resolve(0, L"de-DE") == 1, "English fallback failed");
        check(aelura::i18n::resolve(1, L"en-US") == 0 && aelura::i18n::resolve(2, L"zh-CN") == 1, "language override failed");
        aelura::i18n::select(2); check(std::wstring(aelura::i18n::tr("nav.all")) == L"All tools", "English resource failed");
        aelura::i18n::select(1); check(std::wstring(aelura::i18n::tr("nav.all")) == L"全部工具", "Chinese resource failed");
        aelura::i18n::select(0);
        fs::create_directories(root / "dest");
        auto rejects = [&](fs::path const& src, fs::path const& dst) { try { aelura::migration::inspect(src, dst); return false; } catch (...) { return true; } };
        auto run = [&](fs::path const& src, bool copy = false) { return aelura::migration::execute(aelura::migration::inspect(src, root / "dest"), [](auto, auto) { return true; }, copy); };
        write(root / L"中文文件.txt"); auto r = run(root / L"中文文件.txt");
        if (r.success) { check(read(root / L"中文文件.txt") == "content", "file link unreadable"); fs::remove(root / L"中文文件.txt"); }
        else check(read(root / L"中文文件.txt") == "content", "failed file move lost source");
        write(root / "tree" / "nested" / "a.bin", std::string(2300000, 'x')); fs::create_directories(root / "tree" / "empty");
        r = run(root / "tree", true); if (!r.success) std::cerr << fs::path(r.message).u8string() << '\n'; check(r.success, "verified directory copy failed");
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
        write(root / "race" / "a");
        r = aelura::migration::execute(aelura::migration::inspect(root / "race", root / "dest"), [&](auto, auto) { write(root / "dest" / "race" / "other", "keep"); return true; });
        check(!r.success && read(root / "dest" / "race" / "other") == "keep" && fs::exists(root / "race" / "a"), "concurrent destination overwritten");
        write(root / "copyrace" / "a");
        r = aelura::migration::execute(aelura::migration::inspect(root / "copyrace", root / "dest"), [&](auto done, auto) { if (done) write(root / "dest" / "copyrace" / "other", "keep"); return true; }, true);
        check(!r.success && read(root / "dest" / "copyrace" / "other") == "keep" && fs::exists(root / "copyrace" / "a"), "copy commit rollback failed");
        check(aelura::catalog::matches(L"文件管理 FreeMove", L"文件 freemove"), "search not case insensitive");
        HWND window = CreateWindowExW(0, L"STATIC", L"test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        aelura::hotkeys::Manager manager; manager.load(window, root / "keys.ini"); std::wstring error;
        const size_t hashIndex=5;
        check(manager.bindings.at(hashIndex).id == L"hash" && !manager.bindings.at(hashIndex).enabled, "hash hotkey must default to disabled");
        check(manager.bindings.at(6).id==L"record"&&!manager.bindings.at(6).enabled&&manager.bindings.at(7).id==L"replay-save"&&!manager.bindings.at(7).enabled,"recording hotkeys must default to disabled");
        if (!manager.save(2, MOD_CONTROL | MOD_ALT, VK_F22, true, error)) { std::wcerr << error << L" Win32=" << GetLastError() << L" hwnd=" << window << L"\n"; throw std::runtime_error("binding failed"); }
        check(!manager.save(3, MOD_CONTROL | MOD_ALT, VK_F22, true, error), "duplicate binding accepted");
        check(manager.bindings[2].registered && !manager.bindings[3].registered, "binding rollback failed");
        check(manager.save(2, MOD_CONTROL | MOD_ALT, VK_F22, false, error), "disable failed");
        check(RegisterHotKey(window, 999, MOD_CONTROL | MOD_ALT, VK_F22), "disabled binding not released"); UnregisterHotKey(window, 999);
        manager.config = root / "missing-parent" / "not-directory" / "keys.ini"; write(root / "missing-parent");
        check(!manager.save(2, MOD_CONTROL | MOD_ALT, VK_F23, true, error), "persistence failure ignored");
        check(RegisterHotKey(window, 998, MOD_CONTROL | MOD_ALT, VK_F23), "failed persistence leaked hotkey"); UnregisterHotKey(window, 998);
        manager.release();
        const auto upgrade = root / "upgrade.ini";
        check(WritePrivateProfileStringW(L"Capture", L"Modifiers", L"6", upgrade.c_str()) &&
            WritePrivateProfileStringW(L"Capture", L"Key", L"90", upgrade.c_str()) &&
            WritePrivateProfileStringW(L"Capture", L"Enabled", L"0", upgrade.c_str()) &&
            WritePrivateProfileStringW(L"Appearance", L"Theme", L"2", upgrade.c_str()), "upgrade fixture failed");
        manager.load(window, upgrade);
        check(manager.bindings[0].key == 'Z' && !manager.bindings[0].enabled && !manager.bindings.back().enabled, "hash upgrade changed existing settings");
        check(manager.save(hashIndex, MOD_CONTROL | MOD_ALT, VK_F21, true, error), "hash binding save failed");
        check(GetPrivateProfileIntW(L"Appearance", L"Theme", 0, upgrade.c_str()) == 2, "hash save changed theme");
        manager.release(); manager.load(window, upgrade);
        check(manager.bindings.at(hashIndex).id == L"hash" && manager.bindings.at(hashIndex).registered && manager.bindings[0].key == 'Z', "hash binding restore failed");
        aelura::i18n::select(2); manager.localize();
        check(manager.bindings.at(hashIndex).title == L"Open file hash verifier" && manager.bindings.at(hashIndex).category == L"Files", "hash binding localization failed");
        check(manager.save(6,MOD_CONTROL|MOD_ALT,VK_F20,true,error),"recording binding save failed");
        check(!manager.save(7,MOD_CONTROL|MOD_ALT,VK_F20,true,error)&&manager.bindings[6].registered&&!manager.bindings[7].registered,"replay conflict lost recording binding");
        auto old=manager.bindings[6];manager.config=root/L"missing-parent"/L"invalid"/L"keys.ini";
        check(!manager.save(6,MOD_CONTROL|MOD_ALT,VK_F19,true,error)&&manager.bindings[6].key==old.key&&manager.bindings[6].registered,"recording persistence rollback lost old binding");
        check(RegisterHotKey(window,997,MOD_CONTROL|MOD_ALT,VK_F19),"recording persistence failure leaked candidate");UnregisterHotKey(window,997);
        manager.release(); DestroyWindow(window);
        std::cout << "PASS: migration, cancellation, source changes, empty directories, conflicts, persistence rollback, search\n";
}
