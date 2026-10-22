#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include "Integration/FileShellActions.h"
#include "Integration/Privileges.h"
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
bool MainWindow::SaveBehavior(wchar_t const* key, int value) {
    std::error_code error; std::filesystem::create_directories(config_.parent_path(), error);
    if (error || !WritePrivateProfileStringW(L"Behavior", key, std::to_wstring(value).c_str(), config_.c_str())) {
        Notice(tr("error.preference_save"), true); return false;
    }
    return true;
}
void MainWindow::SetupLifecycle() {
    closeAction_ = GetPrivateProfileIntW(L"Behavior", L"CloseAction", 0, config_.c_str());
    if (closeAction_ < 0 || closeAction_ > 2) closeAction_ = 0;
    CloseActionChoice().SelectedIndex(closeAction_);
    tray_.Initialize(hwnd_);
    PrivilegeBanner().IsOpen(!aelura::privileges::elevated());
    ElevateButton().Click([this](auto const&, auto const&) { Elevate(); });
    PrivilegeBanner().Tapped([this](auto const&, Input::TappedRoutedEventArgs const& args) {
        auto source = args.OriginalSource().try_as<DependencyObject>();
        while (source && source != PrivilegeBanner()) {
            if (source.try_as<Button>()) return; // The action button handles keyboard activation too.
            source = Media::VisualTreeHelper::GetParent(source);
        }
        Elevate();
    });
    AutoElevateSwitch().IsOn(GetPrivateProfileIntW(L"Behavior", L"AutoElevate", 0, config_.c_str()) != 0);
    AutoElevateSwitch().Toggled([this](auto const&, auto const&) {
        if (localizing_) return;
        if (!SaveBehavior(L"AutoElevate", AutoElevateSwitch().IsOn() ? 1 : 0)) {
            localizing_ = true; AutoElevateSwitch().IsOn(!AutoElevateSwitch().IsOn()); localizing_ = false;
        }
    });
    CloseActionChoice().SelectionChanged([this](auto const&, auto const&) {
        if (localizing_) return;
        const int selected = CloseActionChoice().SelectedIndex();
        if (selected < 0 || selected > 2) return;
        if (SaveBehavior(L"CloseAction", selected)) closeAction_ = selected;
        else { localizing_ = true; CloseActionChoice().SelectedIndex(closeAction_); localizing_ = false; }
    });
    AutoStartSwitch().Toggled([this](auto const&, auto const&) { if (!localizing_ && !startupBusy_) StartupSetting(AutoStartSwitch().IsOn()); });
    StartupSetting();
}
void MainWindow::ShowInitial() {
    if (startupActivation_ && tray_.Ensure()) {
        startupPlayed_ = true; FinishStartup(); ShowWindow(hwnd_, SW_HIDE);
    } else { Activate(); if (startupActivation_) Notice(tr("tray.failed"), true); }
}
void MainWindow::RestoreWindow() {
    ShowWindow(hwnd_, SW_RESTORE); Activate(); SetForegroundWindow(hwnd_);
}
bool MainWindow::HideToTray() {
    if (!tray_.Ensure()) { RestoreWindow(); Notice(tr("tray.failed"), true); return false; }
    FinishStartup(); ShowWindow(hwnd_, SW_HIDE); return true;
}
void MainWindow::LocalizeLifecycle() {
    BehaviorTitle().Text(tr("settings.behavior"));
    CloseActionChoice().Header(box_value(tr("close.action")));
    CloseAskOption().Content(box_value(tr("close.ask")));
    CloseExitOption().Content(box_value(tr("close.exit")));
    CloseTrayOption().Content(box_value(tr("close.tray")));
    auto selected = CloseActionChoice().SelectedIndex();
    CloseActionChoice().SelectedIndex(-1); CloseActionChoice().SelectedIndex(selected);
    AutoStartSwitch().Header(box_value(tr("startup.auto")));
    AutoElevateSwitch().Header(box_value(tr("privilege.auto")));
    AutoElevateHint().Text(tr("privilege.auto_hint"));
    PrivilegeBanner().Title(tr("privilege.title")); PrivilegeBanner().Message(tr("privilege.hint"));
    ElevateLabel().Text(tr("privilege.action"));
    for (auto toggle : {AutoStartSwitch(), AutoElevateSwitch()}) {
        toggle.OnContent(box_value(tr("common.enabled"))); toggle.OffContent(box_value(tr("common.disabled")));
    }
    tray_.Localize();
}
fire_and_forget MainWindow::RequestClose(bool explicitExit) {
    auto lifetime = get_strong();
    if(recordingCloseBusy_)co_return;
    if (elevationBusy_) co_return;
    if (aelura::files::active()) { RestoreWindow(); Page(5); Notice(tr("filesearch.wait_files"), true); co_return; }
    if (fileSearchIndex_ && fileSearchIndex_->progress().running) { RestoreWindow(); Page(5); Notice(tr("filesearch.wait_index"), true); co_return; }
    if (fileTask_ != FileTask::None) { RestoreWindow(); Notice(tr("files.wait"), true); co_return; }
    if (closeDialogOpen_) { RestoreWindow(); co_return; }
    int action = explicitExit ? 1 : closeAction_;
    if (!action) {
        closeDialogOpen_ = true; RestoreWindow();
        try {
            ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.RequestedTheme(Root().RequestedTheme());
            dialog.Title(box_value(tr("close.title")));
            dialog.PrimaryButtonText(tr("close.tray")); dialog.SecondaryButtonText(tr("close.exit"));
            dialog.CloseButtonText(tr("common.cancel")); dialog.DefaultButton(ContentDialogButton::Primary);
            StackPanel content; content.Spacing(16); TextBlock hint; hint.Text(tr("close.hint")); hint.TextWrapping(TextWrapping::Wrap);
            CheckBox remember; remember.Content(box_value(tr("close.remember"))); content.Children().Append(hint); content.Children().Append(remember); dialog.Content(content);
            auto result = co_await dialog.ShowAsync(); closeDialogOpen_ = false;
            if (result == ContentDialogResult::None) co_return;
            action = result == ContentDialogResult::Primary ? 2 : 1;
            // Recheck after the asynchronous dialog: a hotkey may have started a file task.
            if (fileTask_ != FileTask::None) { Notice(tr("files.wait"), true); co_return; }
            if (aelura::files::active()) { Page(5); Notice(tr("filesearch.wait_files"), true); co_return; }
            if (fileSearchIndex_ && fileSearchIndex_->progress().running) { Page(5); Notice(tr("filesearch.wait_index"), true); co_return; }
            if (remember.IsChecked().Value()) {
                if (!SaveBehavior(L"CloseAction", action)) co_return;
                closeAction_ = action; localizing_ = true; CloseActionChoice().SelectedIndex(action); localizing_ = false;
            }
        } catch (hresult_error const&) { closeDialogOpen_ = false; Notice(tr("close.dialog_failed"), true); co_return; }
    }
    if (action == 2) HideToTray(); else {
        recordingCloseBusy_=true;ContentShell().IsHitTestVisible(false);hotkeys_.release();
        if(recording_){Notice(tr("record.exiting"));co_await recording_->shutdown();}
        RecordingHost().Content(nullptr);recording_.reset();Close();
    }
}
fire_and_forget MainWindow::Elevate() {
    if (elevationBusy_ || aelura::privileges::elevated()) co_return;
    if (fileTask_ != FileTask::None || aelura::files::active() || closeDialogOpen_ || startupBusy_ || recordingCloseBusy_ || (recording_ && recording_->busy())) { Notice(tr("privilege.wait"), true); co_return; }
    auto lifetime = get_strong(); elevationBusy_ = true;
    Root().IsHitTestVisible(false); if (fileSearch_) fileSearch_->hide();
    winrt::apartment_context ui;
    const auto owner = hwnd_;
    // Shell activation may wait on the new process; never block this dispatcher.
    co_await winrt::resume_background();
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto launch = aelura::privileges::restart(owner);
    if (SUCCEEDED(initialized)) CoUninitialize();
    co_await ui;
    bool ready = false;
    if (launch.process) {
        auto deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline && WaitForSingleObject(launch.process, 0) == WAIT_TIMEOUT) {
            if (WaitForSingleObject(launch.ready, 0) == WAIT_OBJECT_0) { ready = true; break; }
            co_await winrt::resume_after(std::chrono::milliseconds(50));
        }
        co_await ui;
        CloseHandle(launch.process); CloseHandle(launch.ready);
    }
    if (ready) {
        if (fileSearchIndex_) fileSearchIndex_->cancel();
        if (recording_) co_await recording_->shutdown();
        RecordingHost().Content(nullptr); recording_.reset();
        hotkeys_.release();
        Close();
        // The successor waits for the process, not just the main HWND. Auxiliary
        // XAML windows must not keep the predecessor's singleton alive.
        Application::Current().Exit(); co_return;
    }
    Root().IsHitTestVisible(true); elevationBusy_ = false;
    if (launch.error != ERROR_CANCELLED) Notice(tr("privilege.failed"), true);
}
fire_and_forget MainWindow::StartupSetting(std::optional<bool> enable) {
    if (startupBusy_) co_return;
    auto lifetime = get_strong(); startupBusy_ = true; AutoStartSwitch().IsEnabled(false);
    winrt::apartment_context ui;
    using namespace Windows::ApplicationModel;
    try {
        auto task = co_await StartupTask::GetAsync(L"AeluraStartup");
        auto state = task.State();
        const bool scheduled = aelura::privileges::startupEnabled();
        bool configured = true;
        if (enable && (*enable != scheduled)) {
            if (aelura::privileges::elevated()) configured = aelura::privileges::setStartup(*enable);
            else {
                auto launch = aelura::privileges::configureStartup(hwnd_, *enable);
                configured = false;
                if (launch.process) {
                    while (WaitForSingleObject(launch.process, 0) == WAIT_TIMEOUT) co_await winrt::resume_after(std::chrono::milliseconds(100));
                    co_await ui;
                    DWORD code{}; configured = GetExitCodeProcess(launch.process, &code) && code == 0; CloseHandle(launch.process);
                }
            }
        }
        if (configured && enable) {
            // Retire the legacy packaged startup entry only after the scheduled task succeeds.
            if (state == StartupTaskState::Enabled) { task.Disable(); state = task.State(); }
        }
        const bool scheduledNow = aelura::privileges::startupEnabled();
        const bool active = scheduledNow || state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
        localizing_ = true; AutoStartSwitch().IsOn(active); localizing_ = false;
        AutoStartState().Text(scheduledNow ? tr("startup.admin_hint") : state == StartupTaskState::DisabledByUser ? tr("startup.blocked_user") :
            state == StartupTaskState::DisabledByPolicy || state == StartupTaskState::EnabledByPolicy ? tr("startup.policy") : tr("startup.hint"));
        AutoStartSwitch().IsEnabled(scheduledNow || (state != StartupTaskState::DisabledByPolicy && state != StartupTaskState::EnabledByPolicy));
        if (enable && (!configured || active != *enable)) Notice(tr("startup.not_changed"), true);
    } catch (hresult_error const&) {
        localizing_ = true; AutoStartSwitch().IsOn(false); localizing_ = false;
        AutoStartState().Text(tr("startup.failed")); AutoStartSwitch().IsEnabled(false);
    }
    startupBusy_ = false;
}
}
