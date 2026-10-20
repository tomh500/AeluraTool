#pragma once
#include "../Tools/ScreenRecorder/Recording.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
namespace aelura::recording {
class Panel {
public:
    Panel(HWND,std::filesystem::path const&,std::function<void()> restore);
    ~Panel();
    winrt::Microsoft::UI::Xaml::UIElement content() const;
    void localize(); void theme(winrt::Microsoft::UI::Xaml::ElementTheme);
    void toggle(); void replaySave(); bool busy() const;
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
