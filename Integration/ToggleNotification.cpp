#include "pch.h"
#include "ToggleNotification.h"
#include "I18n.h"
#include "Diagnostics.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Foundation.Numerics.h>
namespace aelura::notifications {
using namespace winrt; using namespace Microsoft::UI::Xaml;
struct ToggleNotification::Impl {
    HWND owner{}, hwnd{}; Window window{nullptr}; Controls::Grid root{nullptr};
    Controls::TextBlock title, module; Controls::ToggleSwitch indicator;
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    ULONGLONG until{}, hiddenAt{}; bool leaving{}, closing{};
    explicit Impl(HWND h):owner(h){}
    void animation(bool enter) {
        auto visual=Hosting::ElementCompositionPreview::GetElementVisual(root);
        visual.StopAnimation(L"Offset");visual.StopAnimation(L"Opacity");
        if(!Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) {visual.Opacity(enter?1.f:0.f);return;}
        auto compositor=visual.Compositor();
        auto opacity=compositor.CreateScalarKeyFrameAnimation();opacity.Duration(std::chrono::milliseconds(200));opacity.InsertKeyFrame(0,enter?0.f:1.f);opacity.InsertKeyFrame(1,enter?1.f:0.f);
        auto offset=compositor.CreateVector3KeyFrameAnimation();offset.Duration(std::chrono::milliseconds(200));offset.InsertKeyFrame(0,{enter?30.f:0.f,0,0});offset.InsertKeyFrame(1,{enter?0.f:30.f,0,0});
        visual.StartAnimation(L"Opacity",opacity);visual.StartAnimation(L"Offset",offset);
    }
    void create() {
        closing=false;
        title=Controls::TextBlock();module=Controls::TextBlock();indicator=Controls::ToggleSwitch();
        window=Window();check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
        auto presenter=Microsoft::UI::Windowing::OverlappedPresenter::Create();presenter.SetBorderAndTitleBar(false,false);presenter.IsResizable(false);presenter.IsAlwaysOnTop(true);window.AppWindow().SetPresenter(presenter);
        SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT);
        root=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource LayerFillColorDefaultBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="10" Padding="16" ColumnSpacing="14"/>)").as<Controls::Grid>();
        Controls::ColumnDefinition iconColumn,textColumn;iconColumn.Width({64,GridUnitType::Pixel});textColumn.Width({1,GridUnitType::Star});root.ColumnDefinitions().Append(iconColumn);root.ColumnDefinitions().Append(textColumn);
        indicator.IsHitTestVisible(false);indicator.IsTabStop(false);indicator.OnContent(box_value(L""));indicator.OffContent(box_value(L""));indicator.VerticalAlignment(VerticalAlignment::Center);root.Children().Append(indicator);
        Controls::StackPanel copy;copy.Spacing(5);title.FontSize(15);module.FontSize(13);module.TextTrimming(TextTrimming::CharacterEllipsis);copy.Children().Append(title);copy.Children().Append(module);Controls::Grid::SetColumn(copy,1);root.Children().Append(copy);window.Content(root);
        window.Closed([this](auto const&,WindowEventArgs const& e){if(!closing){e.Handled(true);ShowWindow(hwnd,SW_HIDE);if(timer)timer.Stop();}});
        timer=window.DispatcherQueue().CreateTimer();timer.Interval(std::chrono::milliseconds(30));timer.Tick([this](auto const&,auto const&){auto now=GetTickCount64();if(!leaving&&now>=until){animation(false);leaving=true;hiddenAt=now+210;}if(leaving&&now>=hiddenAt){ShowWindow(hwnd,SW_HIDE);timer.Stop();}});
    }
    void show(std::wstring const& name,bool enabled,ElementTheme theme,bool action=false) {
        if(!window)create();
        root.RequestedTheme(theme);root.Language(i18n::chinese()?L"zh-CN":L"en-US");
        title.Text(i18n::tr(action?"keys.triggered":enabled?"toggle.enabled":"toggle.disabled"));module.Text(name);indicator.IsOn(enabled);window.Title(name);
        indicator.Visibility(action?Visibility::Collapsed:Visibility::Visible);
        root.ColumnDefinitions().GetAt(0).Width({action?0.0:64.0,GridUnitType::Pixel});
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor);auto dpi=GetDpiForWindow(owner);int width=MulDiv(340,dpi,96),height=MulDiv(90,dpi,96),margin=MulDiv(24,dpi,96);
        window.AppWindow().MoveAndResize({monitor.rcWork.right-width-margin,monitor.rcWork.bottom-height-margin,width,height});
        bool visible=IsWindowVisible(hwnd)!=FALSE;
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        if(!visible||leaving)animation(true);
        leaving=false;until=GetTickCount64()+3000;timer.Start();
    }
    void close(){if(timer)timer.Stop();if(window){closing=true;try{window.Close();}catch(hresult_error const&){}window=nullptr;}}
};
ToggleNotification::ToggleNotification(HWND owner):impl_(std::make_shared<Impl>(owner)){}
ToggleNotification::~ToggleNotification(){close();}
void ToggleNotification::show(std::wstring const& module,bool enabled,ElementTheme theme){try{impl_->show(module,enabled,theme);}catch(hresult_error const& e){diagnostics::failure("shortcut.notification",e.code(),e.message());impl_->close();}}
void ToggleNotification::action(std::wstring const& name,ElementTheme theme){try{impl_->show(name,false,theme,true);}catch(hresult_error const& e){diagnostics::failure("shortcut.notification",e.code(),e.message());impl_->close();}}
void ToggleNotification::close(){impl_->close();}
}
