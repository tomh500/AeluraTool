#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <utility>
#include "Integration/CaptureProtocol.h"
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
    namespace {
        namespace capture = ::Aelura::Capture;
        std::wstring KeyName(UINT key) {
            return key >= VK_F1 && key <= VK_F24 ? L"F" + std::to_wstring(key - VK_F1 + 1) : std::wstring(1, static_cast<wchar_t>(key));
        }
    }
    void MainWindow::Notice(hstring const& message, bool error) {
        Status().Title(error ? L"需要处理" : L"遥汐工具箱");
        Status().Message(message); Status().Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
    }
    void MainWindow::Page(int index) {
        HomePage().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
        KeysPage().Visibility(index == 1 ? Visibility::Visible : Visibility::Collapsed);
        AboutPage().Visibility(index == 2 ? Visibility::Visible : Visibility::Collapsed);
    }
    void MainWindow::Setup() {
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd_));
        singleton_ = CreateMutexW(nullptr, FALSE, L"Local\\AeluraTool.Singleton");
        if (!singleton_) throw_last_error();
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            EnumWindows([](HWND window, LPARAM)->BOOL {
                if (GetPropW(window, L"AeluraTool.MainWindow")) { ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); return FALSE; } return TRUE;
                }, 0);
            Close(); return;
        }
        SetPropW(hwnd_, L"AeluraTool.MainWindow", reinterpret_cast<HANDLE>(1));
        SetWindowPos(hwnd_, nullptr, 0, 0, 1180, 840, SWP_NOMOVE | SWP_NOZORDER);
        if (!SetWindowSubclass(hwnd_, WindowProc, 1, reinterpret_cast<DWORD_PTR>(this))) throw_last_error();
        PWSTR local{};
        check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local));
        config_ = std::filesystem::path(local) / L"AeluraTool" / L"hotkeys.ini"; CoTaskMemFree(local);
        auto read = [&](wchar_t const* key, int fallback) { return GetPrivateProfileIntW(L"Capture", key, fallback, config_.c_str()); };
        modifiers_ = read(L"Modifiers", MOD_CONTROL | MOD_ALT); key_ = read(L"Key", 'A');
        enabled_ = read(L"Enabled", 1) != 0; bool print = read(L"PrintScreen", 0) != 0;
        if (!(modifiers_ & (MOD_CONTROL | MOD_ALT | MOD_SHIFT)) || modifiers_ & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) modifiers_ = MOD_CONTROL | MOD_ALT;
        if (!((key_ >= 'A' && key_ <= 'Z') || (key_ >= '0' && key_ <= '9') || (key_ >= VK_F1 && key_ <= VK_F24))) key_ = 'A';
        CtrlKey().IsChecked((modifiers_ & MOD_CONTROL) != 0); AltKey().IsChecked((modifiers_ & MOD_ALT) != 0); ShiftKey().IsChecked((modifiers_ & MOD_SHIFT) != 0);
        EnableKeys().IsOn(enabled_); PrintKey().IsChecked(print);
        int selected = 0, index = 0;
        for (UINT k = 'A'; k <= 'Z'; ++k, ++index) { KeyChoice().Items().Append(box_value(KeyName(k))); if (k == key_) selected = index; }
        for (UINT k = '0'; k <= '9'; ++k, ++index) { KeyChoice().Items().Append(box_value(KeyName(k))); if (k == key_) selected = index; }
        for (UINT k = VK_F1; k <= VK_F24; ++k, ++index) { KeyChoice().Items().Append(box_value(KeyName(k))); if (k == key_) selected = index; }
        KeyChoice().SelectedIndex(selected);
        if (!Bind(modifiers_, key_, enabled_, print, false)) ShortcutLabel().Text(L"快捷键尚未注册，请前往全局快捷键更换组合。");
        HomeNav().Click([this](auto const&, auto const&) { Page(0); });
        KeysNav().Click([this](auto const&, auto const&) { Page(1); });
        AboutNav().Click([this](auto const&, auto const&) { Page(2); });
        CaptureButton().Click([this](auto const&, auto const&) { Invoke(1); });
        SettingsButton().Click([this](auto const&, auto const&) { Invoke(2); });
        PreviewButton().Click([this](auto const&, auto const&) { Invoke(3); });
        Search().TextChanged([this](auto const&, auto const&) {
            std::wstring query(Search().Text()); for (auto& c : query) c = towlower(c);
            const bool match = query.empty() || std::wstring(L"截图 分享 screenshotplus capture png 标注").find(query) != std::wstring::npos;
            CaptureCard().Visibility(match ? Visibility::Visible : Visibility::Collapsed); NoResults().Visibility(match ? Visibility::Collapsed : Visibility::Visible);
            });
        SaveKeys().Click([this](auto const&, auto const&) {
            const UINT mods = (CtrlKey().IsChecked().Value() ? MOD_CONTROL : 0) | (AltKey().IsChecked().Value() ? MOD_ALT : 0) | (ShiftKey().IsChecked().Value() ? MOD_SHIFT : 0);
            int i = KeyChoice().SelectedIndex(); UINT key = i < 26 ? 'A' + i : i < 36 ? '0' + i - 26 : VK_F1 + i - 36;
            Bind(mods, key, EnableKeys().IsOn(), PrintKey().IsChecked().Value(), true);
            });
        timer_ = DispatcherQueue().CreateTimer(); timer_.Interval(std::chrono::milliseconds(100));
        timer_.Tick([this](auto const&, auto const&) {
            if (HWND engine = EngineWindow()) {
                timer_.Stop();
                const UINT command = std::exchange(pending_, 0);
                DWORD_PTR result = 0;
                if (command == capture::Start) {
                    if (!SendMessageTimeoutW(engine, capture::Message, capture::CanCapture, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) || result != capture::Accepted) {
                        Notice(L"截图工具正忙，请先关闭截图设置或完成当前编辑。", true);
                        return;
                    }
                    ShowWindow(hwnd_, SW_MINIMIZE);
                }
                if (!SendMessageTimeoutW(engine, capture::Message, command, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
                    if (command == capture::Start) ShowWindow(hwnd_, SW_RESTORE);
                    Notice(L"截图进程未响应，请重试。", true);
                } else if (result != capture::Accepted) {
                    if (command == capture::Start) ShowWindow(hwnd_, SW_RESTORE);
                    Notice(L"截图工具正忙，请完成当前操作后重试。", true);
                } else Notice(command == capture::Start ? L"已唤起截图，按 Esc 可取消。" : command == capture::Settings ? L"截图设置已打开。" : L"已请求显示最近截图。");
                pending_ = 0;
            }
            else if (++attempts_ >= 100 || (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)) {
                timer_.Stop(); pending_ = 0; Notice(L"截图进程未能就绪，请重试；确认 Tools 中的组件完整。", true);
            }
            });
    }
    bool MainWindow::Bind(UINT mods, UINT key, bool enabled, bool print, bool persist) {
        if (enabled && !mods) { Notice(L"至少选择一个修饰键 Ctrl / Alt / Shift。", true); return false; }
        const bool same = registered_ && mods == modifiers_ && key == key_;
        const int candidate = slot_ == 100 ? 101 : 100;
        if (enabled && !same && !RegisterHotKey(hwnd_, candidate, mods | MOD_NOREPEAT, key)) {
            Notice(registered_ ? L"该快捷键已被其他程序占用，原快捷键保持有效。" : L"该快捷键已被占用，请在全局快捷键页面更换组合。", true); return false;
        }
        if (enabled && print && !print_ && !RegisterHotKey(hwnd_, 102, MOD_NOREPEAT, VK_SNAPSHOT)) {
            if (!same) UnregisterHotKey(hwnd_, candidate);
            Notice(L"PrintScreen 已被占用，原快捷键保持有效。", true); return false;
        }
        if (persist) {
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            const auto data = L"[Capture]\r\nModifiers=" + std::to_wstring(mods) + L"\r\nKey=" + std::to_wstring(key) + L"\r\nEnabled=" + std::to_wstring(enabled) + L"\r\nPrintScreen=" + std::to_wstring(print) + L"\r\n";
            auto temp = config_; temp += L".tmp";
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written = 0; const wchar_t bom = 0xFEFF;
            bool ok = file != INVALID_HANDLE_VALUE;
            if (ok) { ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && WriteFile(file, data.data(), static_cast<DWORD>(data.size() * sizeof(wchar_t)), &written, nullptr) && written == data.size() * sizeof(wchar_t) && FlushFileBuffers(file); CloseHandle(file); }
            if (!ok || !MoveFileExW(temp.c_str(), config_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temp.c_str()); if (enabled && !same) UnregisterHotKey(hwnd_, candidate); if (enabled && print && !print_) UnregisterHotKey(hwnd_, 102);
                Notice(L"快捷键配置保存失败，原设置保持有效。", true); return false;
            }
        }
        if (registered_ && (!enabled || !same)) UnregisterHotKey(hwnd_, slot_);
        if (enabled && !same) slot_ = candidate;
        if (print_ && (!enabled || !print)) UnregisterHotKey(hwnd_, 102);
        registered_ = enabled; print_ = enabled && print; enabled_ = enabled; modifiers_ = mods; key_ = key;
        std::wstring label = L"全局快捷键：";
        if (enabled) { if (mods & MOD_CONTROL) label += L"Ctrl + "; if (mods & MOD_ALT) label += L"Alt + "; if (mods & MOD_SHIFT) label += L"Shift + "; label += KeyName(key); if (print) label += L" / PrintScreen"; }
        else label += L"已停用";
        ShortcutLabel().Text(label); Notice(persist ? L"快捷键已保存并生效。" : L"截图工具已就绪，快捷键由遥汐统一管理。"); return true;
    }
    HWND MainWindow::EngineWindow() {
        struct SearchState { DWORD pid; HWND hwnd; } state{ pid_, nullptr };
        EnumWindows([](HWND window, LPARAM value)->BOOL {
            auto& s = *reinterpret_cast<SearchState*>(value); DWORD pid{}; GetWindowThreadProcessId(window, &pid);
            if (pid == s.pid && GetPropW(window, capture::ReadyProperty)) { s.hwnd = window; return FALSE; } return TRUE;
            }, reinterpret_cast<LPARAM>(&state)); return state.hwnd;
    }
    void MainWindow::Invoke(UINT command) {
        if (pending_) { Notice(L"截图组件正在启动，请稍候。"); return; }
        if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
            if (process_) { CloseHandle(process_); process_ = nullptr; }
            if (job_) { CloseHandle(job_); job_ = nullptr; }
            wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
            auto exe = std::filesystem::path(path).parent_path() / L"Tools" / L"ScreenShotPlus.Host.exe";
            std::wstring args = L"\"" + exe.wstring() + L"\" --aelura-host";
            job_ = CreateJobObjectW(nullptr, nullptr); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { Notice(L"无法创建截图进程管理器。", true); return; }
            STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, exe.parent_path().c_str(), &startup, &process)) {
                Notice(L"无法启动截图组件，请确认 Tools/ScreenShotPlus.Host.exe 存在。", true); return;
            }
            if (!AssignProcessToJobObject(job_, process.hProcess)) { TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess); Notice(L"无法托管截图进程。", true); return; }
            process_ = process.hProcess; pid_ = process.dwProcessId;
            if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
                TerminateProcess(process_, 1); CloseHandle(process_); process_ = nullptr;
                CloseHandle(process.hThread); Notice(L"截图进程启动失败，请重试。", true); return;
            }
            CloseHandle(process.hThread);
        }
        pending_ = command; attempts_ = 0; Notice(L"正在唤起截图工具…"); timer_.Start();
    }
    LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<MainWindow*>(data);
        if (msg == WM_HOTKEY && ((self->registered_ && wp == static_cast<WPARAM>(self->slot_)) || (self->print_ && wp == 102))) { self->Invoke(1); return 0; }
        if (msg == WM_DESTROY) {
            if (self->timer_) self->timer_.Stop();
            if (self->registered_) UnregisterHotKey(hwnd, self->slot_); if (self->print_) UnregisterHotKey(hwnd, 102);
            if (auto engine = self->EngineWindow()) PostMessageW(engine, capture::Message, capture::Shutdown, 0);
            RemovePropW(hwnd, L"AeluraTool.MainWindow");
            RemoveWindowSubclass(hwnd, WindowProc, id);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    MainWindow::~MainWindow() {
        if (timer_) timer_.Stop();
        if (process_) CloseHandle(process_); if (job_) CloseHandle(job_); if (singleton_) CloseHandle(singleton_);
    }
}
