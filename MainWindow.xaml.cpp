#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <utility>
#include "Integration/CaptureProtocol.h"
#include "Integration/ToolCatalog.h"
#include "Integration/FileMigration.h"
#include <shobjidl.h>
#include <winrt/Windows.UI.Text.h>
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
    namespace {
        namespace capture = ::Aelura::Capture;
    }
    void MainWindow::Notice(hstring const& message, bool error) {
        Status().Title(error ? L"需要处理" : L"遥汐工具箱");
        Status().IsOpen(true); Status().Message(message); Status().Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
    }
    void MainWindow::Page(int index) {
        page_ = index;
        HomePage().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
        KeysPage().Visibility(index == 1 ? Visibility::Visible : Visibility::Collapsed);
        PreferencesPage().Visibility(index == 2 ? Visibility::Visible : Visibility::Collapsed);
        MovePage().Visibility(index == 3 ? Visibility::Visible : Visibility::Collapsed);
        const wchar_t* categories[] = {L"全部工具", L"图像处理", L"文件管理", L"游戏工具"};
        PageTitle().Text(index == 0 ? categories[category_] : index == 1 ? L"快捷键" : index == 2 ? L"设置" : L"文件迁移");
        PageDescription().Text(index == 0 ? L"找到现在需要的工具。" : index == 1 ? L"按操作管理常用按键组合。" : index == 2 ? L"让遥汐更适合你的习惯。" : L"释放磁盘空间，保留原路径访问。");
        ResultCount().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
        auto standard = Application::Current().Resources().Lookup(box_value(L"DefaultButtonStyle")).as<Style>();
        auto selected = Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>();
        HomeNav().Style(index == 0 && category_ == 0 ? selected : standard);
        ImageNav().Style(index == 0 && category_ == 1 ? selected : standard);
        FilesNav().Style((index == 0 && category_ == 2) || index == 3 ? selected : standard);
        GamesNav().Style(index == 0 && category_ == 3 ? selected : standard);
        KeysNav().Style(index == 1 ? selected : standard); PreferencesNav().Style(index == 2 ? selected : standard);
        if (index == 0) RenderTools(); if (index == 1) RenderKeys();
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
        hotkeys_.load(hwnd_, config_);
        int theme = GetPrivateProfileIntW(L"Appearance", L"Theme", 0, config_.c_str());
        if (theme < 0 || theme > 2) theme = 0;
        Root().RequestedTheme(static_cast<ElementTheme>(theme)); ThemeChoice().SelectedIndex(theme);
        auto titleTheme = [this] {
            const BOOL dark = Root().ActualTheme() == ElementTheme::Dark;
            DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        };
        Root().ActualThemeChanged([titleTheme](auto const&, auto const&) { titleTheme(); }); titleTheme();
        ThemeChoice().SelectionChanged([this](auto const&, auto const&) {
            int selected = ThemeChoice().SelectedIndex();
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            if (ec || !WritePrivateProfileStringW(L"Appearance", L"Theme", std::to_wstring(selected).c_str(), config_.c_str())) {
                Notice(L"主题设置无法保存。", true); return;
            }
            Root().RequestedTheme(static_cast<ElementTheme>(selected));
        });
        HomeNav().Click([this](auto const&, auto const&) { category_ = 0; Page(0); });
        ImageNav().Click([this](auto const&, auto const&) { category_ = 1; Page(0); });
        FilesNav().Click([this](auto const&, auto const&) { category_ = 2; Page(0); });
        GamesNav().Click([this](auto const&, auto const&) { category_ = 3; Page(0); });
        KeysNav().Click([this](auto const&, auto const&) { Page(1); });
        PreferencesNav().Click([this](auto const&, auto const&) { Page(2); });
        Search().TextChanged([this](auto const&, auto const&) { if (!Search().Text().empty() && page_ != 0) { category_ = 0; Page(0); } else RenderTools(); });
        KeySearch().TextChanged([this](auto const&, auto const&) { RenderKeys(); });
        SourceFileButton().Click([this](auto const&, auto const&) { PickPath(true, false); });
        SourceFolderButton().Click([this](auto const&, auto const&) { PickPath(true, true); });
        DestinationButton().Click([this](auto const&, auto const&) { PickPath(false, true); });
        CheckMoveButton().Click([this](auto const&, auto const&) { MoveFiles(true); });
        MoveButton().Click([this](auto const&, auto const&) { MoveFiles(false); });
        CancelMoveButton().Click([this](auto const&, auto const&) { cancelMove_ = true; });
        LicensesButton().Click([this](auto const&, auto const&) {
            ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.RequestedTheme(Root().RequestedTheme());
            dialog.Title(box_value(L"第三方许可")); dialog.CloseButtonText(L"关闭");
            dialog.Content(box_value(L"ScreenShotPlus · MIT License\nnlohmann/json · MIT License\nStrikeSense 的第三方依赖许可随附安装目录 Tools/Licenses。")); dialog.ShowAsync();
        });
        moveTimer_ = DispatcherQueue().CreateTimer(); moveTimer_.Interval(std::chrono::milliseconds(120));
        moveTimer_.Tick([this](auto const&, auto const&) { auto total = moveTotal_.load(); MoveProgress().Value(total ? 100.0 * moveDone_.load() / total : 0); });
        Page(0);
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
        if (msg == WM_CLOSE && self->moveBusy_) { self->Notice(L"请等待迁移完成，或先取消当前任务。", true); return 0; }
        if (msg == WM_GETMINMAXINFO) { auto info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {1000, 680}; return 0; }
        if (msg == WM_HOTKEY) {
            for (auto const& b : self->hotkeys_.bindings) if (b.registered && wp == static_cast<WPARAM>(b.slot)) {
                if (b.id == L"print") self->Invoke(1); else if (b.id == L"preview") self->Invoke(3); else self->ActivateTool(b.id);
                return 0;
            }
        }
        if (msg == WM_DESTROY) {
            if (self->timer_) self->timer_.Stop();
            self->hotkeys_.release();
            if (auto engine = self->EngineWindow()) PostMessageW(engine, capture::Message, capture::Shutdown, 0);
            RemovePropW(hwnd, L"AeluraTool.MainWindow");
            RemoveWindowSubclass(hwnd, WindowProc, id);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    MainWindow::~MainWindow() {
        if (timer_) timer_.Stop();
        if (strike_) CloseHandle(strike_);
        if (process_) CloseHandle(process_); if (job_) CloseHandle(job_); if (singleton_) CloseHandle(singleton_);
    }
}
