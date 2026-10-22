#include "pch.h"
#include "RecordingSelector.h"
#include "CaptureProtocol.h"
#include <chrono>

namespace aelura::recording {
// The screenshot process owns the selection surface. Region-only selection
// uses its existing pixels, mouse state machine, snapping, style and animation.
struct Selector::Impl {
    Selector* owner;
    std::function<HWND()> host;
    HWND receiver{}, engine{};
    bool sent{};
    std::chrono::steady_clock::time_point created{std::chrono::steady_clock::now()};
    static LRESULT CALLBACK receive(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == WM_COPYDATA && reinterpret_cast<HWND>(wp) == self->engine) {
            auto data = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (!data || data->dwData != Aelura::Capture::RegionResultTag || data->cbData != sizeof(Aelura::Capture::RegionResult) || !data->lpData) return FALSE;
            const auto result = *static_cast<Aelura::Capture::RegionResult const*>(data->lpData);
            self->owner->error = result.error;
            if (SUCCEEDED(result.error) && result.bounds.right - result.bounds.left >= 2 && result.bounds.bottom - result.bounds.top >= 2)
                self->owner->result = result.bounds;
            self->owner->done = true;
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    Impl(Selector* p, std::function<HWND()> callback) : owner(p), host(std::move(callback)) {
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"AeluraTool.RecordingRegionResult"; cls.lpfnWndProc = receive;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) winrt::throw_last_error();
        receiver = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, this);
        if (!receiver) winrt::throw_last_error();
    }
    ~Impl() {
        if (sent && !owner->done && IsWindow(engine)) PostMessageW(engine, Aelura::Capture::Message, Aelura::Capture::CancelRegion, reinterpret_cast<LPARAM>(receiver));
        if (receiver) DestroyWindow(receiver);
    }
    void poll() {
        if (owner->done) return;
        if (!sent) {
            engine = host();
            if (engine) {
                DWORD pid{}; GetWindowThreadProcessId(engine, &pid); AllowSetForegroundWindow(pid);
                DWORD_PTR acknowledgement{};
                if (!SendMessageTimeoutW(engine, Aelura::Capture::Message, Aelura::Capture::SelectRegion,
                    reinterpret_cast<LPARAM>(receiver), SMTO_ABORTIFHUNG, 2000, &acknowledgement) || acknowledgement != Aelura::Capture::Accepted) {
                    owner->error = E_FAIL; owner->done = true;
                }
                sent = true;
            } else if (std::chrono::steady_clock::now() - created > std::chrono::seconds(12)) { owner->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT); owner->done = true; }
        } else if (!IsWindow(engine)) { owner->error = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED); owner->done = true; }
    }
};
Selector::Selector(std::function<HWND()> callback) : impl_(std::make_unique<Impl>(this, std::move(callback))) {}
Selector::~Selector() = default;
void Selector::poll() { impl_->poll(); }
}
