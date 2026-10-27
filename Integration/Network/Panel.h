#pragma once
#include <memory>
#include <winrt/Microsoft.UI.Xaml.h>
namespace aelura::network {
class Panel {
public:
    explicit Panel(int mode);
    ~Panel();
    winrt::Microsoft::UI::Xaml::UIElement content() const;
    void localize();
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
