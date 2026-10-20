#include "pch.h"
#include "Integration/I18n.h"
#include "MainWindow.xaml.h"
#include "Integration/ToolCatalog.h"
#include "Integration/FileMigration.h"
#include <shobjidl.h>
#include <winrt/Windows.UI.Text.h>
#include <sstream>
#include <iomanip>
#include <optional>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
namespace {
TextBlock text(std::wstring const& value, double size = 14) { TextBlock t; t.Text(value); t.FontSize(size); t.TextWrapping(TextWrapping::Wrap); return t; }
ColumnDefinition column(GridLength length) { ColumnDefinition c; c.Width(length); return c; }
std::wstring message(std::exception const& ex) { return to_hstring(ex.what()).c_str(); }
std::wstring format(aelura::migration::Plan const& p) {
    std::wostringstream s; s << tr("move.destination_prefix") << p.destination.wstring() << L"\n" << std::fixed << std::setprecision(1) << (p.bytes / 1048576.0) << L" MB · " << (p.sameVolume ? tr("move.same_volume") : tr("move.cross_volume")); return s.str();
}
}
void MainWindow::RenderTools() {
    ToolCards().Children().Clear(); unsigned count = 0;
    for (auto const& tool : aelura::catalog::tools) {
        if (category_ && category_ != tool.category) continue;
        ++count; Border card; card.Style(Root().Resources().Lookup(box_value(L"Card")).as<Style>());
        Grid grid; grid.ColumnSpacing(20); grid.ColumnDefinitions().Append(column({48, GridUnitType::Pixel})); grid.ColumnDefinitions().Append(column({1, GridUnitType::Star})); grid.ColumnDefinitions().Append(column({1, GridUnitType::Auto}));
        FontIcon icon; icon.Glyph(tool.glyph); icon.FontSize(28); icon.VerticalAlignment(VerticalAlignment::Center); grid.Children().Append(icon);
        StackPanel copy; copy.Spacing(6); auto title = text(tr(tool.title), 19); title.FontWeight(Windows::UI::Text::FontWeights::SemiBold()); copy.Children().Append(title); copy.Children().Append(text(tr(tool.description)));
        const wchar_t* names[] = {L"", tr("nav.images"), tr("nav.files"), tr("nav.games")}; auto category = text(names[tool.category], 12); copy.Children().Append(category); Grid::SetColumn(copy, 1); grid.Children().Append(copy);
        StackPanel actions; actions.Spacing(8); actions.VerticalAlignment(VerticalAlignment::Center);
        Button open; open.Content(box_value(tr(tool.action))); auto id = std::wstring(tool.id); open.Click([this, id](auto const&, auto const&) { ActivateTool(id); }); actions.Children().Append(open);
        if (id == L"capture") {
            Button settings; settings.Content(box_value(tr("capture.settings"))); settings.Click([this](auto const&, auto const&) { Invoke(2); }); actions.Children().Append(settings);
            Button recent; recent.Content(box_value(tr("capture.recent"))); recent.Click([this](auto const&, auto const&) { Invoke(3); }); actions.Children().Append(recent);
        }
        Grid::SetColumn(actions, 2); grid.Children().Append(actions); card.Child(grid); ToolCards().Children().Append(card);
    }
    NoResults().Visibility(count ? Visibility::Collapsed : Visibility::Visible); ResultCount().Text(std::to_wstring(count) + tr(count == 1 ? "tools.count_one" : "tools.count_suffix"));
}
void MainWindow::RenderKeys() {
    KeyRows().Children().Clear(); unsigned count = 0;
    for (size_t i = 0; i < hotkeys_.bindings.size(); ++i) {
        auto const& b = hotkeys_.bindings[i]; auto keys = aelura::hotkeys::label(b);
        if (!aelura::catalog::matches(b.title + L" " + b.category + L" " + keys, std::wstring(KeySearch().Text()))) continue;
        ++count; Border card; card.Style(Root().Resources().Lookup(box_value(L"Card")).as<Style>());
        Grid row; row.ColumnSpacing(18); row.ColumnDefinitions().Append(column({1, GridUnitType::Star})); row.ColumnDefinitions().Append(column({180, GridUnitType::Pixel})); row.ColumnDefinitions().Append(column({1, GridUnitType::Auto}));
        StackPanel labels; labels.Spacing(4); labels.Children().Append(text(b.title, 16)); labels.Children().Append(text(b.category, 12)); row.Children().Append(labels);
        StackPanel state; state.Spacing(4); state.Children().Append(text(keys)); state.Children().Append(text(!b.enabled ? tr("common.disabled") : b.registered ? tr("common.enabled") : tr("common.occupied"), 12)); Grid::SetColumn(state, 1); row.Children().Append(state);
        Button edit; edit.Content(box_value(tr("common.edit"))); edit.VerticalAlignment(VerticalAlignment::Center); edit.Click([this, i](auto const&, auto const&) { EditKey(i); }); Grid::SetColumn(edit, 2); row.Children().Append(edit);
        card.Child(row); KeyRows().Children().Append(card);
    }
    NoKeys().Visibility(count ? Visibility::Collapsed : Visibility::Visible);
}
fire_and_forget MainWindow::EditKey(size_t index) {
    auto lifetime = get_strong(); auto const b = hotkeys_.bindings.at(index);
    ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.RequestedTheme(Root().RequestedTheme()); dialog.Title(box_value(b.title));
    dialog.PrimaryButtonText(tr("common.save")); dialog.CloseButtonText(tr("common.cancel")); dialog.DefaultButton(ContentDialogButton::Primary);
    StackPanel panel; panel.Spacing(16); ToggleSwitch enabled; enabled.Header(box_value(tr("keys.enable"))); enabled.IsOn(b.enabled); panel.Children().Append(enabled);
    StackPanel mods; mods.Orientation(Orientation::Horizontal); mods.Spacing(12);
    CheckBox ctrl, alt, shift, win; ctrl.Content(box_value(L"Ctrl")); alt.Content(box_value(L"Alt")); shift.Content(box_value(L"Shift")); win.Content(box_value(L"Win"));
    ctrl.IsChecked((b.modifiers & MOD_CONTROL) != 0); alt.IsChecked((b.modifiers & MOD_ALT) != 0); shift.IsChecked((b.modifiers & MOD_SHIFT) != 0); win.IsChecked((b.modifiers & MOD_WIN) != 0);
    for (auto c : {ctrl, alt, shift, win}) mods.Children().Append(c);
    ComboBox key; key.Header(box_value(tr("common.key"))); key.HorizontalAlignment(HorizontalAlignment::Stretch); std::vector<UINT> values;
    for (UINT k = 'A'; k <= 'Z'; ++k) values.push_back(k); for (UINT k = '0'; k <= '9'; ++k) values.push_back(k); for (UINT k = VK_F1; k <= VK_F24; ++k) values.push_back(k);
    if (b.id == L"print") panel.Children().Append(text(L"PrintScreen"));
    else { panel.Children().Append(mods); for (auto k : values) { auto sample = b; sample.modifiers = 0; sample.key = k; key.Items().Append(box_value(aelura::hotkeys::label(sample))); if (k == b.key) key.SelectedIndex(static_cast<int>(key.Items().Size()) - 1); } if (key.SelectedIndex() < 0) key.SelectedIndex(0); panel.Children().Append(key); }
    TextBlock error; error.TextWrapping(TextWrapping::Wrap); panel.Children().Append(error); dialog.Content(panel);
    dialog.PrimaryButtonClick([&](auto const&, ContentDialogButtonClickEventArgs const& args) {
        UINT modifiers = b.id == L"print" ? 0 : (ctrl.IsChecked().Value() ? MOD_CONTROL : 0) | (alt.IsChecked().Value() ? MOD_ALT : 0) | (shift.IsChecked().Value() ? MOD_SHIFT : 0) | (win.IsChecked().Value() ? MOD_WIN : 0);
        UINT k = b.id == L"print" ? VK_SNAPSHOT : values.at(key.SelectedIndex()); std::wstring issue;
        if (!hotkeys_.save(index, modifiers, k, enabled.IsOn(), issue)) { error.Text(issue); args.Cancel(true); }
    });
    try { co_await dialog.ShowAsync(); RenderKeys(); } catch (hresult_error const& e) { Notice(e.message(), true); }
}
void MainWindow::ActivateTool(std::wstring const& id) {
    if (id == L"capture") Invoke(1);
    else if (id == L"move") { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); Page(3); }
    else if (id == L"hash") { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); Page(4); }
    else if (id == L"record") { RestoreWindow(); Page(5); }
    else if (id == L"strike") LaunchStrike();
}
void MainWindow::LaunchStrike() {
    if (strike_ && WaitForSingleObject(strike_, 0) == WAIT_TIMEOUT) {
        DWORD pid = GetProcessId(strike_);
        EnumWindows([](HWND w, LPARAM data)->BOOL { DWORD process{}; GetWindowThreadProcessId(w, &process); if (process == static_cast<DWORD>(data) && GetPropW(w, L"Aelura.Strike.Main")) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); return FALSE; } return TRUE; }, pid); return;
    }
    if (strike_) { CloseHandle(strike_); strike_ = nullptr; }
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768); auto exe = std::filesystem::path(path).parent_path() / L"Tools" / L"StrikeSense" / L"StrikeSense.exe";
    std::wstring args = L"\"" + exe.wstring() + L"\" --aelura-host " + (aelura::i18n::chinese() ? L"--language=zh" : L"--language=en"); STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &startup, &pi)) { Notice(tr("strike.error"), true); return; }
    strike_ = pi.hProcess; CloseHandle(pi.hThread);
}
void MainWindow::PickPath(bool source, bool folder) {
    if (fileTask_ != FileTask::None) return;
    try {
        com_ptr<IFileOpenDialog> dialog; check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{}; check_hresult(dialog->GetOptions(&options)); check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)));
        auto result = dialog->Show(hwnd_); if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return; check_hresult(result);
        com_ptr<IShellItem> item; check_hresult(dialog->GetResult(item.put())); PWSTR path{}; check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        (source ? SourcePath() : DestinationPath()).Text(path); CoTaskMemFree(path);
    } catch (hresult_error const& e) { Notice(e.message(), true); }
}
fire_and_forget MainWindow::MoveFiles(bool inspectOnly) {
    if (!AcquireFileTask(FileTask::Migration)) co_return; auto lifetime = get_strong();
    auto source = std::filesystem::path(SourcePath().Text().c_str()), destination = std::filesystem::path(DestinationPath().Text().c_str());
    moveBusy_ = true; cancelMove_ = false; moveDone_ = 0; moveTotal_ = 0;
    auto controls = [&](bool enabled) { LanguageChoice().IsEnabled(enabled); SourcePath().IsEnabled(enabled); DestinationPath().IsEnabled(enabled); SourceFileButton().IsEnabled(enabled); SourceFolderButton().IsEnabled(enabled); DestinationButton().IsEnabled(enabled); CheckMoveButton().IsEnabled(enabled); MoveButton().IsEnabled(enabled); };
    controls(false); MoveProgress().Visibility(Visibility::Visible); MoveProgress().IsIndeterminate(true);
    std::optional<aelura::migration::Plan> plan; std::wstring issue; auto queue = DispatcherQueue();
    co_await resume_background();
    try { plan = aelura::migration::inspect(source, destination); } catch (std::exception const& e) { issue = message(e); }
    co_await wil::resume_foreground(queue);
    if (plan) MoveSummary().Text(format(*plan));
    if (!plan || inspectOnly) { moveBusy_ = false; ReleaseFileTask(); controls(true); MoveProgress().Visibility(Visibility::Collapsed); if (!issue.empty()) Notice(hstring(issue), true); co_return; }
    ContentDialog confirm; confirm.XamlRoot(Root().XamlRoot()); confirm.RequestedTheme(Root().RequestedTheme()); confirm.Title(box_value(tr("move.confirm"))); confirm.Content(box_value(format(*plan) + tr("move.close_programs"))); confirm.PrimaryButtonText(tr("move.proceed")); confirm.CloseButtonText(tr("common.cancel"));
    ContentDialogResult choice{}; try { choice = co_await confirm.ShowAsync(); } catch (hresult_error const& e) { Notice(e.message(), true); }
    if (choice != ContentDialogResult::Primary) { moveBusy_ = false; ReleaseFileTask(); controls(true); MoveProgress().Visibility(Visibility::Collapsed); co_return; }
    CancelMoveButton().Visibility(Visibility::Visible); MoveProgress().IsIndeterminate(false); moveTimer_.Start();
    co_await resume_background();
    auto result = aelura::migration::execute(*plan, [this](uintmax_t done, uintmax_t total) { moveDone_ = done; moveTotal_ = total; return !cancelMove_.load(); });
    co_await wil::resume_foreground(queue);
    moveTimer_.Stop(); moveBusy_ = false; ReleaseFileTask(); controls(true); CancelMoveButton().Visibility(Visibility::Collapsed); MoveProgress().Value(result.success ? 100 : 0); MoveSummary().Text(result.message); Notice(hstring(result.message), !result.success);
}
}
