#include "pch.h"
#include "RecordingNotification.h"
#include "I18n.h"
#include "Diagnostics.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <chrono>
#include <shlobj.h>

namespace aelura::recording {
using namespace winrt; using namespace Microsoft::UI::Xaml;
struct Notification::Impl : std::enable_shared_from_this<Impl> {
    HWND main{}, hwnd{}; Window window{nullptr};
    Controls::Grid root{nullptr}; Controls::TextBlock title, detail; Controls::ProgressRing progress;
    Controls::Button open;std::filesystem::path output;
    bool saving{}, visible{}, leaving{}, closing{};
    uint64_t savedGeneration{};
    std::chrono::steady_clock::time_point hideAt{}, closeAt{};
    explicit Impl(HWND h) : main(h) {}
    void create() {
        if (window) return;
        window = Window(); check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
        auto presenter = Microsoft::UI::Windowing::OverlappedPresenter::Create();
        presenter.SetBorderAndTitleBar(false, false); presenter.IsResizable(false); presenter.IsAlwaysOnTop(true);
        window.AppWindow().SetPresenter(presenter);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        root = Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="14" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="10"/>)").as<Controls::Grid>();
        Controls::StackPanel row; row.Orientation(Controls::Orientation::Horizontal); row.Spacing(12);
        progress.Width(22); progress.Height(22); row.Children().Append(progress);
        Controls::StackPanel text; text.Spacing(5); text.Width(288); title.FontSize(15); detail.FontSize(12); detail.TextTrimming(TextTrimming::CharacterEllipsis); text.Children().Append(title); text.Children().Append(detail); row.Children().Append(text);open.Content(row);open.Padding({0,0,0,0});open.HorizontalContentAlignment(HorizontalAlignment::Stretch);root.Children().Append(open);
        window.Content(root);
        auto weak = weak_from_this();
        open.Click([weak](auto const&,auto const&){if(auto self=weak.lock();self&&!self->output.empty()){
            PIDLIST_ABSOLUTE item{};auto hr=SHParseDisplayName(self->output.c_str(),nullptr,&item,0,nullptr);
            if(SUCCEEDED(hr)){hr=SHOpenFolderAndSelectItems(item,0,nullptr,0);CoTaskMemFree(item);}
            if(FAILED(hr))diagnostics::failure("replay.notification.open",hr);
        }});
        window.Closed([weak](auto const&, WindowEventArgs const& e) { if (auto self = weak.lock(); self && !self->closing) { e.Handled(true); ShowWindow(self->hwnd, SW_HIDE); self->visible = false; } });
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    }
    void animate(bool entering) {
        auto visual = Hosting::ElementCompositionPreview::GetElementVisual(root);
        if (!Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) { visual.Opacity(entering ? 1.0f : 0.0f); return; }
        auto compositor = visual.Compositor();
        auto easing = compositor.CreateCubicBezierEasingFunction({0.16f, 1.0f}, {0.3f, 1.0f});
        auto offset = compositor.CreateVector3KeyFrameAnimation(); offset.Duration(std::chrono::milliseconds(entering ? 420 : 240));
        offset.InsertKeyFrame(0, {entering ? 44.0f : 0.0f, 0, 0}); offset.InsertKeyFrame(1, {entering ? 0.0f : 44.0f, 0, 0}, easing);
        auto opacity = compositor.CreateScalarKeyFrameAnimation(); opacity.Duration(std::chrono::milliseconds(entering ? 300 : 200));
        opacity.InsertKeyFrame(0, entering ? 0.0f : 1.0f); opacity.InsertKeyFrame(1, entering ? 1.0f : 0.0f, easing);
        visual.StartAnimation(L"Offset", offset); visual.StartAnimation(L"Opacity", opacity);
    }
    void update(Snapshot const& s, ElementTheme theme) {
        const auto now = std::chrono::steady_clock::now();
        const bool begin = s.replay && s.saving && !saving;
        const bool completed = s.replay && s.savedGeneration > savedGeneration;
        const bool end = (saving && !s.saving) || completed;
        savedGeneration = s.savedGeneration;
        if (begin || end) {
            create(); root.RequestedTheme(theme); window.Title(i18n::tr("record.save_replay"));
            title.Text(i18n::tr(s.saving ? "record.saving" : FAILED(s.error) ? (s.error == E_ABORT ? "record.save_canceled" : "record.save_failed") : "record.replay_saved"));
            detail.Text(s.saving ? i18n::tr("record.replay_saving_hint") : s.output.filename().wstring());
            output=!s.saving&&SUCCEEDED(s.error)?s.output:std::filesystem::path{};open.IsEnabled(!output.empty());
            Controls::ToolTipService::SetToolTip(open,box_value(i18n::tr("record.show_file")));
            progress.IsActive(s.saving); progress.Visibility(s.saving ? Visibility::Visible : Visibility::Collapsed);
            if (!visible || leaving) {
                MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), &mi);
                const auto dpi = GetDpiForWindow(main); const int width = MulDiv(370, dpi, 96), height = MulDiv(92, dpi, 96), margin = MulDiv(20, dpi, 96);
                window.AppWindow().MoveAndResize({mi.rcWork.right - width - margin, mi.rcWork.top + margin, width, height});
                ShowWindow(hwnd, SW_SHOWNOACTIVATE); SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                animate(true); visible = true; leaving = false;
            }
            hideAt = s.saving ? std::chrono::steady_clock::time_point::max() : now + std::chrono::seconds(8);
        }
        saving = s.replay && s.saving;
        if (window) root.RequestedTheme(theme);
        if (visible && !leaving && now >= hideAt) { animate(false); leaving = true; closeAt = now + std::chrono::milliseconds(260); }
        if (visible && leaving && now >= closeAt) { ShowWindow(hwnd, SW_HIDE); visible = leaving = false; }
    }
};
Notification::Notification(HWND main) : impl_(std::make_shared<Impl>(main)) {}
Notification::~Notification() { close(); }
void Notification::close() { if (impl_->window) { impl_->closing = true; try { impl_->window.Close(); } catch (hresult_error const& e) { diagnostics::failure("replay.notification.close", e.code(), e.message()); } impl_->window = nullptr; } }
void Notification::update(Snapshot const& s, ElementTheme theme) { impl_->update(s, theme); }
}
