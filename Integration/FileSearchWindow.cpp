#include "pch.h"
#include "FileSearchWindow.h"
#include "I18n.h"
#include "FileShellActions.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using aelura::i18n::tr;
namespace aelura::filesearch {
SearchWindow::SearchWindow(std::shared_ptr<Index> index, bool floating, HWND owner) : index_(std::move(index)), session_(index_), hwnd_(owner) {
    root_ = Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="20" RowSpacing="12">
      <Grid.Resources><Style TargetType="TextBlock"><Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/></Style></Grid.Resources>
      <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
      <TextBox x:Name="Query" FontSize="20" MaxLength="512"/>
      <StackPanel x:Name="Options" Grid.Row="1" Spacing="4"><CheckBox x:Name="EverythingSDK"/><StackPanel Orientation="Horizontal" Spacing="14"><CheckBox x:Name="MatchPath"/><CheckBox x:Name="MatchCase"/><CheckBox x:Name="WholeWord"/><CheckBox x:Name="Regex"/></StackPanel></StackPanel>
      <StackPanel x:Name="Feedback" Grid.Row="2" Spacing="4"><TextBlock x:Name="State" TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/><TextBlock x:Name="IndexState" TextWrapping="Wrap" FontSize="12" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/></StackPanel>
      <ListView x:Name="Results" Grid.Row="3" SelectionMode="Single" IsItemClickEnabled="False" HorizontalContentAlignment="Stretch"><ListView.ItemTemplate><DataTemplate><TextBlock Text="{Binding}" TextWrapping="NoWrap" TextTrimming="CharacterEllipsis" Padding="4,7"/></DataTemplate></ListView.ItemTemplate></ListView>
      <StackPanel x:Name="Actions" Grid.Row="4" Spacing="10"><StackPanel Orientation="Horizontal" Spacing="8"><Button x:Name="Open"/><Button x:Name="Locate"/><Button x:Name="Copy"/><Button x:Name="Refresh"/><Button x:Name="CancelIndex"/><Button x:Name="CancelFiles" Visibility="Collapsed"/></StackPanel><TextBlock x:Name="Hint" FontSize="12" TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/></StackPanel>
    </Grid>)").as<Grid>();
    query_ = root_.FindName(L"Query").as<TextBox>(); list_ = root_.FindName(L"Results").as<ListView>(); state_ = root_.FindName(L"State").as<TextBlock>();
    if (floating) {
        window_ = Window(); window_.Content(root_);
        check_hresult(window_.as<::IWindowNative>()->get_WindowHandle(&hwnd_));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        auto presenter = window_.AppWindow().Presenter().as<Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter.IsMaximizable(false); presenter.IsMinimizable(false); presenter.IsResizable(false);
        presenter.SetBorderAndTitleBar(false, false);
        window_.AppWindow().Closing([this](auto const&, auto const& args) { if (!closing_) { args.Cancel(true); hide(); } });
    }
    auto queue = root_.DispatcherQueue();
    debounce_ = queue.CreateTimer(); debounce_.Interval(std::chrono::milliseconds(180)); debounce_.IsRepeating(false);
    debounce_.Tick([this](auto const&, auto const&) {
        auto checked = [this](wchar_t const* name) { return root_.FindName(name).as<CheckBox>().IsChecked().Value(); };
        generation_ = session_.search(std::wstring(query_.Text()), {checked(L"MatchPath"), checked(L"MatchCase"), checked(L"WholeWord"), checked(L"Regex")});
        indexRevision_ = index_->progress().revision; lastQueryTime_ = GetTickCount64();
        searching_ = true; status();
    });
    poll_ = queue.CreateTimer(); poll_.Interval(std::chrono::milliseconds(60));
    poll_.Tick([this](auto const&, auto const&) {
        files::completeClipboardMove(hwnd_);
        auto engine = root_.FindName(L"EverythingSDK").as<CheckBox>();
        if (engine.IsChecked().Value() != index_->everything()) {
            syncingEngine_ = true; engine.IsChecked(index_->everything()); syncingEngine_ = false; changed();
        }
        if (searching_ && !menuOpen_ && !dialogOpen_) {
            auto next = session_.snapshot();
            if (next.generation == generation_ && next.state != State::Searching) {
                searching_ = false; indexRevision_ = next.sourceRevision; result_ = std::move(next); render();
            }
        }
        auto progress = index_->progress();
        if (!composing_ && !menuOpen_ && !dialogOpen_ && !debounce_.IsRunning() && !searching_ && (index_->everything() || progress.revision != indexRevision_) && GetTickCount64() - lastQueryTime_ >= 1000 && !query_.Text().empty()) {
            debounce_.Start();
        }
        status();
    });
    query_.TextChanged([this](auto const&, auto const&) { changed(); });
    query_.Loaded([this](auto const&, auto const&) { if (visible_) query_.Focus(FocusState::Programmatic); });
    query_.TextCompositionStarted([this](auto const&, auto const&) { composing_ = true; changed(); });
    query_.TextCompositionEnded([this](auto const&, auto const&) { composing_ = false; changed(); });
    auto engine = root_.FindName(L"EverythingSDK").as<CheckBox>(); engine.IsChecked(index_->everything());
    auto changeEngine = [this, engine](auto const&, auto const&) {
        if (syncingEngine_) return;
        if (!index_->setEverything(engine.IsChecked().Value())) {
            syncingEngine_ = true; engine.IsChecked(index_->everything()); syncingEngine_ = false; notice("filesearch.setting_failed"); return;
        }
        changed(); status();
    };
    engine.Checked(changeEngine); engine.Unchecked(changeEngine);
    for (auto name : {L"MatchPath", L"MatchCase", L"WholeWord", L"Regex"}) {
        auto box = root_.FindName(name).as<CheckBox>();
        box.Checked([this](auto const&, auto const&) { changed(); }); box.Unchecked([this](auto const&, auto const&) { changed(); });
    }
    int kind = 0;
    for (auto name : {L"Open", L"Locate", L"Copy"}) {
        root_.FindName(name).as<Button>().Click([this, kind](auto const&, auto const&) { action(kind); }); ++kind;
    }
    root_.FindName(L"Refresh").as<Button>().Click([this](auto const&, auto const&) { if (index_->everything()) debounce_.Start(); else index_->refresh(); status(); });
    root_.FindName(L"CancelIndex").as<Button>().Click([this](auto const&, auto const&) { index_->cancel(); status(); });
    root_.FindName(L"CancelFiles").as<Button>().Click([](auto const&, auto const&) { files::cancel(); });
    list_.ContextRequested([this](auto const&, Input::ContextRequestedEventArgs const& args) { context(args); });
    list_.DoubleTapped([this](auto const&, auto const&) { action(0); });
    list_.AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, Input::KeyRoutedEventArgs const& args) {
        if (compositionKey_ || menuOpen_ || dialogOpen_) return;
        const auto selected = list_.SelectedIndex();
        if (selected >= 0 && static_cast<size_t>(selected) < result_.items.size()) {
            int command = -1; auto key = args.Key(); using Windows::System::VirtualKey;
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (key == VirtualKey::C) command = GetKeyState(VK_SHIFT) & 0x8000 ? 2 : 3;
                else if (key == VirtualKey::X) command = 4;
                else if (key == VirtualKey::V) command = 5;
            } else if (key == VirtualKey::F2) command = 6;
            else if (key == VirtualKey::Delete) command = 7;
            else if (key == VirtualKey::Enter && (GetKeyState(VK_MENU) & 0x8000)) command = 9;
            if (command >= 0) { auto item = result_.items[selected]; fileAction(command, item.path, item.folder); args.Handled(true); return; }
        }
        if (!compositionKey_ && args.Key() == Windows::System::VirtualKey::Enter) {
            action(GetKeyState(VK_CONTROL) & 0x8000 ? 1 : 0); args.Handled(true);
        }
    })), true);
    root_.AddHandler(UIElement::PreviewKeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, auto const&) { compositionKey_ = composing_; })), true);
    root_.AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto const&, Input::KeyRoutedEventArgs const& args) {
        if (compositionKey_ || menuOpen_ || dialogOpen_ || args.Handled()) return; // Preserve native text/IME and flyout handling.
        using Windows::System::VirtualKey;
        auto key = args.Key();
        if (key == VirtualKey::Escape) { if (window_) hide(); else query_.Text(L""); args.Handled(true); }
        else if ((key == VirtualKey::Down || key == VirtualKey::Up) && query_.FocusState() != FocusState::Unfocused && !result_.items.empty()) {
            int next = std::clamp(list_.SelectedIndex() + (key == VirtualKey::Down ? 1 : -1), 0, static_cast<int>(result_.items.size()) - 1);
            list_.SelectedIndex(next); list_.ScrollIntoView(list_.SelectedItem()); args.Handled(true);
        } else if (key == VirtualKey::Enter && query_.FocusState() != FocusState::Unfocused) {
            action(GetKeyState(VK_CONTROL) & 0x8000 ? 1 : 0); args.Handled(true);
        }
    })), true);
    root_.ActualThemeChanged([this](auto const&, auto const&) { if (!window_) return;
        BOOL dark = root_.ActualTheme() == ElementTheme::Dark; DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    });
    localize(); layout();
}
SearchWindow::~SearchWindow() { close(); }
void SearchWindow::show() {
    if (visible_ && !window_) return;
    visible_ = true;
    if (!window_) { query_.Focus(FocusState::Programmatic); changed(); poll_.Start(); return; }
    POINT cursor{}; GetCursorPos(&cursor); MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT bounds{}; GetWindowRect(hwnd_, &bounds);
    SetWindowPos(hwnd_, HWND_TOP, monitor.rcWork.left + (std::max)(0L, (monitor.rcWork.right - monitor.rcWork.left - (bounds.right - bounds.left)) / 2),
        monitor.rcWork.top + (std::max)(0L, (monitor.rcWork.bottom - monitor.rcWork.top - (bounds.bottom - bounds.top)) / 3), 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    window_.Activate(); SetForegroundWindow(hwnd_); query_.Focus(FocusState::Programmatic); query_.SelectAll(); changed();
    poll_.Start();
    root_.DispatcherQueue().TryEnqueue([box = query_] { box.Focus(FocusState::Programmatic); box.SelectAll(); });
}
void SearchWindow::hide() { visible_ = false; debounce_.Stop(); poll_.Stop(); session_.cancel(); if (window_) ShowWindow(hwnd_, SW_HIDE); }
void SearchWindow::close() {
    if (closing_) return;
    closing_ = true; hide(); if (window_) window_.Close();
}
void SearchWindow::appearance(ElementTheme theme) {
    root_.RequestedTheme(theme);
    if (window_) { BOOL dark = root_.ActualTheme() == ElementTheme::Dark; DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)); }
}
void SearchWindow::localize() {
    root_.Language(aelura::i18n::chinese() ? L"zh-CN" : L"en-US");
    if (window_) window_.Title(tr("filesearch.title")); query_.PlaceholderText(tr("filesearch.placeholder"));
    root_.FindName(L"EverythingSDK").as<CheckBox>().Content(box_value(tr("filesearch.use_sdk")));
    Automation::AutomationProperties::SetName(query_, tr("filesearch.title"));
    Automation::AutomationProperties::SetName(list_, tr("filesearch.results"));
    for (auto const& pair : {std::pair{L"MatchPath", "filesearch.match_path"}, {L"MatchCase", "filesearch.match_case"}, {L"WholeWord", "filesearch.whole_word"}, {L"Regex", "filesearch.regex"}})
        root_.FindName(pair.first).as<CheckBox>().Content(box_value(tr(pair.second)));
    for (auto const& pair : {std::pair{L"Open", "filesearch.open_file"}, {L"Locate", "filesearch.locate"}, {L"Copy", "filesearch.copy"}, {L"Refresh", "filesearch.refresh"}, {L"CancelIndex", "filesearch.cancel_index"}})
        root_.FindName(pair.first).as<Button>().Content(box_value(tr(pair.second)));
    status();
    root_.FindName(L"CancelFiles").as<Button>().Content(box_value(tr("filesearch.cancel_files")));
}
void SearchWindow::changed() {
    if (!visible_) return;
    layout();
    noticeUntil_ = 0;
    debounce_.Stop(); session_.cancel(); generation_ = session_.snapshot().generation;
    searching_ = false; result_ = {}; rendered_.clear(); list_.Items().Clear();
    if (query_.Text().empty()) { status(); return; }
    result_.state = State::Searching; status(); if (!composing_) debounce_.Start();
}
void SearchWindow::layout() {
    if (!window_) return;
    const bool expanded = !query_.Text().empty();
    for (auto name : {L"Options", L"Feedback", L"Results", L"Actions"}) root_.FindName(name).as<UIElement>().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
    const auto scale = GetDpiForWindow(hwnd_) / 96.0;
    window_.AppWindow().Resize({static_cast<int>(760 * scale), static_cast<int>((expanded ? 520 : 84) * scale)});
}
void SearchWindow::render() {
    // Keep surviving rows in place during background index updates. Never reset
    // the ListView or select its first row on every published index batch.
    std::wstring selected;
    auto selection = list_.SelectedIndex();
    if (selection >= 0 && static_cast<size_t>(selection) < rendered_.size()) selected = rendered_[selection].path;
    preserveResultOrder(rendered_, result_.items);
    auto rows = list_.Items();
    while (rows.Size() > result_.items.size()) rows.RemoveAtEnd();
    for (uint32_t i = 0; i < result_.items.size(); ++i) {
        auto const& item = result_.items[i];
        if (i >= rows.Size()) rows.Append(box_value(item.name + L"\n" + item.path));
        else if (i >= rendered_.size() || item.path != rendered_[i].path || item.name != rendered_[i].name)
            rows.SetAt(i, box_value(item.name + L"\n" + item.path));
    }
    auto at = std::find_if(result_.items.begin(), result_.items.end(), [&](auto const& item) { return item.path == selected; });
    if (at != result_.items.end()) list_.SelectedIndex(static_cast<int>(at - result_.items.begin()));
    else if (rendered_.empty() && !result_.items.empty()) list_.SelectedIndex(0);
    else if (!selected.empty()) list_.SelectedIndex(-1);
    rendered_ = result_.items;
    status();
}
void SearchWindow::status() {
    auto key = result_.state == State::Idle ? "filesearch.idle" : result_.state == State::Searching ? "filesearch.searching" :
        result_.state == State::InvalidQuery ? "filesearch.invalid_query" : result_.state == State::EngineUnavailable ? "filesearch.sdk_unavailable" : result_.state == State::TimedOut ? "filesearch.sdk_timeout" :
        result_.state == State::Ready ? (result_.items.empty() ? "filesearch.none" : "filesearch.count") : "filesearch.failed";
    std::wstring value = tr(key);
    if (result_.state == State::Ready && !result_.items.empty()) {
        value += std::to_wstring(result_.items.size()) + L" / " + std::to_wstring(result_.total);
        if (result_.total > result_.items.size()) value += tr("filesearch.refine");
    }
    state_.Text(value);
    if (noticeKey_ && GetTickCount64() < noticeUntil_) state_.Text(tr(noticeKey_));
    auto operation = files::state();
    root_.FindName(L"CancelFiles").as<Button>().Visibility(operation.busy ? Visibility::Visible : Visibility::Collapsed);
    if (operation.busy) state_.Text(std::wstring(tr("filesearch.file_busy")) + std::to_wstring(operation.completed) + L" / " + std::to_wstring(operation.total));
    else if (!operation.recovery.empty()) state_.Text(std::wstring(tr("filesearch.recovery")) + operation.recovery.wstring());
    if (operation.revision != fileRevision_) {
        fileRevision_ = operation.revision;
        if (!operation.busy) { noticeKey_ = operation.cancelled ? "filesearch.file_cancelled" : operation.failed ? "filesearch.file_failed" : "filesearch.file_done"; noticeUntil_ = GetTickCount64() + 5000; }
    }
    const auto progress = index_->progress();
    const bool sdk = index_->everything();
    root_.FindName(L"Hint").as<TextBlock>().Text(tr(sdk ? "filesearch.sdk_hint" : "filesearch.hint"));
    std::wstring indexText = tr(progress.running ? "filesearch.indexing" : progress.cancelled ? "filesearch.index_cancelled" : progress.failed ? "filesearch.index_failed" : "filesearch.index_ready");
    indexText += std::to_wstring(progress.indexed);
    if (progress.skipped) indexText += std::wstring(tr("filesearch.skipped")) + std::to_wstring(progress.skipped);
    if (progress.cached) indexText += tr("filesearch.cached");
    if (progress.cacheFailed) indexText += tr("filesearch.cache_failed");
    root_.FindName(L"IndexState").as<TextBlock>().Text(sdk ? tr("filesearch.sdk_active") : indexText);
    root_.FindName(L"Refresh").as<Button>().IsEnabled(sdk || !progress.running);
    root_.FindName(L"Refresh").as<Button>().Content(box_value(tr(sdk ? "filesearch.refresh_results" : "filesearch.refresh")));
    root_.FindName(L"CancelIndex").as<Button>().Visibility(sdk || operation.busy ? Visibility::Collapsed : Visibility::Visible);
    root_.FindName(L"CancelIndex").as<Button>().IsEnabled(!sdk && progress.running);
    for (auto name : {L"Open", L"Locate", L"Copy"}) root_.FindName(name).as<Button>().IsEnabled(result_.state == State::Ready && !result_.items.empty());
}
void SearchWindow::context(Input::ContextRequestedEventArgs const& args) {
    if (dialogOpen_ || files::active()) { args.Handled(true); return; }
    auto element = args.OriginalSource().try_as<DependencyObject>();
    ListViewItem row{nullptr};
    while (element && element != list_) { row = element.try_as<ListViewItem>(); if (row) break; element = Media::VisualTreeHelper::GetParent(element); }
    Windows::Foundation::Point point{}; bool positioned = args.TryGetPosition(list_, point);
    if (row) list_.SelectedIndex(list_.IndexFromContainer(row));
    else if (positioned) { args.Handled(true); return; } // A blank area must not act on an unrelated selection.
    auto selected = list_.SelectedIndex();
    if (selected < 0 || static_cast<size_t>(selected) >= result_.items.size()) return;
    auto target = result_.items[selected]; MenuFlyout menu;
    const char* keys[] = {"filesearch.open_file", "filesearch.locate", "filesearch.copy", "filesearch.copy_file", "filesearch.cut_file", "filesearch.paste_file", "filesearch.rename", "filesearch.delete", "filesearch.desktop_shortcut", "filesearch.properties"};
    for (int command = 0; command < 10; ++command) {
        if (command == 3 || command == 6 || command == 8) menu.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem item; item.Text(tr(command == 5 && !target.folder ? "filesearch.paste_parent" : keys[command]));
        if (command == 5) item.IsEnabled(IsClipboardFormatAvailable(CF_HDROP) != FALSE);
        item.Click([this, command, target](auto const&, auto const&) { fileAction(command, target.path, target.folder); });
        menu.Items().Append(item);
    }
    menuOpen_ = true; menu.Closed([this](auto const&, auto const&) { menuOpen_ = false; });
    Controls::Primitives::FlyoutShowOptions options;
    if (positioned) options.Position(point);
    else if (auto anchor = list_.ContainerFromIndex(selected).try_as<FrameworkElement>()) options.Position(anchor.TransformToVisual(list_).TransformPoint({0, static_cast<float>(anchor.ActualHeight())}));
    menu.ShowAt(list_, options); args.Handled(true);
}
fire_and_forget SearchWindow::fileAction(int command, std::wstring path, bool folder) {
    if (dialogOpen_ || files::active()) co_return;
    try {
        if (command <= 2) {
            if (command == 2) { Windows::ApplicationModel::DataTransfer::DataPackage data; data.SetText(path); Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data); notice("filesearch.copied"); }
            else if (command == 1) { PIDLIST_ABSOLUTE item{}; bool ok = SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)); if (ok) { ok = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0)); CoTaskMemFree(item); } if (!ok) notice("filesearch.action_failed"); }
            else if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) notice("filesearch.action_failed");
            co_return;
        }
        if (command == 3 || command == 4) { notice(SUCCEEDED(files::clipboard(path, command == 4)) ? (command == 4 ? "filesearch.cut_ready" : "filesearch.copy_ready") : "filesearch.action_failed"); co_return; }
        if (command == 9) { if (!files::properties(path, hwnd_)) notice("filesearch.action_failed"); co_return; }
        files::Request request; request.owner = hwnd_; request.index = index_; request.sources.emplace_back(path);
        if (command == 5) { request = files::pasteRequest(folder ? std::filesystem::path(path) : std::filesystem::path(path).parent_path(), hwnd_); request.index = index_; if (request.sources.empty()) { notice("filesearch.paste_empty"); co_return; } }
        else if (command == 8) {
            request.kind = files::Kind::Shortcut; PWSTR desktop{}; check_hresult(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)); request.destination = desktop; CoTaskMemFree(desktop);
        } else {
            dialogOpen_ = true; files::dialog(true); ContentDialog dialog; dialog.XamlRoot(root_.XamlRoot()); dialog.RequestedTheme(root_.ActualTheme()); dialog.CloseButtonText(tr("common.cancel"));
            TextBox name; name.MaxLength(255); name.Text(std::filesystem::path(path).filename().wstring());
            if (command == 6) dialog.DefaultButton(ContentDialogButton::Primary);
            if (command == 6) { request.kind = files::Kind::Rename; dialog.Title(box_value(tr("filesearch.rename"))); name.Header(box_value(tr("filesearch.new_name"))); dialog.Content(name); dialog.PrimaryButtonText(tr("common.save")); dialog.PrimaryButtonClick([name](auto const&, ContentDialogButtonClickEventArgs const& args) { if (!files::validName(std::wstring(name.Text()))) { name.Header(box_value(tr("filesearch.invalid_name"))); args.Cancel(true); } }); }
            else { request.kind = files::Kind::Recycle; dialog.Title(box_value(tr("filesearch.delete"))); dialog.Content(box_value(std::wstring(tr("filesearch.delete_confirm")) + L"\n" + path)); dialog.PrimaryButtonText(tr("filesearch.delete")); }
            auto chosen = co_await dialog.ShowAsync(); dialogOpen_ = false; files::dialog(false);
            if (chosen != ContentDialogResult::Primary) co_return;
            if (command == 6) { request.name = name.Text(); if (request.name == std::filesystem::path(path).filename().wstring()) co_return; }
        }
        if (!files::start(std::move(request))) notice("filesearch.file_failed");
    } catch (...) { if (dialogOpen_) files::dialog(false); dialogOpen_ = false; notice("filesearch.action_failed"); }
}
void SearchWindow::notice(char const* key) { noticeKey_ = key; noticeUntil_ = GetTickCount64() + 2500; status(); }
void SearchWindow::action(int kind) {
    const auto index = list_.SelectedIndex();
    if (result_.state != State::Ready || index < 0 || static_cast<size_t>(index) >= result_.items.size()) return;
    auto const path = result_.items[index].path;
    try {
        if (kind == 2) {
            Windows::ApplicationModel::DataTransfer::DataPackage package; package.SetText(path);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package); notice("filesearch.copied"); return;
        }
        bool ok{};
        if (kind == 1) {
            PIDLIST_ABSOLUTE item{};
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
                ok = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0)); CoTaskMemFree(item);
            }
        } else ok = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        if (ok) { if (window_) hide(); } else notice("filesearch.action_failed");
    } catch (...) { notice("filesearch.action_failed"); }
}
}
