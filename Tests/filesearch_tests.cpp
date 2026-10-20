#include "../Integration/FileSearch.h"
#include "../Integration/FileMigration.h"
#include "../Integration/HotkeyManager.h"
#include "../Integration/ToolCatalog.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace aelura::filesearch;
namespace fs = std::filesystem;
void check(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
void write(fs::path const& path) { fs::create_directories(path.parent_path()); std::ofstream(path) << "temporary fixture"; }
void waitIndex(Index& index) {
    auto deadline = GetTickCount64() + 15000;
    while (index.progress().running && GetTickCount64() < deadline) Sleep(5);
    check(!index.progress().running && !index.progress().failed, "index did not complete");
}
Snapshot wait(Session& session) {
    auto deadline = GetTickCount64() + 5000;
    do { auto result = session.snapshot(); if (result.state != State::Searching) return result; Sleep(2); } while (GetTickCount64() < deadline);
    throw std::runtime_error("search did not complete");
}
}
void run_filesearch_tests(fs::path const& root) {
    using namespace aelura::filesearch;
    auto files = root / L"files", cache = root / L"cache" / L"index.bin";
    write(files / L"Alpha.txt"); write(files / L"Alphabet.md"); write(files / L"中文 日本語.txt");
    write(files / L"Äpfel.TXT");
    write(files / L"nested folder" / L"Alpha.txt");
    write(files / L"linked" / L"not-followed.txt"); fs::create_directories(root / L"outside");
    auto moved = aelura::migration::execute(aelura::migration::inspect(files / L"linked", root / L"outside"), [](auto, auto) { return true; });
    check(moved.success, "junction test setup failed");
    auto index = std::make_shared<Index>(cache, std::vector<fs::path>{files}); waitIndex(*index);
    auto source = index->snapshot();
    check(source.progress.indexed == 7, "unexpected index size / reparse traversal");
    check(match(source, L"not-followed", {}).total == 0, "index recursed through a junction");
    check(match(source, L"alpha", {}).total == 3, "case-insensitive substring search failed");
    check(match(source, L"alpha", {false, true}).total == 0, "case-sensitive search failed");
    check(match(source, L"alpha", {false, false, true}).total == 2, "whole-word search failed");
    check(match(source, L"中文 日本語", {}).total == 1, "Unicode AND search failed");
    check(match(source, L"äPFEL", {}).total == 1, "Unicode case-insensitive search failed");
    check(match(source, L"中", {false, false, true}).total == 0, "Unicode word boundary failed");
    check(match(source, L"\"中文 日本語\"", {}).total == 1, "quoted search failed");
    check(match(source, L"ext:txt;md", {}).total == 5, "extension search failed");
    check(match(source, L"folder:", {}).total == 2, "folder filter failed");
    check(match(source, L"file:Alpha ext:txt", {}).total == 2, "combined file filter failed");
    check(match(source, L"nested folder Alpha", {true}).total == 1, "path AND terms failed");
    check(match(source, L"\"" + (files / L"nested folder").wstring() + L"\"", {true}).total == 2, "path query failed");
    check(match(source, L"A?pha*", {}).total == 3, "wildcard search failed");
    check(match(source, L"^Alpha\\.txt$", {false, true, false, true}).total == 2, "regex search failed");
    check(match(source, L"[", {false, false, false, true}).state == State::InvalidQuery, "invalid regex accepted");
    check(match(source, L"\"unfinished", {}).state == State::InvalidQuery, "unclosed quote accepted");
    check(match(source, std::wstring(513, L'a'), {}).state == State::InvalidQuery, "oversize query accepted");
    check(match(source, L"Alpha", {}, [] { return true; }).state == State::Idle, "matching cancellation failed");
    check(fs::exists(cache), "index cache was not saved");
    Session session(index); session.search(L"old"); auto newest = session.search(L"中文"); auto result = wait(session);
    check(result.generation == newest && result.items.size() == 1, "stale query replaced latest result");
    session.search(L"Alpha"); session.cancel(); Sleep(10); check(session.snapshot().state == State::Idle, "cancel retained a result");
    fs::rename(files / L"Alpha.txt", files / L"Renamed.txt"); fs::remove(files / L"Alphabet.md"); write(files / L"New.txt");
    index->refresh(); waitIndex(*index); source = index->snapshot();
    check(match(source, L"Renamed", {}).total == 1 && match(source, L"Alphabet", {}).total == 0 && match(source, L"New", {}).total == 1, "refresh did not reflect rename/delete/create");
    session.search(L"Renamed"); result = wait(session);
    check(result.total == 1 && result.sourceRevision == source.progress.revision, "query lost its index revision");
    check(match(source, L"[", {false, false, false, true}).sourceRevision == source.progress.revision, "invalid query lost its index revision");
    index->refresh(); index->cancel(); waitIndex(*index);
    check(index->progress().cancelled && match(index->snapshot(), L"Renamed", {}).total == 1, "refresh cancel lost prior index");
    // Corrupt caches are ignored; a normal scan repairs them.
    auto badCache = root / L"bad.bin"; std::ofstream(badCache) << "truncated cache";
    Index repaired(badCache, {files}); waitIndex(repaired); check(repaired.progress().indexed == 7, "corrupt cache recovery failed");
    Index missing(root / L"missing.bin", {root / L"absent"}); waitIndex(missing); check(missing.progress().skipped == 1, "missing root not reported");
    auto many = std::make_shared<Batch>();
    for (int i = 0; i < 500; ++i) many->push_back({{L"item.txt", L"C:\\fixture\\item.txt", false}, L"item.txt", L"c:\\fixture\\item.txt"});
    IndexSnapshot large; large.batches.push_back(many); result = match(large, L"item", {});
    check(result.total == 500 && result.items.size() == ResultLimit, "result cap or total incorrect");
    HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    aelura::hotkeys::Manager manager; manager.load(window, root / "keys.ini"); std::wstring error; const size_t keyIndex = 6;
    check(manager.bindings[keyIndex].id == L"filesearch" && manager.bindings[keyIndex].enabled, "search hotkey default missing");
    check(manager.save(keyIndex, MOD_CONTROL | MOD_ALT, VK_F20, true, error), "search hotkey save failed");
    check(RegisterHotKey(window, 999, MOD_CONTROL | MOD_ALT, VK_F19), "occupancy fixture failed");
    check(!manager.save(keyIndex, MOD_CONTROL | MOD_ALT, VK_F19, true, error) && manager.bindings[keyIndex].key == VK_F20 && manager.bindings[keyIndex].registered, "occupied rollback failed");
    UnregisterHotKey(window, 999); write(root / "blocked"); manager.config = root / "blocked" / "keys.ini";
    check(!manager.save(keyIndex, MOD_CONTROL | MOD_ALT, VK_F18, true, error) && manager.bindings[keyIndex].key == VK_F20 && manager.bindings[keyIndex].registered, "save rollback failed");
    check(RegisterHotKey(window, 998, MOD_CONTROL | MOD_ALT, VK_F18), "failed save leaked candidate"); UnregisterHotKey(window, 998);
    manager.release(); DestroyWindow(window);
    check(aelura::catalog::matches(L"快捷文件检索 Everything", L"everything 检索"), "catalog search failed");
    std::cout << "PASS: built-in indexing, junction exclusion, Unicode, filters, wildcards, regex, cache, refresh, cancellation, stale queries, hotkey rollback\n";
}
