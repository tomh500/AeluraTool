#pragma once
#include "Recording.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <vector>
#include "Recording.Gpu.h"
namespace aelura::recording::media {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
struct Runtime {
    Runtime(); ~Runtime();
};
struct Frames {
    HDC screen{}, memory{}; HBITMAP bitmap{}; HGDIOBJ old{}; void* pixels{};
    RECT region{}; unsigned width{}, height{}; Options options;
    Ptr<IWICImagingFactory> factory;
    Frames(Options const&, RECT); ~Frames();
    std::vector<BYTE> next();
};
struct Microphone {
    Ptr<IAudioClient> client; Ptr<IAudioCaptureClient> capture;
    WAVEFORMATEX* format{}; unsigned channels{}, rate{};
    explicit Microphone(bool); ~Microphone();
    std::vector<int16_t> read();
};
struct Writer {
    Ptr<IMFSinkWriter> sink; DWORD video{}, audio{MAXDWORD}; unsigned rate{}, channels{};
    std::unique_ptr<gpu::Encoder> native;
    Writer(Options const&, unsigned width, unsigned height, Microphone const&, std::filesystem::path const&,gpu::Pipeline* = nullptr,std::unique_ptr<gpu::Encoder> = {});
    void frame(std::vector<BYTE> const&, LONGLONG time, LONGLONG duration);
    void texture(ID3D11Texture2D*,LONGLONG,LONGLONG);
    void texture12(ID3D12Resource*,LONGLONG,LONGLONG);
    void packets(std::vector<gpu::Packet> const&);
    void sound(std::vector<int16_t> const&, LONGLONG time);
    void finish();
};
}
