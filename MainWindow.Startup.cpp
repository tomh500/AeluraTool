#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include <winrt/Windows.UI.ViewManagement.h>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Animation;
namespace winrt::AeluraTool::implementation {
void MainWindow::FinishStartup() {
    if (startupBoard_) { startupBoard_.Stop(); startupBoard_ = nullptr; }
    StartupLayer().Visibility(Visibility::Collapsed); ContentShell().Opacity(1);
    if (auto offset = ContentShell().RenderTransform().try_as<TranslateTransform>()) offset.Y(0);
}
void MainWindow::PlayStartup(bool preview) {
    if (!preview && startupPlayed_) return;
    startupPlayed_ = true; FinishStartup();
    try {
        if (!Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) {
            if (preview) Notice(aelura::i18n::tr("motion.system_disabled")); return;
        }
        if (!preview && !StartupMotionSwitch().IsOn()) return;
        wchar_t lastVersion[32]{};
        GetPrivateProfileStringW(L"Appearance", L"LastAnimatedVersion", L"", lastVersion, 32, config_.c_str());
        const bool full = preview || std::wstring(lastVersion) != L"1.4.0";
        if (!full) {
            Storyboard board; startupBoard_ = board; DoubleAnimation fade;
            fade.From(.65); fade.To(1); fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(180)));
            Storyboard::SetTarget(fade, ContentShell()); Storyboard::SetTargetProperty(fade, L"Opacity"); board.Children().Append(fade);
            board.Completed([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->FinishStartup(); }); board.Begin(); return;
        }
        if (!preview) {
            std::error_code ec; std::filesystem::create_directories(config_.parent_path(), ec);
            if (!ec) WritePrivateProfileStringW(L"Appearance", L"LastAnimatedVersion", L"1.4.0", config_.c_str());
        }
        StartupLayer().Visibility(Visibility::Visible);
        TranslateTransform offset; ContentShell().RenderTransform(offset);
        Storyboard board; startupBoard_ = board;
        auto animate = [&](DependencyObject const& target, wchar_t const* property, double from, double to, int duration, int delay = 0) {
            DoubleAnimation animation; animation.From(from); animation.To(to);
            animation.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(duration)));
            animation.BeginTime(Windows::Foundation::TimeSpan{std::chrono::milliseconds(delay)});
            CubicEase easing; easing.EasingMode(EasingMode::EaseOut); animation.EasingFunction(easing);
            Storyboard::SetTarget(animation, target); Storyboard::SetTargetProperty(animation, property); board.Children().Append(animation);
        };
        animate(StartupLayer(), L"Opacity", 1, 0, 300, 550);
        animate(StartupName(), L"Opacity", 0, 1, 260);
        animate(TideMark().RenderTransform(), L"ScaleX", .55, 1, 700);
        animate(ContentShell(), L"Opacity", .45, 1, 650, 100);
        animate(offset, L"Y", 12, 0, 650, 100);
        board.Completed([weak = get_weak()](auto const&, auto const&) { if (auto self = weak.get()) self->FinishStartup(); });
        board.Begin();
    } catch (...) { FinishStartup(); }
}
}
