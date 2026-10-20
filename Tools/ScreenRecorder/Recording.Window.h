#pragma once
#include "Recording.Gpu.h"
namespace aelura::recording::gpu {
class WindowCapture {
public:
    WindowCapture(HWND, ID3D11Device*, bool cursor);
    ~WindowCapture();
    ID3D11Texture2D* next();
    bool closed() const;
    unsigned width{}, height{};
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
