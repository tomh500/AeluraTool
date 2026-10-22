#include "../Integration/FileShellActions.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <wrl/client.h>
#include "../Integration/FileMigration.h"
namespace {
namespace fs = std::filesystem;
void check(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
void write(fs::path const& path) { fs::create_directories(path.parent_path()); std::ofstream(path) << "temporary shell fixture"; }
void wait() { auto deadline = GetTickCount64() + 20000; while (aelura::files::state().busy && GetTickCount64() < deadline) Sleep(5); check(!aelura::files::state().busy, "shell job did not finish"); }
}
void run_file_shell_tests(fs::path const& root) {
    using namespace aelura::files;
    write(root / L"source" / L"中文.txt"); fs::create_directories(root / L"destination");
    auto source = root / L"source" / L"中文.txt";
    check(validName(L"新名称.txt") && !validName(L"CON.txt") && !validName(L"../bad") && !validName(L"bad.") && !validName(L"COM1"), "rename name validation failed");
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    check(protectedPath(L"C:\\") && protectedPath(fs::path(windows) / L"System32"), "critical paths were not blocked");
    auto initialized = OleInitialize(nullptr); check(SUCCEEDED(initialized), "test STA initialization failed");
    for (bool cut : {false, true}) {
        Microsoft::WRL::ComPtr<IDataObject> data; check(SUCCEEDED(dataObject(source, cut, &data)), "shell file data object failed");
        auto transfer = readTransfer(data.Get());
        std::error_code identityError;
        // Shell may expand an 8.3 temporary-directory path to its long name.
        const bool sameFile = transfer.paths.size() == 1 && fs::equivalent(transfer.paths[0], source, identityError);
        if (!sameFile || transfer.move != cut) {
            std::wcerr << L"Transfer paths=" << transfer.paths.size() << L" expected=" << source
                << L" actual=" << (transfer.paths.empty() ? fs::path{} : transfer.paths[0])
                << L" cut=" << cut << L" move=" << transfer.move << L" error=" << identityError.value() << L"\n";
        }
        check(sameFile && !identityError && transfer.move == cut, "Explorer-compatible file transfer data failed");
    }
    OleUninitialize();
    Request copy; copy.kind = Kind::Paste; copy.sources = {source}; copy.destination = root / L"destination"; copy.interactive = false;
    check(SUCCEEDED(preflight(copy)), "copy preflight failed"); check(start(copy), "copy did not start"); wait();
    check(!state().failed && fs::exists(source) && fs::exists(copy.destination / source.filename()), "shell copy failed or removed source");
    check(start(copy), "collision copy did not start"); wait(); check(!state().failed && fs::exists(source), "collision copy failed");
    size_t count = 0; for (auto const& item : fs::directory_iterator(copy.destination)) { (void)item; ++count; } check(count == 2, "copy collision overwrote the existing target");
    Request rename; rename.kind = Kind::Rename; rename.sources = {source}; rename.name = L"Renamed.txt"; rename.interactive = false;
    check(start(rename), "rename did not start"); wait(); auto renamed = source.parent_path() / rename.name;
    check(!state().failed && fs::exists(renamed) && !fs::exists(source), "shell rename failed");
    write(source); rename.sources = {source}; check(FAILED(preflight(rename)), "rename overwrote existing target");
    Request link; link.kind = Kind::Shortcut; link.sources = {renamed}; link.destination = root / L"destination"; link.interactive = false;
    check(start(link), "shortcut did not start"); wait(); check(!state().failed && fs::exists(link.destination / L"Renamed.txt.lnk"), "shortcut failed");
    check(start(link), "second shortcut did not start"); wait(); check(fs::exists(link.destination / L"Renamed.txt (2).lnk"), "shortcut overwrote prior shortcut");
    copy.move = true; copy.sources = {renamed}; check(start(copy), "cut/paste did not start"); wait();
    check(!state().failed && !fs::exists(renamed) && fs::exists(copy.destination / renamed.filename()), "cut/paste failed");
    auto verified = root / L"verified" / L"verify.txt"; write(verified);
    copy.sources = {verified}; copy.verifyMove = true; copy.recoveryFolder = root / L"recovery";
    check(start(copy), "verified move did not start"); wait();
    check(!state().failed && !state().cancelled && !fs::exists(verified) && fs::exists(copy.destination / verified.filename()) && state().recovery.empty(), "copy-verify-remove move failed");
    auto locked = root / L"locked" / L"locked.txt"; write(locked);
    auto handle = CreateFileW(locked.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "locked source fixture failed"); copy.sources = {locked};
    check(start(copy), "locked verified move did not start"); wait(); CloseHandle(handle);
    check(state().failed && fs::exists(locked) && !state().recovery.empty() && fs::exists(state().recovery), "failed verified move lost original or recovery record");
    Request recycle; recycle.kind = Kind::Recycle; recycle.sources = {source}; recycle.interactive = false;
    check(start(recycle), "recycle did not start"); wait(); check(!state().failed && !fs::exists(source), "temporary file recycling failed");
    Request nested = copy; nested.move = false; nested.sources = {root / L"destination"}; nested.destination = root / L"destination"; check(FAILED(preflight(nested)), "recursive paste accepted");
    check(preflight(copy, [] { return true; }) == E_ABORT, "preflight cancellation ignored");
    write(root / L"junction-source" / L"child.txt"); fs::create_directories(root / L"junction-target");
    auto linked = aelura::migration::execute(aelura::migration::inspect(root / L"junction-source", root / L"junction-target"), [](auto, auto) { return true; });
    check(linked.success, "junction preflight fixture failed");
    Request noLink = copy; noLink.sources = {root / L"junction-source"}; noLink.move = false; check(FAILED(preflight(noLink)), "recursive junction operation accepted");
    noLink.sources = {root / L"junction-source" / L"child.txt"}; check(FAILED(preflight(noLink)), "reparse ancestor operation accepted");
    fs::create_directories(root / L"cancel-source"); for (int i = 0; i < 1000; ++i) write(root / L"cancel-source" / (std::to_wstring(i) + L".txt"));
    Request cancelled; cancelled.kind = Kind::Paste; cancelled.sources = {root / L"cancel-source"}; cancelled.destination = root / L"destination"; cancelled.interactive = false;
    check(start(cancelled), "cancel fixture did not start"); cancel(); wait(); check(state().cancelled && fs::exists(root / L"cancel-source" / L"0.txt"), "cancellation lost source or was ignored");
    dialog(true); check(active() && !start(link), "modal dialog did not block a competing job"); dialog(false);
    std::cout << "PASS: Shell transfer data, collision safety, cut/paste, verified move and recovery, recycling, rename, shortcuts, reparse exclusion, cancellation, modal close guard (temporary paths only)\n";
}
