#pragma once
#include "Recording.Gpu.h"
#include <chrono>
#include <mutex>
namespace aelura::recording::gpu {
// DXGI permits only one duplication of an output per process. Both recording
// sessions share acquisition, device and immediate-context serialization.
struct DesktopCapture {
    static std::shared_ptr<DesktopCapture> share(ID3D11Device*,IDXGIOutput1*);
    std::recursive_mutex mutex;
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> texture;RECT monitor{};
    bool waiting{true};uint64_t generation{},recoveries{};
    void next();
private:
    DesktopCapture(ID3D11Device*,DXGI_OUTPUT_DESC const&);
    Ptr<IDXGIOutputDuplication> duplication;
    std::wstring displayName;
    std::chrono::steady_clock::time_point retryAfter{};
    void lost();bool reconnect();
};
}
