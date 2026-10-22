#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <winrt/Microsoft.UI.Xaml.h>
namespace aelura::notifications {
class ToggleNotification {
public:
    explicit ToggleNotification(HWND owner);
    ~ToggleNotification();
    void show(std::wstring const& module, bool enabled, winrt::Microsoft::UI::Xaml::ElementTheme theme);
    void action(std::wstring const& name, winrt::Microsoft::UI::Xaml::ElementTheme theme);
    void close();
private:
    struct Impl; std::shared_ptr<Impl> impl_;
};
}
