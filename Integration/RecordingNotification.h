#pragma once
#include "../Tools/ScreenRecorder/Recording.h"
#include <winrt/Microsoft.UI.Xaml.h>
namespace aelura::recording {
class Notification {
public:
    explicit Notification(HWND main);
    ~Notification();
    void update(Snapshot const&, winrt::Microsoft::UI::Xaml::ElementTheme);
    void close();
private:
    struct Impl; std::shared_ptr<Impl> impl_;
};
}
