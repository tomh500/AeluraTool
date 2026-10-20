#include "Recording.Media.h"
#include <winrt/base.h>
#include <mferror.h>
#include <mftransform.h>
#include <d3d11.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mfreadwrite.lib")
#pragma comment(lib,"mfuuid.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"d3d11.lib")
namespace aelura::recording::media {
using winrt::check_hresult;
Runtime::Runtime() { check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); try { check_hresult(MFStartup(MF_VERSION)); } catch (...) { CoUninitialize(); throw; } }
Runtime::~Runtime() { MFShutdown(); CoUninitialize(); }
Frames::Frames(Options const& o, RECT r) : region(r), options(o) {
    width = outputExtent(unsigned(r.right-r.left), o.scale);
    height = outputExtent(unsigned(r.bottom-r.top), o.scale);
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    screen = GetDC(nullptr); memory = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER),r.right-r.left,-(r.bottom-r.top),1,32,BI_RGB};
    bitmap = CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!screen || !memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); if (screen) ReleaseDC(nullptr,screen);
        winrt::throw_hresult(E_OUTOFMEMORY);
    }
    old = SelectObject(memory,bitmap);
}
Frames::~Frames() { SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr,screen); }
std::vector<BYTE> Frames::next() {
    const auto sw = region.right-region.left, sh = region.bottom-region.top;
    if (!BitBlt(memory,0,0,sw,sh,screen,region.left,region.top,SRCCOPY | CAPTUREBLT)) winrt::throw_last_error();
    if (options.cursor) {
        CURSORINFO c{sizeof(c)};
        if (GetCursorInfo(&c) && c.flags == CURSOR_SHOWING) {
            ICONINFO i{}; if (GetIconInfo(c.hCursor,&i)) {
                DrawIconEx(memory,c.ptScreenPos.x-region.left-i.xHotspot,c.ptScreenPos.y-region.top-i.yHotspot,c.hCursor,0,0,0,nullptr,DI_NORMAL);
                if(i.hbmMask) DeleteObject(i.hbmMask); if(i.hbmColor) DeleteObject(i.hbmColor);
            }
        }
    }
    GdiFlush();
    std::vector<BYTE> scaled; BYTE const* bgra = static_cast<BYTE*>(pixels);
    if (width != unsigned(sw) || height != unsigned(sh)) {
        Ptr<IWICBitmap> input; Ptr<IWICBitmapScaler> scaler;
        check_hresult(factory->CreateBitmapFromMemory(sw,sh,GUID_WICPixelFormat32bppBGRA,sw*4,sw*sh*4,static_cast<BYTE*>(pixels),&input));
        check_hresult(factory->CreateBitmapScaler(&scaler));
        const WICBitmapInterpolationMode modes[]{WICBitmapInterpolationModeNearestNeighbor,WICBitmapInterpolationModeLinear,WICBitmapInterpolationModeCubic,WICBitmapInterpolationModeFant};
        check_hresult(scaler->Initialize(input.Get(),width,height,modes[static_cast<int>(options.scaling)]));
        scaled.resize(size_t(width)*height*4); check_hresult(scaler->CopyPixels(nullptr,width*4,static_cast<UINT>(scaled.size()),scaled.data())); bgra = scaled.data();
    }
    // BT.601 limited-range NV12, matching the media type's declared matrix/range.
    std::vector<BYTE> nv12(size_t(width)*height*3/2);
    auto clip = [](int v) { return static_cast<BYTE>(std::clamp(v,0,255)); };
    for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        auto p = bgra + (size_t(y)*width+x)*4; const int b=p[0],g=p[1],r=p[2];
        nv12[size_t(y)*width+x]=clip(((66*r+129*g+25*b+128)>>8)+16);
        if (!(x&1) && !(y&1)) {
            int rr=0,gg=0,bb=0;
            for(unsigned dy=0;dy<2;++dy) for(unsigned dx=0;dx<2;++dx) { auto q=p+(size_t(dy)*width+dx)*4; bb+=q[0];gg+=q[1];rr+=q[2]; }
            auto uv=nv12.data()+size_t(width)*height+size_t(y/2)*width+x;
            uv[0]=clip(((-38*rr-74*gg+112*bb+512)>>10)+128); uv[1]=clip(((112*rr-94*gg-18*bb+512)>>10)+128);
        }
    }
    return nv12;
}
Microphone::Microphone(bool enabled) {
    if (!enabled) return;
    Ptr<IMMDeviceEnumerator> e; Ptr<IMMDevice> device;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e)));
    check_hresult(e->GetDefaultAudioEndpoint(eCapture,eCommunications,&device));
    check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));
    check_hresult(client->GetMixFormat(&format));
    try {
        const auto tag = format->wFormatTag;
        GUID subtype{}; if (tag == WAVE_FORMAT_EXTENSIBLE) subtype = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
        if (!((tag == WAVE_FORMAT_IEEE_FLOAT || subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && format->wBitsPerSample == 32) &&
            !((tag == WAVE_FORMAT_PCM || subtype == KSDATAFORMAT_SUBTYPE_PCM) && format->wBitsPerSample == 16)) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
        channels = std::min(2u,unsigned(format->nChannels)); rate = format->nSamplesPerSec;
        if (!channels || !rate) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
        check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,format,nullptr));
        check_hresult(client->GetService(IID_PPV_ARGS(&capture))); check_hresult(client->Start());
    } catch (...) { CoTaskMemFree(format); format=nullptr; throw; }
}
Microphone::~Microphone() { if(client) client->Stop(); CoTaskMemFree(format); }
std::vector<int16_t> Microphone::read() {
    std::vector<int16_t> out; if(!capture) return out;
    UINT32 count{}; check_hresult(capture->GetNextPacketSize(&count));
    const bool floating = format->wBitsPerSample == 32;
    while(count) {
        BYTE* bytes{}; DWORD flags{}; check_hresult(capture->GetBuffer(&bytes,&count,&flags,nullptr,nullptr));
        const size_t base=out.size();
        try {
            out.resize(base+size_t(count)*channels);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) for(unsigned f=0;f<count;++f) for(unsigned c=0;c<channels;++c) {
                const auto index=size_t(f)*format->nChannels+c;
                const float value=floating?reinterpret_cast<float*>(bytes)[index]:reinterpret_cast<int16_t*>(bytes)[index]/32768.f;
                out[base+size_t(f)*channels+c]=static_cast<int16_t>(std::clamp(value,-1.f,0.999969f)*32768.f);
            }
        } catch (...) { capture->ReleaseBuffer(count); throw; }
        check_hresult(capture->ReleaseBuffer(count)); check_hresult(capture->GetNextPacketSize(&count));
    }
    return out;
}
namespace {
Ptr<IMFMediaType> videoType(GUID subtype,unsigned w,unsigned h,unsigned fps) {
    Ptr<IMFMediaType> type; check_hresult(MFCreateMediaType(&type));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video)); check_hresult(type->SetGUID(MF_MT_SUBTYPE,subtype));
    check_hresult(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,w,h)); check_hresult(MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,fps,1));
    check_hresult(MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1)); check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
    check_hresult(type->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT601)); check_hresult(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235)); return type;
}
Ptr<IMFSample> sample(void const* data,DWORD size,LONGLONG time,LONGLONG duration) {
    Ptr<IMFMediaBuffer> buffer; Ptr<IMFSample> s; check_hresult(MFCreateMemoryBuffer(size,&buffer)); BYTE* p{};
    check_hresult(buffer->Lock(&p,nullptr,nullptr)); memcpy(p,data,size); check_hresult(buffer->Unlock()); check_hresult(buffer->SetCurrentLength(size));
    check_hresult(MFCreateSample(&s)); check_hresult(s->AddBuffer(buffer.Get())); check_hresult(s->SetSampleTime(time)); check_hresult(s->SetSampleDuration(duration)); return s;
}
bool hardware(IMFSinkWriter* writer,DWORD stream) {
    Ptr<IMFSinkWriterEx> ex; check_hresult(writer->QueryInterface(IID_PPV_ARGS(&ex)));
    for (DWORD n=0;n<16;++n) {
        Ptr<IMFTransform> transform; GUID category{};
        if (FAILED(ex->GetTransformForStream(stream,n,&category,&transform))) break;
        if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
        Ptr<IMFAttributes> a; check_hresult(transform->GetAttributes(&a)); UINT32 length{};
        if (SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute,&length)) && length) return true;
        GUID clsid{}; if(FAILED(a->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&clsid))) return false;
        IMFActivate** list{}; UINT32 count{}; check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE,nullptr,nullptr,&list,&count));
        bool found=false; for(UINT32 i=0;i<count;++i) { GUID id{}; if(SUCCEEDED(list[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&id)) && id==clsid) found=true; list[i]->Release(); } CoTaskMemFree(list); return found;
    }
    return false;
}
}
Writer::Writer(Options const& o,unsigned w,unsigned h,Microphone const& mic,std::filesystem::path const& path,gpu::Pipeline* pipeline,std::unique_ptr<gpu::Encoder> encoder) : rate(mic.rate),channels(mic.channels),native(std::move(encoder)) {
    if(pipeline)native=pipeline->vendor==0x10de?gpu::nvidia(o,*pipeline):gpu::amd(o,*pipeline);
    Ptr<IMFAttributes> attrs; check_hresult(MFCreateAttributes(&attrs,5));
    check_hresult(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,o.encoder==Encoder::GPU));
    check_hresult(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING,TRUE));
    if(o.encoder==Encoder::GPU && !native) {
        Ptr<ID3D11Device> device; D3D_FEATURE_LEVEL level{};
        check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,nullptr));
        Ptr<IMFDXGIDeviceManager> manager; UINT token{}; check_hresult(MFCreateDXGIDeviceManager(&token,&manager));
        check_hresult(manager->ResetDevice(device.Get(),token)); check_hresult(attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER,manager.Get()));
    }
    check_hresult(MFCreateSinkWriterFromURL(path.c_str(),nullptr,attrs.Get(),&sink));
    auto output=videoType(o.codec==Codec::H264?MFVideoFormat_H264:MFVideoFormat_HEVC,w,h,o.fps);
    if(native){auto extra=native->headers();if(!extra.empty())check_hresult(output->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,extra.data(),static_cast<UINT32>(extra.size())));}
    check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE,o.bitrate)); check_hresult(sink->AddStream(output.Get(),&video));
    auto input=native?output:videoType(MFVideoFormat_NV12,w,h,o.fps); check_hresult(sink->SetInputMediaType(video,input.Get(),nullptr));
    // Verify the actual selected transform. A GPU request must never quietly use CPU.
    if(!native && hardware(sink.Get(),video)!=(o.encoder==Encoder::GPU)) winrt::throw_hresult(MF_E_TOPO_CODEC_NOT_FOUND);
    if(channels) {
        Ptr<IMFMediaType> a; check_hresult(MFCreateMediaType(&a)); check_hresult(a->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));
        check_hresult(a->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC)); check_hresult(a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,channels));
        check_hresult(a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,rate)); check_hresult(a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
        check_hresult(a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,16000));check_hresult(a->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0));check_hresult(sink->AddStream(a.Get(),&audio));
        Ptr<IMFMediaType> pcm;check_hresult(MFCreateMediaType(&pcm));check_hresult(pcm->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));
        check_hresult(pcm->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));check_hresult(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,channels));check_hresult(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,rate));check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
        check_hresult(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,rate*channels*2));check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,channels*2));check_hresult(sink->SetInputMediaType(audio,pcm.Get(),nullptr));
    }
    check_hresult(sink->BeginWriting());
}
void Writer::frame(std::vector<BYTE> const& data,LONGLONG t,LONGLONG d) { auto s=sample(data.data(),static_cast<DWORD>(data.size()),t,d); check_hresult(sink->WriteSample(video,s.Get())); }
void Writer::sound(std::vector<int16_t> const& data,LONGLONG t) { if(data.empty() || audio==MAXDWORD) return; auto s=sample(data.data(),static_cast<DWORD>(data.size()*2),t,LONGLONG(data.size()/channels)*10000000/rate); check_hresult(sink->WriteSample(audio,s.Get())); }
void Writer::packets(std::vector<gpu::Packet> const& packets){for(auto const& p:packets){auto s=sample(p.data.data(),static_cast<DWORD>(p.data.size()),p.time,p.duration);check_hresult(s->SetUINT32(MFSampleExtension_CleanPoint,p.key));check_hresult(sink->WriteSample(video,s.Get()));}}
void Writer::texture(ID3D11Texture2D* t,LONGLONG time,LONGLONG duration){packets(native->encode(t,time,duration));}
void Writer::finish() { if(native)packets(native->flush());check_hresult(sink->Finalize()); sink.Reset(); }
}
