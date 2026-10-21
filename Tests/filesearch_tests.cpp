#include "../Integration/FileSearch.h"
#include "../Integration/NtfsFileIndex.h"
#include "../Integration/FileMigration.h"
#include "../Integration/HotkeyManager.h"
#include "../Integration/ToolCatalog.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
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
    {
        Index reused(cache, {files}); waitIndex(reused);
        check(reused.progress().cached && match(reused.snapshot(), L"Alphabet", {}).total == 1 &&
            match(reused.snapshot(), L"Renamed", {}).total == 0, "startup discarded cache and rescanned files");
        reused.refresh(); waitIndex(reused);
        check(!reused.progress().cached && match(reused.snapshot(), L"Renamed", {}).total == 1, "explicit refresh failed after cache reuse");
    }
    index->refresh(); waitIndex(*index); source = index->snapshot();
    check(match(source, L"Renamed", {}).total == 1 && match(source, L"Alphabet", {}).total == 0 && match(source, L"New", {}).total == 1, "refresh did not reflect rename/delete/create");
    session.search(L"Renamed"); result = wait(session);
    check(result.total == 1 && result.sourceRevision == source.progress.revision, "query lost its index revision");
    check(match(source, L"[", {false, false, false, true}).sourceRevision == source.progress.revision, "invalid query lost its index revision");
    index->refresh(); index->cancel(); waitIndex(*index);
    check(index->progress().cancelled && match(index->snapshot(), L"Renamed", {}).total == 1, "refresh cancel lost prior index");
    fs::rename(files / L"Renamed.txt", files / L"Edited.txt"); write(files / L"unrelated.txt");
    index->updatePaths({files / L"Renamed.txt", files / L"Edited.txt"}); waitIndex(*index);
    check(match(index->snapshot(), L"Edited", {}).total == 1 && match(index->snapshot(), L"Renamed", {}).total == 0 && match(index->snapshot(), L"unrelated", {}).total == 0, "targeted update was missing or rescanned unrelated paths");
    check(index->setEverything(true), "SDK switch fixture failed"); fs::rename(files / L"Edited.txt", files / L"Later.txt");
    index->updatePaths({files / L"Edited.txt", files / L"Later.txt"}); check(!index->progress().running, "SDK mode started a built-in update");
    check(index->setEverything(false), "built-in restore failed"); waitIndex(*index);
    check(match(index->snapshot(), L"Later", {}).total == 1 && match(index->snapshot(), L"unrelated", {}).total == 0, "deferred updates failed or triggered a full rescan");
    fs::remove(files / L"unrelated.txt");
    auto deferredCache = root / L"deferred.bin";
    { Index deferred(deferredCache, {files}); waitIndex(deferred); check(deferred.setEverything(true), "deferred SDK setup failed");
      fs::rename(files / L"Later.txt", files / L"AfterRestart.txt"); write(files / L"not-rescanned.txt");
      deferred.updatePaths({files / L"Later.txt", files / L"AfterRestart.txt"}); }
    { Index restored(deferredCache, {files}); check(restored.everything() && !restored.progress().running, "deferred SDK startup scanned files");
      check(restored.setEverything(false), "deferred restore failed"); waitIndex(restored);
      check(match(restored.snapshot(), L"AfterRestart", {}).total == 1 && match(restored.snapshot(), L"Later", {}).total == 0 && match(restored.snapshot(), L"not-rescanned", {}).total == 0, "deferred updates did not survive restart or scanned unrelated files"); }
    fs::remove(files / L"not-rescanned.txt");
    // Corrupt caches are ignored; a normal scan repairs them.
    auto badCache = root / L"bad.bin"; std::ofstream(badCache) << "truncated cache";
    Index repaired(badCache, {files}); waitIndex(repaired); check(repaired.progress().indexed == 7, "corrupt cache recovery failed");
    Index missing(root / L"missing.bin", {root / L"absent"}); waitIndex(missing); check(missing.progress().skipped == 1, "missing root not reported");
    Index changedRoots(cache, {root / L"absent"}); waitIndex(changedRoots);
    check(!changedRoots.progress().cached && changedRoots.progress().indexed == 0, "cache from different roots was reused");
    write(root / L"blocked-cache"); Index unsaved(root / L"blocked-cache" / L"index.bin", {files}); waitIndex(unsaved);
    check(unsaved.progress().cacheFailed && unsaved.progress().indexed == 7, "cache save failure was silent or discarded the usable index");
    // Test metadata parsing and path reconstruction entirely with synthetic data.
    ntfs::Records records{{10, {5, L"folder", FILE_ATTRIBUTE_DIRECTORY}}, {11, {10, L"file.txt", 0}},
        {12, {5, L"junction", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT}}, {13, {12, L"hidden.txt", 0}},
        {14, {15, L"cycle", FILE_ATTRIBUTE_DIRECTORY}}, {15, {14, L"cycle", FILE_ATTRIBUTE_DIRECTORY}}};
    check(ntfs::resolve(11, 5, L"C:\\", records) == L"C:\\folder\\file.txt", "MFT parent path resolution failed");
    check(ntfs::resolve(13, 5, L"C:\\", records).empty() && ntfs::resolve(14, 5, L"C:\\", records).empty(), "MFT followed reparse point or cycle");
    USN_RECORD_V2 record{}; record.RecordLength = static_cast<DWORD>(offsetof(USN_RECORD_V2, FileName) + 8);
    record.MajorVersion = 2; record.FileReferenceNumber = 21; record.ParentFileReferenceNumber = 10;
    record.FileNameLength = 8; record.FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    std::vector<char> encoded(record.RecordLength); std::memcpy(encoded.data(), &record, record.FileNameOffset);
    std::memcpy(encoded.data() + record.FileNameOffset, L"test", 8);
    check(ntfs::readRecords(encoded.data(), encoded.size(), records) && records.at(21).name == L"test", "MFT record parsing failed");
    std::vector<ntfs::Change> changes;
    check(ntfs::readChanges(encoded.data(), encoded.size(), changes) && changes.size() == 1 && changes[0].id == 21, "USN change parsing failed");
    check(!ntfs::readChanges(encoded.data(), encoded.size() - 1, changes) && changes.size() == 1, "malformed USN buffer partially published");
    ntfs::Journal journal; journal.root = L"C:\\"; journal.rootId = 5; journal.records = records; journal.journalId = 7; journal.next = 100;
    USN_JOURNAL_DATA_V0 checkpoint{}; checkpoint.UsnJournalID = 7; checkpoint.FirstUsn = 90; checkpoint.LowestValidUsn = 90; checkpoint.NextUsn = 110;
    check(journal.continuous(checkpoint), "valid USN checkpoint rejected");
    checkpoint.UsnJournalID = 8; check(!journal.continuous(checkpoint), "recreated journal accepted"); checkpoint.UsnJournalID = 7;
    checkpoint.FirstUsn = 101; check(!journal.continuous(checkpoint), "wrapped journal accepted"); checkpoint.FirstUsn = 90;
    checkpoint.LowestValidUsn = 101; check(!journal.continuous(checkpoint), "invalid journal range accepted"); checkpoint.LowestValidUsn = 90;
    checkpoint.NextUsn = 99; check(!journal.continuous(checkpoint), "future journal cursor accepted");
    std::vector<fs::path> affected;
    journal.apply({{10, {5, L"folder", FILE_ATTRIBUTE_DIRECTORY}, USN_REASON_RENAME_OLD_NAME},
        {10, {5, L"renamed", FILE_ATTRIBUTE_DIRECTORY}, USN_REASON_RENAME_NEW_NAME},
        {30, {10, L"new.txt", 0}, USN_REASON_FILE_CREATE},
        {21, {10, L"test", 0}, USN_REASON_FILE_DELETE}}, affected);
    check(ntfs::resolve(11, 5, journal.root, journal.records) == L"C:\\renamed\\file.txt" &&
        ntfs::resolve(30, 5, journal.root, journal.records) == L"C:\\renamed\\new.txt" && !journal.records.count(21), "USN rename/create/delete graph update failed");
    check(std::find(affected.begin(), affected.end(), fs::path(L"C:\\folder")) != affected.end() &&
        std::find(affected.begin(), affected.end(), fs::path(L"C:\\renamed")) != affected.end(), "directory rename lost old or new subtree root");
    affected.clear(); journal.apply({{31, {12, L"hidden.txt", 0}, USN_REASON_FILE_CREATE}}, affected);
    check(affected.empty(), "USN update traversed a reparse parent");
    check(!ntfs::readRecords(encoded.data(), encoded.size() - 1, records), "truncated MFT record accepted");
    encoded[4] = 3; check(!ntfs::readRecords(encoded.data(), encoded.size(), records), "unsupported MFT version accepted");
    auto many = std::make_shared<Batch>();
    for (int i = 0; i < 500; ++i) many->push_back({{L"item.txt", L"C:\\fixture\\item.txt", false}, L"item.txt"});
    IndexSnapshot large; large.batches.push_back(many); result = match(large, L"item", {});
    check(result.total == 500 && result.items.size() == ResultLimit, "result cap or total incorrect");
    std::vector<Item> visible{{L"B", L"C:\\B"}, {L"C", L"C:\\C"}};
    std::vector<Item> incoming{{L"A", L"C:\\A"}, {L"B", L"C:\\B"}, {L"C", L"C:\\C"}};
    preserveResultOrder(visible, incoming);
    check(incoming[0].path == L"C:\\B" && incoming[1].path == L"C:\\C" && incoming[2].path == L"C:\\A", "index update reordered visible rows");
    visible = incoming; incoming = {{L"C renamed", L"C:\\C"}, {L"D", L"C:\\D"}};
    preserveResultOrder(visible, incoming);
    check(incoming.size() == 2 && incoming[0].name == L"C renamed" && incoming[1].path == L"C:\\D", "row removal/update lost surviving order");
    HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    aelura::hotkeys::Manager manager; manager.load(window, root / "keys.ini"); std::wstring error;
    auto searchBinding = std::find_if(manager.bindings.begin(), manager.bindings.end(), [](auto const& b) { return b.id == L"filesearch"; });
    check(searchBinding != manager.bindings.end(), "search hotkey missing");
    const size_t keyIndex = static_cast<size_t>(searchBinding - manager.bindings.begin());
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
