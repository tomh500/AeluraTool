#pragma once
#include <windows.h>
#include <memory>
#include <winrt/Microsoft.UI.Xaml.h>
namespace aelura::ipv6 {
bool helperLaunch();
class Panel {
public:
    explicit Panel(HWND);
    ~Panel();
    winrt::Microsoft::UI::Xaml::UIElement content() const;
    void localize();
    bool busy() const;
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
