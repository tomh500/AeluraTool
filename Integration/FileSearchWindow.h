#pragma once
#include "FileSearch.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Dispatching.h>

namespace aelura::filesearch {
class SearchWindow {
public:
    explicit SearchWindow(std::shared_ptr<Index> index, bool floating = true, HWND owner = nullptr);
    ~SearchWindow();
    void show();
    void hide();
    void close();
    void appearance(winrt::Microsoft::UI::Xaml::ElementTheme theme);
    void localize();
    winrt::Microsoft::UI::Xaml::UIElement content() const { return root_; }
private:
    void changed();
    void render();
    void action(int kind);
    void status();
    void layout();
    void notice(char const* key);
    std::shared_ptr<Index> index_;
    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox query_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ListView list_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock state_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer debounce_{nullptr}, poll_{nullptr};
    Session session_;
    Snapshot result_;
    uint64_t generation_{};
    uint64_t indexRevision_{}, lastQueryTime_{};
    char const* noticeKey_{};
    uint64_t noticeUntil_{};
    bool closing_{}, visible_{};
    bool composing_{}, compositionKey_{};
    HWND hwnd_{};
};
}
