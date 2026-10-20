#pragma once
#include "../Tools/ScreenRecorder/Recording.h"
#include <optional>
namespace aelura::recording {
class Selector {
public:
    explicit Selector(std::function<HWND()> host);
    ~Selector();
    void poll();
    HRESULT error{S_OK};
    bool done{};
    std::optional<RECT> result;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
