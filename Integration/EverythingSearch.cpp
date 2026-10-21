#include "EverythingSearch.h"
#define EVERYTHINGUSERAPI
#include "../Tools/EverythingSDK/include/Everything.h"
#include <algorithm>
#include <chrono>
#pragma comment(lib, "advapi32.lib")

namespace aelura::filesearch {
namespace {
std::timed_mutex transaction;
struct ReplyState { bool ready{}; DWORD id{}; };
std::atomic<DWORD> replySequence{0x41454C};
LRESULT CALLBACK reply(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto state = reinterpret_cast<ReplyState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state && message == WM_COPYDATA && Everything_IsQueryReply(message, wParam, lParam, state->id)) {
        state->ready = true;
        return TRUE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
struct QueryWindow {
    HWND window{};
    ~QueryWindow() { Everything_Reset(); if (window) DestroyWindow(window); }
};
}
Snapshot queryEverything(std::wstring const& query, Options options, std::function<bool()> const& cancelled) {
    Snapshot result;
    if (query.empty()) return result;
    if (query.size() > 512) { result.state = State::InvalidQuery; return result; }
    auto stop = [&] { return cancelled && cancelled(); };
    std::unique_lock<std::timed_mutex> lock(transaction, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20))) if (stop()) return result;
    if (stop()) return result;
    static std::once_flag registration;
    std::call_once(registration, [] {
        WNDCLASSW type{}; type.lpfnWndProc = reply; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"Aelura.EverythingSDK.Reply";
        RegisterClassW(&type);
    });
    ReplyState response{false, ++replySequence}; QueryWindow listener;
    listener.window = CreateWindowExW(0, L"Aelura.EverythingSDK.Reply", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    result.state = State::Failed;
    if (!listener.window) return result;
    ChangeWindowMessageFilterEx(listener.window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    SetWindowLongPtrW(listener.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&response));
    Everything_Reset();
    Everything_SetSearchW(query.c_str());
    Everything_SetMatchPath(options.path || (!options.regex && query.find_first_of(L"\\/") != std::wstring::npos));
    Everything_SetMatchCase(options.matchCase); Everything_SetMatchWholeWord(options.wholeWord); Everything_SetRegex(options.regex);
    Everything_SetMax(static_cast<DWORD>(ResultLimit)); Everything_SetOffset(0);
    Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
    Everything_SetRequestFlags(EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH);
    Everything_SetReplyWindow(listener.window); Everything_SetReplyID(response.id);
    if (!Everything_QueryW(FALSE)) {
        if (Everything_GetLastError() == EVERYTHING_ERROR_IPC) result.state = State::EngineUnavailable;
        return result;
    }
    const auto deadline = GetTickCount64() + 5000;
    while (!response.ready) {
        if (stop()) { result.state = State::Idle; return result; }
        if (GetTickCount64() >= deadline) { result.state = State::TimedOut; return result; }
        MSG message{};
        while (PeekMessageW(&message, listener.window, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (!response.ready) MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (stop()) { result.state = State::Idle; return result; }
    result.state = State::Ready; result.total = Everything_GetTotResults();
    const auto count = (std::min)(Everything_GetNumResults(), static_cast<DWORD>(ResultLimit));
    for (DWORD i = 0; i < count; ++i) {
        auto length = Everything_GetResultFullPathNameW(i, nullptr, 0);
        if (!length || length > 32767) { result.state = State::Failed; result.items.clear(); return result; }
        std::wstring path(static_cast<size_t>(length) + 1, L'\0');
        auto written = Everything_GetResultFullPathNameW(i, path.data(), static_cast<DWORD>(path.size()));
        if (!written || written > length) { result.state = State::Failed; result.items.clear(); return result; }
        path.resize(written);
        auto name = Everything_GetResultFileNameW(i);
        result.items.push_back({name ? std::wstring(name) : std::filesystem::path(path).filename().wstring(), std::move(path), Everything_IsFolderResult(i) != FALSE});
    }
    return result;
}
}
