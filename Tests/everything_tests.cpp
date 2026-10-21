#include "../Integration/EverythingSearch.h"
#include "../Tools/EverythingSDK/ipc/everything_ipc.h"
#include <cstring>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace aelura::filesearch;
void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
struct Server {
    std::wstring directory;
    std::atomic<int> mode{0}; // 0=reply, 1=accept without reply, 2=unresponsive delivery
    std::atomic<DWORD> flags{0}, maximum{0};
    HWND window{}; std::thread worker;
    struct Pending { DWORD window, id; std::wstring name; };
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto self = reinterpret_cast<Server*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_COPYDATA) {
            auto data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (data->dwData != EVERYTHING_IPC_COPYDATAQUERYW || data->cbData < sizeof(EVERYTHING_IPC_QUERYW)) return FALSE;
            auto query = reinterpret_cast<EVERYTHING_IPC_QUERYW*>(data->lpData);
            self->flags = query->search_flags; self->maximum = query->max_results;
            if (self->mode == 2) { Sleep(2500); return TRUE; }
            if (self->mode == 1) return TRUE;
            auto pending = new Pending{query->reply_hwnd, query->reply_copydata_message, std::wstring(query->search_string) + L".txt"};
            PostMessageW(window, WM_APP, 0, reinterpret_cast<LPARAM>(pending)); return TRUE;
        }
        if (self && message == WM_APP) {
            std::unique_ptr<Pending> pending(reinterpret_cast<Pending*>(lParam));
            auto nameBytes = (pending->name.size() + 1) * sizeof(wchar_t), pathBytes = (self->directory.size() + 1) * sizeof(wchar_t);
            std::vector<char> encoded(sizeof(EVERYTHING_IPC_LISTW) + nameBytes + pathBytes);
            auto list = reinterpret_cast<EVERYTHING_IPC_LISTW*>(encoded.data());
            list->totfiles = list->totitems = 501; list->numfiles = list->numitems = 1;
            list->items[0].filename_offset = sizeof(EVERYTHING_IPC_LISTW);
            list->items[0].path_offset = static_cast<DWORD>(sizeof(EVERYTHING_IPC_LISTW) + nameBytes);
            std::memcpy(encoded.data() + list->items[0].filename_offset, pending->name.c_str(), nameBytes);
            std::memcpy(encoded.data() + list->items[0].path_offset, self->directory.c_str(), pathBytes);
            COPYDATASTRUCT reply{pending->id, static_cast<DWORD>(encoded.size()), encoded.data()}; DWORD_PTR accepted{};
            SendMessageTimeoutW(reinterpret_cast<HWND>(static_cast<UINT_PTR>(pending->window)), WM_COPYDATA,
                reinterpret_cast<WPARAM>(window), reinterpret_cast<LPARAM>(&reply), SMTO_ABORTIFHUNG, 1000, &accepted);
            return 0;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    explicit Server(fs::path const& root) : directory(root.wstring()) {
        std::promise<HWND> started; auto ready = started.get_future();
        worker = std::thread([this, &started] {
            WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"Aelura.Tests.EverythingIPC";
            RegisterClassW(&type);
            auto handle = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, this);
            started.set_value(handle); if (!handle) return;
            MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        });
        window = ready.get();
    }
    ~Server() { if (window) PostMessageW(window, WM_CLOSE, 0, 0); if (worker.joinable()) worker.join(); }
};
}
void run_everything_tests(fs::path const& root) {
    // Debug and Release runs share the synthetic SDK window class. Keep their
    // missing-server and timeout fixtures from answering each other's queries.
    struct Isolation {
        HANDLE mutex{CreateMutexW(nullptr, FALSE, L"Local\\AeluraTool.SdkTestIsolation")};
        Isolation() {
            check(mutex != nullptr, "SDK isolation mutex creation failed");
            auto result = WaitForSingleObject(mutex, 60000);
            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) { CloseHandle(mutex); mutex = nullptr; throw std::runtime_error("SDK isolation mutex timeout"); }
        }
        ~Isolation() { if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); } }
    } isolation;
    using namespace aelura::filesearch;
    fs::create_directories(root / L"files");
    check(queryEverything(L"test", {}, {}).state == State::EngineUnavailable, "missing SDK client not reported");
    check(queryEverything(std::wstring(513, L'x'), {}, {}).state == State::InvalidQuery, "SDK query size was not checked");
    check(queryEverything(L"test", {}, [] { return true; }).state == State::Idle, "SDK pre-query cancellation failed");
    {
        Server server(root / L"files"); check(server.window != nullptr, "isolated SDK server failed");
        auto result = queryEverything(L"中文", {true, true, true, true}, {});
        check(result.state == State::Ready && result.total == 501 && result.items.size() == 1 &&
            result.items[0].path == (root / L"files" / L"中文.txt").wstring(), "SDK Unicode paths or total count incorrect");
        check(server.maximum == ResultLimit && server.flags == (EVERYTHING_IPC_MATCHPATH | EVERYTHING_IPC_MATCHCASE | EVERYTHING_IPC_MATCHWHOLEWORD | EVERYTHING_IPC_REGEX), "SDK options or result limit not forwarded");
        auto first = std::async(std::launch::async, [] { return queryEverything(L"first", {}, {}); });
        auto second = std::async(std::launch::async, [] { return queryEverything(L"second", {}, {}); });
        check(first.get().items.at(0).name == L"first.txt" && second.get().items.at(0).name == L"second.txt", "concurrent SDK transactions mixed results");
        server.mode = 1;
        auto start = GetTickCount64();
        check(queryEverything(L"cancel", {}, [start] { return GetTickCount64() - start > 100; }).state == State::Idle, "SDK pending reply cancellation failed");
        check(queryEverything(L"timeout", {}, {}).state == State::TimedOut, "SDK reply timeout not reported");
        server.mode = 2; start = GetTickCount64();
        check(queryEverything(L"hung", {}, {}).state == State::EngineUnavailable && GetTickCount64() - start < 4000, "SDK delivery blocked indefinitely");
    }
    auto cache = root / L"index.bin";
    {
        Index index(cache, {root / L"files"});
        while (index.progress().running) Sleep(5);
        check(index.setEverything(true) && index.everything(), "SDK mode save failed");
    }
    {
        Index index(cache, {root / L"files"});
        check(index.everything() && !index.progress().running, "saved SDK mode started a built-in scan");
        auto shared = std::shared_ptr<Index>(&index, [](Index*) {}); Session session(shared);
        session.search(L"test"); auto deadline = GetTickCount64() + 3000;
        while (session.snapshot().state == State::Searching && GetTickCount64() < deadline) Sleep(5);
        check(session.snapshot().state == State::EngineUnavailable, "session did not route to the SDK backend");
        check(index.setEverything(false) && !index.everything(), "switching back to built-in failed");
        while (index.progress().running) Sleep(5);
        check(!index.progress().failed, "built-in backend failed after mode change");
    }
    std::ofstream(root / L"blocked") << "fixture";
    Index blocked(root / L"blocked" / L"index.bin", {root / L"files"});
    check(!blocked.setEverything(true) && !blocked.everything(), "failed mode save did not preserve previous backend");
    std::cout << "PASS: isolated Everything SDK IPC, Unicode, options, totals, concurrent transactions, cancellation, reply/delivery timeout, mode persistence and rollback\n";
}
