#include "pch.h"
#include "RecordingSelector.h"
#include "I18n.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <robuffer.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#undef min
#undef max
namespace aelura::recording {
using namespace winrt; using namespace Microsoft::UI::Xaml; using namespace Controls;
using aelura::i18n::tr;
struct Selector::Impl {
    Selector* owner; Window window{nullptr}; HWND hwnd{}; Canvas canvas{nullptr};
    Shapes::Rectangle outline{nullptr}; std::array<Shapes::Rectangle,4> shades;
    Border toolbar{nullptr}; TextBlock dimensions{nullptr};
    RECT desktop{},selected{},hover{},before{}; POINT down{}; bool dragging{},active{}; int edges{};
    double scale{1}; std::vector<RECT> windows;
    explicit Impl(Selector* s):owner(s) {}
    static LRESULT CALLBACK keys(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
        auto self=reinterpret_cast<Impl*>(data);
        if(message==WM_KEYDOWN&&(wp==VK_ESCAPE||wp==VK_RETURN)){self->finish(wp==VK_RETURN);return 0;}
        if(message==WM_CLOSE){self->finish(false);return 0;}
        if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,keys,id);
        return DefSubclassProc(hwnd,message,wp,lp);
    }
    POINT point(Input::PointerRoutedEventArgs const& args) {
        const auto p=args.GetCurrentPoint(canvas).Position();
        POINT r{desktop.left+LONG(std::lround(p.X*scale)),desktop.top+LONG(std::lround(p.Y*scale))};
        // Match the screenshot selector's exclusive outer-edge behavior.
        if(r.x==desktop.right-1) r.x=desktop.right; if(r.y==desktop.bottom-1) r.y=desktop.bottom;
        r.x=std::clamp(r.x,desktop.left,desktop.right); r.y=std::clamp(r.y,desktop.top,desktop.bottom); return r;
    }
    void rect(FrameworkElement const& e,double x,double y,double w,double h) {
        Canvas::SetLeft(e,x);Canvas::SetTop(e,y); e.Width(std::max(0.0,w));e.Height(std::max(0.0,h));
    }
    void paint() {
        auto r=active?selected:hover;
        double w=(desktop.right-desktop.left)/scale,h=(desktop.bottom-desktop.top)/scale;
        if(IsRectEmpty(&r)) { r=desktop; rect(shades[0],0,0,w,h); for(int i=1;i<4;++i) shades[i].Visibility(Visibility::Collapsed); outline.Visibility(Visibility::Collapsed); }
        else {
            for(auto const& shade:shades) shade.Visibility(Visibility::Visible);
            auto x=(r.left-desktop.left)/scale,y=(r.top-desktop.top)/scale,rw=(r.right-r.left)/scale,rh=(r.bottom-r.top)/scale;
            rect(shades[0],0,0,w,y);rect(shades[1],0,y+rh,w,h-y-rh);rect(shades[2],0,y,x,rh);rect(shades[3],x+rw,y,w-x-rw,rh);
            outline.Visibility(Visibility::Visible);rect(outline,x,y,rw,rh);
        }
        dimensions.Text(active?std::to_wstring(selected.right-selected.left)+L" × "+std::to_wstring(selected.bottom-selected.top):tr("record.select_hint"));
        POINT cursor{};GetCursorPos(&cursor); MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);
        double x=active?(selected.left-desktop.left)/scale:(mi.rcWork.left-desktop.left)/scale+24;
        double y=active?(selected.bottom-desktop.top)/scale+12:(mi.rcWork.top-desktop.top)/scale+24;
        const double tw=std::min(620.0,w-16);toolbar.Width(tw); x=std::clamp(x,8.0,std::max(8.0,w-tw-8));
        y=std::clamp(y,8.0,std::max(8.0,h-110));Canvas::SetLeft(toolbar,x);Canvas::SetTop(toolbar,y);
    }
    void finish(bool accept) {
        if(owner->done) return;
        if(accept && (!active || selected.right-selected.left<2 || selected.bottom-selected.top<2)) return;
        if(accept) owner->result=selected;
        owner->done=true; ShowWindow(hwnd,SW_HIDE);
    }
    void setup(ElementTheme theme,bool replay) {
        desktop={GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
        desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
        EnumWindows([](HWND w,LPARAM p)->BOOL {
            auto& self=*reinterpret_cast<Impl*>(p);
            if(!IsWindowVisible(w)||IsIconic(w)||w==GetDesktopWindow()||w==GetShellWindow()||(GetWindowLongPtrW(w,GWL_EXSTYLE)&WS_EX_TRANSPARENT)) return TRUE;
            DWORD cloaked{};if(SUCCEEDED(DwmGetWindowAttribute(w,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked) return TRUE;
            wchar_t name[64]{};GetClassNameW(w,name,64);if(std::wstring_view(name)==L"Progman"||std::wstring_view(name)==L"WorkerW") return TRUE;
            RECT bounds{},clip{};if(FAILED(DwmGetWindowAttribute(w,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))) GetWindowRect(w,&bounds);
            if(IntersectRect(&clip,&bounds,&self.desktop)) self.windows.push_back(clip); return TRUE;
        },reinterpret_cast<LPARAM>(this));
        // A frozen screenshot is used only while choosing; recording captures live pixels.
        auto width=desktop.right-desktop.left,height=desktop.bottom-desktop.top;
        if(width<=0||height<=0||uint64_t(width)*height>134217728) throw hresult_invalid_argument();
        HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};void* data{};
        HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&data,nullptr,0);
        if(!bitmap||!dc||!screen) { if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);if(screen)ReleaseDC(nullptr,screen);throw_hresult(E_OUTOFMEMORY); }
        HGDIOBJ previous=SelectObject(dc,bitmap);
        Media::Imaging::WriteableBitmap image(width,height);
        try {
            if(!BitBlt(dc,0,0,width,height,screen,desktop.left,desktop.top,SRCCOPY|CAPTUREBLT)) throw_last_error();GdiFlush();
            BYTE* p{};check_hresult(image.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&p));
            memcpy(p,data,size_t(width)*height*4);for(size_t i=3;i<size_t(width)*height*4;i+=4)p[i]=255;image.Invalidate();
        } catch(...) { SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);throw; }
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
        window=Window();window.Title(tr("record.select"));check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
        if(!SetWindowSubclass(hwnd,keys,0xAE10,reinterpret_cast<DWORD_PTR>(this)))throw_last_error();
        auto presenter=Microsoft::UI::Windowing::OverlappedPresenter::Create();presenter.SetBorderAndTitleBar(false,false);presenter.IsAlwaysOnTop(true);presenter.IsResizable(false);
        window.AppWindow().SetPresenter(presenter);window.AppWindow().MoveAndResize({desktop.left,desktop.top,width,height});
        scale=GetDpiForWindow(hwnd)/96.0;
        Grid root=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>)").as<Grid>();root.RequestedTheme(theme);
        Image backdrop;backdrop.Source(image);backdrop.Stretch(Media::Stretch::Fill);root.Children().Append(backdrop);
        canvas=Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SubtleFillColorTransparentBrush}"/>)").as<Canvas>();root.Children().Append(canvas);
        for(auto& shade:shades) { shade=Markup::XamlReader::Load(LR"(<Rectangle xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Fill="{ThemeResource SolidBackgroundFillColorBaseBrush}" Opacity="0.65" IsHitTestVisible="False"/>)").as<Shapes::Rectangle>();canvas.Children().Append(shade); }
        outline=Markup::XamlReader::Load(LR"(<Rectangle xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Stroke="{ThemeResource AccentFillColorDefaultBrush}" StrokeThickness="2" IsHitTestVisible="False"/>)").as<Shapes::Rectangle>();canvas.Children().Append(outline);
        toolbar=Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource CardBackgroundFillColorDefaultBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="8" Padding="12"/>)").as<Border>();
        StackPanel content;content.Spacing(8);dimensions=TextBlock();dimensions.TextWrapping(TextWrapping::Wrap);content.Children().Append(dimensions);
        StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.Spacing(8);
        Button accept;accept.Content(box_value(tr(replay?"record.replay_start":"record.start")));accept.Click([this](auto const&,auto const&){finish(true);});
        Button cancel;cancel.Content(box_value(tr("common.cancel")));cancel.Click([this](auto const&,auto const&){finish(false);});
        actions.Children().Append(accept);actions.Children().Append(cancel);content.Children().Append(actions);toolbar.Child(content);canvas.Children().Append(toolbar);
        auto inToolbar=[this](Input::PointerRoutedEventArgs const& e) { const auto p=e.GetCurrentPoint(toolbar).Position();return p.X>=0&&p.Y>=0&&p.X<=toolbar.ActualWidth()&&p.Y<=toolbar.ActualHeight(); };
        canvas.PointerPressed([this,inToolbar,accept](auto const&,Input::PointerRoutedEventArgs const& e) {
            if(inToolbar(e))return;if(e.GetCurrentPoint(canvas).Properties().IsRightButtonPressed()){finish(false);return;}
            accept.Focus(FocusState::Programmatic);
            SetFocus(hwnd);
            down=point(e);before=selected;edges=0;
            if(active) {
                const int tolerance=int(8*scale);
                if(abs(down.x-selected.left)<tolerance)edges|=1;if(abs(down.x-selected.right)<tolerance)edges|=2;
                if(abs(down.y-selected.top)<tolerance)edges|=4;if(abs(down.y-selected.bottom)<tolerance)edges|=8;
                if(!edges&&PtInRect(&selected,down))edges=16;
            }
            if(!edges) {selected={down.x,down.y,down.x,down.y};active=false;}
            dragging=true;canvas.CapturePointer(e.Pointer());e.Handled(true);
        });
        canvas.PointerMoved([this](auto const&,Input::PointerRoutedEventArgs const& e) {
            const auto p=point(e);
            if(dragging) {
                if(edges==16) {selected=before;auto dx=std::clamp(p.x-down.x,desktop.left-before.left,desktop.right-before.right),dy=std::clamp(p.y-down.y,desktop.top-before.top,desktop.bottom-before.bottom);OffsetRect(&selected,dx,dy);}
                else if(edges) {auto a=POINT{before.left,before.top},b=POINT{before.right,before.bottom};if(edges&1)a.x=p.x;if(edges&2)b.x=p.x;if(edges&4)a.y=p.y;if(edges&8)b.y=p.y;selected=selectionRect(a,b,desktop);}
                else selected=selectionRect(down,p,desktop);
                if(abs(p.x-down.x)+abs(p.y-down.y)>3)active=true;
            } else if(!active) {hover={};for(auto const& w:windows)if(PtInRect(&w,p)){hover=w;break;}}
            paint();
        });
        canvas.PointerReleased([this](auto const&,Input::PointerRoutedEventArgs const& e) {
            if(!dragging)return;dragging=false;canvas.ReleasePointerCapture(e.Pointer());
            if(!active && !IsRectEmpty(&hover)) {selected=hover;active=true;}paint();e.Handled(true);
        });
        root.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& e) {if(e.Key()==winrt::Windows::System::VirtualKey::Escape)finish(false);if(e.Key()==winrt::Windows::System::VirtualKey::Enter)finish(true);});
        for(auto key:{winrt::Windows::System::VirtualKey::Enter,winrt::Windows::System::VirtualKey::Escape}){
            Input::KeyboardAccelerator accelerator;accelerator.Key(key);accelerator.Invoked([this,key](auto const&,Input::KeyboardAcceleratorInvokedEventArgs const& e){finish(key==winrt::Windows::System::VirtualKey::Enter);e.Handled(true);});root.KeyboardAccelerators().Append(accelerator);
        }
        window.Closed([this](auto const&,WindowEventArgs const&){owner->done=true;});
        root.Loaded([accept](auto const&,auto const&){accept.Focus(FocusState::Programmatic);});
        window.Content(root);window.Activate();SetWindowPos(hwnd,HWND_TOPMOST,desktop.left,desktop.top,width,height,SWP_SHOWWINDOW);SetForegroundWindow(hwnd);SetFocus(hwnd);paint();
    }
};
Selector::Selector(ElementTheme theme,bool replay):impl_(std::make_unique<Impl>(this)) {impl_->setup(theme,replay);}
Selector::~Selector(){if(impl_->hwnd)RemoveWindowSubclass(impl_->hwnd,Impl::keys,0xAE10);if(impl_->window)impl_->window.Close();}
}
