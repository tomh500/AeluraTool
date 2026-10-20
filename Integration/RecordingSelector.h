#pragma once
#include "../Tools/ScreenRecorder/Recording.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <optional>
namespace aelura::recording {
class Selector {
public:
    Selector(winrt::Microsoft::UI::Xaml::ElementTheme,bool replay);
    ~Selector();
    bool done{};
    std::optional<RECT> result;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
