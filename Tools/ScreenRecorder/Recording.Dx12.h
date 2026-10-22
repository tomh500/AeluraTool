#pragma once
#include "Recording.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>
#include <mutex>
namespace aelura::recording::gpu {
std::recursive_mutex& dx12LifetimeMutex();
// Compatible DX11 capture, shared GPU textures/fences, native DX12 processing
// and NVENC. No desktop GDI readback/upload or D3D11On12 wrapper.
class Dx12Pipeline {
public:
    Dx12Pipeline(Options const&,RECT,HWND window=nullptr);
    ~Dx12Pipeline();
    ID3D12Resource* next();
    ID3D12Resource* process(std::span<BYTE const> bgra,unsigned stride);
    bool captureWaiting() const;bool sourceClosed() const;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    unsigned width{},height{},vendor{};std::wstring adapter;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
