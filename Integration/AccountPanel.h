#pragma once
#include <filesystem>
#include <memory>
#include <winrt/Microsoft.UI.Xaml.h>
namespace aelura::account {
class Panel {
public:
    explicit Panel(std::filesystem::path const& settings);
    ~Panel();
    winrt::Microsoft::UI::Xaml::UIElement content() const;
    void localize();
    void refresh();
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
