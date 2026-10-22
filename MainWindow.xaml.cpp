#include "pch.h"
#include "Integration/I18n.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>
#include <utility>
#include <algorithm>
#include "Integration/CaptureProtocol.h"
#include "Integration/ToolCatalog.h"
#include "Integration/FileMigration.h"
#include <shobjidl.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.System.h>
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
    namespace {
        namespace capture = ::Aelura::Capture;
    }
    void MainWindow::Notice(hstring const& message, bool error) {
        fileTaskNotice_ = false;
        Status().Title(error ? tr("common.error") : tr("app.name"));
        Status().IsOpen(true); Status().Message(message); Status().Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
    }
    void MainWindow::GoBack() {
        if (page_ != 3 && page_ != 4 && page_ != 5 && page_ != 6 && page_ != 7) return;
        navigationHistory_.push_back({{page_, category_, Search().Text()}, toolReturn_});
        auto previous = toolReturn_.value_or(NavigationEntry{0, 2, L""}); toolReturn_.reset();
        restoringNavigation_ = true;
        Search().Text(previous.query);
        restoringNavigation_ = false;
        Page(previous.page, previous.category, false);
        UpdateSearch(); Search().IsSuggestionListOpen(false);
        auto navigation = previous.page == 2 ? PreferencesNav() : previous.page == 1 ? KeysNav() :
            previous.category == 1 ? ImageNav() : previous.category == 3 ? GamesNav() : FilesNav();
        navigation.Focus(FocusState::Programmatic);
    }
    void MainWindow::GoHistoryBack() {
        if (navigationHistory_.empty()) return;
        auto previous = navigationHistory_.back(); navigationHistory_.pop_back();
        toolReturn_ = previous.toolReturn;
        restoringNavigation_ = true; Search().Text(previous.location.query); restoringNavigation_ = false;
        Page(previous.location.page, previous.location.category, false);
        UpdateSearch(); Search().IsSuggestionListOpen(false);
        if (!navigationHistory_.empty()) HistoryBackButton().Focus(FocusState::Programmatic);
    }
    void MainWindow::Page(int index, int category, bool remember) {
        const int nextCategory = category < 0 ? category_ : category;
        if (remember && (index != page_ || nextCategory != category_))
            navigationHistory_.push_back({{page_, category_, Search().Text()}, toolReturn_});
        if (remember && (index == 3 || index == 4 || index == 5 || index == 6 || index == 7) && page_ != 3 && page_ != 4 && page_ != 5 && page_ != 6 && page_ != 7)
            toolReturn_ = NavigationEntry{page_, category_, Search().Text()};
        category_ = nextCategory;
        page_ = index;
        HistoryBackButton().IsEnabled(!navigationHistory_.empty());
        HomePage().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
        KeysPage().Visibility(index == 1 ? Visibility::Visible : Visibility::Collapsed);
        PreferencesPage().Visibility(index == 2 ? Visibility::Visible : Visibility::Collapsed);
        MovePage().Visibility(index == 3 ? Visibility::Visible : Visibility::Collapsed);
        HashPage().Visibility(index == 4 ? Visibility::Visible : Visibility::Collapsed);
        AutoClickerPage().Visibility(index == 7 ? Visibility::Visible : Visibility::Collapsed);
        RecordingPage().Visibility(index == 6 ? Visibility::Visible : Visibility::Collapsed);
        FileSearchPage().Visibility(index == 5 ? Visibility::Visible : Visibility::Collapsed);
        if (index == 5) {
            if (!fileSearchPage_) {
                EnsureFileSearchIndex();
                fileSearchPage_ = std::make_unique<aelura::filesearch::SearchWindow>(fileSearchIndex_, false, hwnd_);
                FileSearchHost().Child(fileSearchPage_->content());
            }
            fileSearchPage_->show();
        } else if (fileSearchPage_) fileSearchPage_->hide();
        const wchar_t* categories[] = {tr("nav.all"), tr("nav.images"), tr("nav.files"), tr("nav.games")};
        PageTitle().Text(index == 0 ? categories[category_] : index == 1 ? tr("nav.keys") : index == 2 ? tr("nav.settings") : index == 7 ? tr("autoclicker.title") : index == 6 ? tr("record.title") : index == 5 ? tr("filesearch.title") : index == 4 ? tr("hash.title") : tr("move.title"));
        PageDescription().Text(index == 0 ? tr("home.description") : index == 1 ? tr("keys.description") : index == 2 ? tr("settings.description") : index == 7 ? tr("autoclicker.description") : index == 6 ? tr("record.description") : index == 5 ? tr("filesearch.description") : index == 4 ? tr("hash.description") : tr("move.description"));
        ResultCount().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
        auto standard = Application::Current().Resources().Lookup(box_value(L"DefaultButtonStyle")).as<Style>();
        auto selected = Root().Resources().Lookup(box_value(L"NavigationSelected")).as<Style>();
        ImageNav().Style(((index == 0 && category_ == 1) || index == 6) ? selected : standard);
        FilesNav().Style((index == 0 && category_ == 2) || index == 3 || index == 4 || index == 5 ? selected : standard);
        GamesNav().Style((index == 0 && category_ == 3) || index == 7 ? selected : standard);
        KeysNav().Style(index == 1 ? selected : standard); PreferencesNav().Style(index == 2 ? selected : standard);
        if (index == 0) RenderTools(); if (index == 1) RenderKeys(); if (index == 4) RenderHash();
        if (index == 2 && ready_) StartupSetting();
    }
    void MainWindow::Setup(bool startupActivation) {
        startupActivation_ = startupActivation;
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd_));
        wchar_t executablePath[32768]{}; GetModuleFileNameW(nullptr, executablePath, 32768);
        AppWindow().SetIcon((std::filesystem::path(executablePath).parent_path() / L"Assets" / L"App.ico").wstring());
        singleton_ = CreateMutexW(nullptr, FALSE, L"Local\\AeluraTool.Singleton");
        const auto singletonError = GetLastError();
        if (!singleton_ && singletonError != ERROR_ACCESS_DENIED) throw_last_error();
        if (singletonError == ERROR_ALREADY_EXISTS || singletonError == ERROR_ACCESS_DENIED) {
            if (!startupActivation_) EnumWindows([](HWND window, LPARAM)->BOOL {
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
        languagePreference_ = GetPrivateProfileIntW(L"Appearance", L"Language", 0, config_.c_str());
        if (languagePreference_ < 0 || languagePreference_ > 2) languagePreference_ = 0;
        aelura::i18n::select(languagePreference_);
        hotkeys_.load(hwnd_, config_);
        int theme = GetPrivateProfileIntW(L"Appearance", L"Theme", 0, config_.c_str());
        if (theme < 0 || theme > 2) theme = 0;
        Root().RequestedTheme(static_cast<ElementTheme>(theme)); ThemeChoice().SelectedIndex(theme);
        LanguageChoice().SelectedIndex(languagePreference_);
        StartupMotionSwitch().IsOn(GetPrivateProfileIntW(L"Appearance", L"StartupAnimation", 1, config_.c_str()) != 0);
        auto titleTheme = [this] {
            const BOOL dark = Root().ActualTheme() == ElementTheme::Dark;
            DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        };
        Root().ActualThemeChanged([this,titleTheme](auto const&, auto const&) { titleTheme(); if(recording_)recording_->theme(Root().RequestedTheme()); }); titleTheme();
        Root().SizeChanged([this](auto const&, auto const&) { FileSearchHost().Height((std::max)(500.0, Root().ActualHeight() - 320.0)); });
        ThemeChoice().SelectionChanged([this](auto const&, auto const&) {
            if (localizing_) return;
            int selected = ThemeChoice().SelectedIndex();
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            if (ec || !WritePrivateProfileStringW(L"Appearance", L"Theme", std::to_wstring(selected).c_str(), config_.c_str())) {
                Notice(tr("error.theme_save"), true); return;
            }
            if(toggleNotification_)toggleNotification_->close();
            Root().RequestedTheme(static_cast<ElementTheme>(selected));if(recording_)recording_->theme(Root().RequestedTheme());
            if (fileSearch_) fileSearch_->appearance(Root().RequestedTheme());
        });
        LanguageChoice().SelectionChanged([this](auto const&, auto const&) {
            if (localizing_) return;
            const int preference = LanguageChoice().SelectedIndex();
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            if (ec || !WritePrivateProfileStringW(L"Appearance", L"Language", std::to_wstring(preference).c_str(), config_.c_str())) {
                localizing_ = true; LanguageChoice().SelectedIndex(languagePreference_); localizing_ = false;
                Notice(tr("error.preference_save"), true); return;
            }
            languagePreference_ = preference; aelura::i18n::select(preference); ApplyLanguage();
        });
        StartupMotionSwitch().Toggled([this](auto const&, auto const&) {
            if (localizing_) return;
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            if (ec || !WritePrivateProfileStringW(L"Appearance", L"StartupAnimation", StartupMotionSwitch().IsOn() ? L"1" : L"0", config_.c_str())) Notice(tr("error.preference_save"), true);
        });
        PreviewMotionButton().Click([this](auto const&, auto const&) { PlayStartup(true); });
        Root().Loaded([this](auto const&, auto const&) { PlayStartup(); });
        Root().AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler([this](auto const&, auto const&) { FinishStartup(); })), true);
        Root().AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, auto const&) { FinishStartup(); })), true);
        Root().PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& args) { if (elevationBusy_) args.Handled(true); });
        HashBackButton().Click([this](auto const&, auto const&) { GoBack(); });
        MoveBackButton().Click([this](auto const&, auto const&) { GoBack(); });
        FileSearchBackButton().Click([this](auto const&, auto const&) { GoBack(); });
        HistoryBackButton().Click([this](auto const&, auto const&) { GoHistoryBack(); });
        Input::KeyboardAccelerator backKey;
        backKey.Key(Windows::System::VirtualKey::Left);
        backKey.Modifiers(Windows::System::VirtualKeyModifiers::Menu);
        backKey.Invoked([this](auto const&, Input::KeyboardAcceleratorInvokedEventArgs const& args) { GoHistoryBack(); args.Handled(true); });
        Root().KeyboardAccelerators().Append(backKey);
        ImageNav().Click([this](auto const&, auto const&) { Page(0, 1); });
        FilesNav().Click([this](auto const&, auto const&) { Page(0, 2); });
        GamesNav().Click([this](auto const&, auto const&) { Page(0, 3); });
        KeysNav().Click([this](auto const&, auto const&) { Page(1); });
        PreferencesNav().Click([this](auto const&, auto const&) { Page(2); });
        SetupSearch();
        KeySearch().TextChanged([this](auto const&, auto const&) { RenderKeys(); });
        SourceFileButton().Click([this](auto const&, auto const&) { PickPath(true, false); });
        SourceFolderButton().Click([this](auto const&, auto const&) { PickPath(true, true); });
        DestinationButton().Click([this](auto const&, auto const&) { PickPath(false, true); });
        CheckMoveButton().Click([this](auto const&, auto const&) { MoveFiles(true); });
        MoveButton().Click([this](auto const&, auto const&) { MoveFiles(false); });
        CancelMoveButton().Click([this](auto const&, auto const&) { cancelMove_ = true; });
        LicensesButton().Click([this](auto const&, auto const&) {
            ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.RequestedTheme(Root().RequestedTheme());
            dialog.Title(box_value(tr("settings.licenses"))); dialog.CloseButtonText(tr("common.close"));
            dialog.Content(box_value(tr("licenses.body"))); dialog.ShowAsync();
        });
        moveTimer_ = DispatcherQueue().CreateTimer(); moveTimer_.Interval(std::chrono::milliseconds(120));
        moveTimer_.Tick([this](auto const&, auto const&) { auto total = moveTotal_.load(); MoveProgress().Value(total ? 100.0 * moveDone_.load() / total : 0); });
        recording_ = std::make_unique<aelura::recording::Panel>(hwnd_,config_.parent_path()/L"recording.json",[this]{RestoreWindow();Page(6);if(recording_&&!recording_->busy())Status().IsOpen(false);},[this]{auto host=EngineWindow();if(!host&&!pending_)Invoke(capture::Prepare);return host;});
        RecordingHost().Content(recording_->content());recording_->theme(Root().RequestedTheme());
        RecordingBackButton().Click([this](auto const&,auto const&){GoBack();});
        SetupAutoClicker(); SetupHash(); SetupLifecycle(); ApplyLanguage(); ready_ = true;
        timer_ = DispatcherQueue().CreateTimer(); timer_.Interval(std::chrono::milliseconds(100));
        timer_.Tick([this](auto const&, auto const&) {
            if (HWND engine = EngineWindow()) {
                timer_.Stop();
                const UINT command = std::exchange(pending_, 0);
                DWORD_PTR result = 0;
                SendMessageTimeoutW(engine, capture::Message, capture::SetLanguage, aelura::i18n::chinese() ? 1 : 2, SMTO_ABORTIFHUNG, 500, &result);
                if (command == capture::Start) {
                    if (!SendMessageTimeoutW(engine, capture::Message, capture::CanCapture, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) || result != capture::Accepted) {
                        Notice(tr("capture.busy_settings"), true);
                        return;
                    }
                    ShowWindow(hwnd_, SW_MINIMIZE);
                }
                if (!SendMessageTimeoutW(engine, capture::Message, command, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
                    if (command == capture::Start) ShowWindow(hwnd_, SW_RESTORE);
                    Notice(tr("capture.no_response"), true);
                } else if (result != capture::Accepted) {
                    if (command == capture::Start) ShowWindow(hwnd_, SW_RESTORE);
                    Notice(tr("capture.busy"), true);
                } else if(command != capture::Prepare) Notice(command == capture::Start ? tr("capture.started") : command == capture::Settings ? tr("capture.settings_open") : tr("capture.recent_open"));
                pending_ = 0;
            }
            else if (++attempts_ >= 100 || (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)) {
                timer_.Stop(); pending_ = 0; Notice(tr("capture.not_ready"), true);
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
        if (elevationBusy_) return;
        if (pending_) { Notice(tr("capture.starting")); return; }
        if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
            if (process_) { CloseHandle(process_); process_ = nullptr; }
            if (job_) { CloseHandle(job_); job_ = nullptr; }
            wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
            auto exe = std::filesystem::path(path).parent_path() / L"Tools" / L"ScreenShotPlus.Host.exe";
            std::wstring args = L"\"" + exe.wstring() + L"\" --aelura-host";
            job_ = CreateJobObjectW(nullptr, nullptr); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { Notice(tr("capture.manager_error"), true); return; }
            STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, exe.parent_path().c_str(), &startup, &process)) {
                Notice(tr("capture.missing"), true); return;
            }
            if (!AssignProcessToJobObject(job_, process.hProcess)) { TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess); Notice(tr("capture.manage_error"), true); return; }
            process_ = process.hProcess; pid_ = process.dwProcessId;
            if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
                TerminateProcess(process_, 1); CloseHandle(process_); process_ = nullptr;
                CloseHandle(process.hThread); Notice(tr("capture.start_error"), true); return;
            }
            CloseHandle(process.hThread);
        }
        pending_ = command; attempts_ = 0; if(command != capture::Prepare)Notice(tr("capture.opening")); timer_.Start();
    }
    LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<MainWindow*>(data);
        if (msg == aelura::desktop::TrayMessage) {
            const auto event = LOWORD(lp);
            if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) self->RestoreWindow();
            else if (event == WM_CONTEXTMENU) {
                const auto command = self->tray_.Menu();
                if (command == 1) self->RestoreWindow();
                if (command == 2) self->RequestClose(true);
            }
            return 0;
        }
        if (self->tray_.TaskbarCreatedMessage() && msg == self->tray_.TaskbarCreatedMessage()) {
            if (!self->tray_.Ensure() && !IsWindowVisible(hwnd)) self->RestoreWindow();
            return 0;
        }
        if (msg == WM_CLOSE && self->fileTask_ != FileTask::None) {
            self->Notice(tr("files.wait"), true);
            self->fileTaskNotice_ = true;
            if (self->fileTask_ == FileTask::Hash) self->Page(4);
            return 0;
        }
        if (msg == WM_GETMINMAXINFO) { auto info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {1000, 680}; return 0; }
        if (msg == WM_CLOSE) { self->RequestClose(); return 0; }
        if (msg == WM_HOTKEY) {
            if(self->recordingCloseBusy_ || self->elevationBusy_)return 0;
            self->FinishStartup();
            for (auto const& b : self->hotkeys_.bindings) if (b.registered && wp == static_cast<WPARAM>(b.slot)) {
                if(b.id==L"autoclicker")self->ToggleAutoClicker(true);else if(b.id==L"record")self->recording_->toggle();else if(b.id==L"replay-save")self->recording_->replaySave();
                else if (b.id == L"filesearch") self->ShowFileSearch(); else if (b.id == L"print") self->Invoke(1); else if (b.id == L"preview") self->Invoke(3); else self->ActivateTool(b.id);
                if(b.id!=L"autoclicker" && self->hotkeyNotifications_ && self->toggleNotification_)self->toggleNotification_->action(b.title,self->Root().RequestedTheme());
                return 0;
            }
        }
        if (msg == WM_DESTROY) {
            if(self->autoClickerTimer_)self->autoClickerTimer_.Stop();
            self->autoClicker_.stop();
            if(self->toggleNotification_)self->toggleNotification_->close();
            if (self->fileSearch_) self->fileSearch_->close();
            if (self->fileSearchPage_) self->fileSearchPage_->close();
            self->tray_.Remove();
            if (self->hashTimer_) self->hashTimer_.Stop();
            if (self->timer_) self->timer_.Stop();
            self->hotkeys_.release();
            if (auto engine = self->EngineWindow()) PostMessageW(engine, capture::Message, capture::Shutdown, 0);
            RemovePropW(hwnd, L"AeluraTool.MainWindow");
            RemoveWindowSubclass(hwnd, WindowProc, id);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    MainWindow::~MainWindow() {
        if (hashTimer_) hashTimer_.Stop();
        if (timer_) timer_.Stop();
        if (strike_) CloseHandle(strike_);
        if (process_) CloseHandle(process_); if (job_) CloseHandle(job_); if (singleton_) CloseHandle(singleton_);
    }
}
