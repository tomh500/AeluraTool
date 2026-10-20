#pragma once
#include "Recording.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>
#include <mutex>
namespace aelura::recording::gpu {
std::recursive_mutex& dx12LifetimeMutex();
// Strict backend: GDI supplies pixels; all GPU processing and encoder surfaces
// are native D3D12. No D3D11 device, WGC interop or D3D11On12 is instantiated.
class Dx12Pipeline {
public:
    Dx12Pipeline(Options const&,RECT);
    ~Dx12Pipeline();
    ID3D12Resource* next();
    ID3D12Resource* process(std::span<BYTE const> bgra,unsigned stride);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    unsigned width{},height{},vendor{};std::wstring adapter;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
