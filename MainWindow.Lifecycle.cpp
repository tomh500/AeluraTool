#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include <winrt/Windows.ApplicationModel.h>
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
    for (auto toggle : {AutoStartSwitch()}) {
        toggle.OnContent(box_value(tr("common.enabled"))); toggle.OffContent(box_value(tr("common.disabled")));
    }
    tray_.Localize();
}
fire_and_forget MainWindow::RequestClose(bool explicitExit) {
    auto lifetime = get_strong();
    if(recordingCloseBusy_)co_return;
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
fire_and_forget MainWindow::StartupSetting(std::optional<bool> enable) {
    if (startupBusy_) co_return;
    auto lifetime = get_strong(); startupBusy_ = true; AutoStartSwitch().IsEnabled(false);
    using namespace Windows::ApplicationModel;
    try {
        auto task = co_await StartupTask::GetAsync(L"AeluraStartup");
        auto state = task.State();
        if (enable) {
            if (*enable && state == StartupTaskState::Disabled) state = co_await task.RequestEnableAsync();
            else if (!*enable && state == StartupTaskState::Enabled) { task.Disable(); state = task.State(); }
        }
        const bool active = state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
        localizing_ = true; AutoStartSwitch().IsOn(active); localizing_ = false;
        AutoStartState().Text(state == StartupTaskState::DisabledByUser ? tr("startup.blocked_user") :
            state == StartupTaskState::DisabledByPolicy || state == StartupTaskState::EnabledByPolicy ? tr("startup.policy") : tr("startup.hint"));
        AutoStartSwitch().IsEnabled(state != StartupTaskState::DisabledByPolicy && state != StartupTaskState::EnabledByPolicy);
        if (enable && active != *enable) Notice(tr("startup.not_changed"), true);
    } catch (hresult_error const&) {
        localizing_ = true; AutoStartSwitch().IsOn(false); localizing_ = false;
        AutoStartState().Text(tr("startup.failed")); AutoStartSwitch().IsEnabled(false);
    }
    startupBusy_ = false;
}
}
