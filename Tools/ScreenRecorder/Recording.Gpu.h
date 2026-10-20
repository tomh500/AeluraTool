#pragma once
#include "Recording.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
struct ID3D12Resource;
struct ID3D12Device;
namespace aelura::recording::gpu {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
struct Pipeline {
    explicit Pipeline(Options const&,RECT,HWND window = nullptr,bool bgraOnly = false); ~Pipeline();
    ID3D11Texture2D* next();
    std::vector<BYTE> readback();
    bool sourceClosed() const;
    bool captureWaiting() const;
    uint64_t captureRecoveries() const;
    Ptr<ID3D11Device> device; unsigned vendor{},width{},height{};std::wstring adapter;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
struct Packet {std::vector<BYTE> data;LONGLONG time{},duration{};bool key{};};
struct Encoder {
    virtual ~Encoder()=default;
    virtual std::vector<Packet> encode(ID3D11Texture2D*,LONGLONG,LONGLONG)=0;
    virtual std::vector<Packet> encode12(ID3D12Resource*,LONGLONG,LONGLONG);
    virtual std::vector<Packet> flush()=0;
    virtual std::vector<BYTE> headers()=0;
};
std::unique_ptr<Encoder> nvidia(Options const&,Pipeline&);
std::unique_ptr<Encoder> amd(Options const&,Pipeline&);
std::unique_ptr<Encoder> nvidia(Options const&,ID3D11Device*,unsigned,unsigned);
std::unique_ptr<Encoder> amd(Options const&,ID3D11Device*,unsigned,unsigned);
bool cudaAvailable();
std::unique_ptr<Encoder> nvidia12(Options const&,ID3D12Device*,unsigned,unsigned);
}
