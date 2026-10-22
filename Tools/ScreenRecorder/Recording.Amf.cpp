#include "Recording.Gpu.h"
#include "Vendor/amf/core/Factory.h"
#include "Vendor/amf/core/Buffer.h"
#include "Vendor/amf/components/VideoEncoderVCE.h"
#include "Vendor/amf/components/VideoEncoderHEVC.h"
#include <winrt/base.h>
#include <chrono>
#include <thread>
namespace aelura::recording::gpu {
namespace {
void check(AMF_RESULT status){if(status!=AMF_OK)winrt::throw_hresult(MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,0x700+unsigned(status)));}
struct Amf final:Encoder {
    HMODULE dll{};amf::AMFContextPtr context;amf::AMFComponentPtr component;bool hevc{};bool ended{};
    ~Amf(){if(component){component->Terminate();component=nullptr;}if(context){context->Terminate();context=nullptr;}if(dll)FreeLibrary(dll);}
    void setup(Options const& o,ID3D11Device* device,unsigned w,unsigned h){
        hevc=o.codec==Codec::HEVC;dll=LoadLibraryExW(AMF_DLL_NAME,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)winrt::throw_last_error();
        auto init=reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll,AMF_INIT_FUNCTION_NAME));if(!init)winrt::throw_hresult(E_NOINTERFACE);amf::AMFFactory* factory{};check(init(AMF_FULL_VERSION,&factory));check(factory->CreateContext(&context));check(context->InitDX11(device));
        check(factory->CreateComponent(context,hevc?AMFVideoEncoder_HEVC:AMFVideoEncoderVCE_AVC,&component));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_USAGE:AMF_VIDEO_ENCODER_USAGE,amf_int64(hevc?AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY:AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_FRAMESIZE:AMF_VIDEO_ENCODER_FRAMESIZE,AMFConstructSize(w,h)));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_FRAMERATE:AMF_VIDEO_ENCODER_FRAMERATE,AMFConstructRate(o.fps,1)));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE:AMF_VIDEO_ENCODER_TARGET_BITRATE,amf_int64(o.bitrate)));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD:AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,amf_int64(hevc?AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR:AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)));
        check(component->SetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET:AMF_VIDEO_ENCODER_QUALITY_PRESET,amf_int64(hevc?AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED:AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED)));
        if(hevc){check(component->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,amf_int64(o.fps*2)));check(component->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,amf_int64(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED)));}
        else{check(component->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN,amf_int64(0)));check(component->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD,amf_int64(o.fps*2)));}
        check(component->Init(amf::AMF_SURFACE_NV12,w,h));
    }
    std::vector<BYTE> headers()override{
        amf::AMFVariant value;check(component->GetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_EXTRADATA:AMF_VIDEO_ENCODER_EXTRADATA,&value));amf::AMFBufferPtr bytes(value.pInterface);if(!bytes)winrt::throw_hresult(E_UNEXPECTED);auto p=static_cast<BYTE*>(bytes->GetNative());return {p,p+bytes->GetSize()};
    }
    std::vector<Packet> output(bool draining){
        std::vector<Packet> packets;const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(true){amf::AMFDataPtr data;auto status=component->QueryOutput(&data);if(status==AMF_EOF)return packets;
            if(status!=AMF_REPEAT&&status!=AMF_OK)check(status);
            if(data){amf::AMFBufferPtr bytes(data);if(!bytes)winrt::throw_hresult(E_UNEXPECTED);Packet p;auto raw=static_cast<BYTE*>(bytes->GetNative());p.data.assign(raw,raw+bytes->GetSize());p.time=data->GetPts();p.duration=data->GetDuration();
                amf_int64 type{};data->GetProperty(hevc?AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE:AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE,&type);p.key=type==0;packets.push_back(std::move(p));if(!draining)return packets;
            }else{if(std::chrono::steady_clock::now()>timeout)winrt::throw_hresult(HRESULT_FROM_WIN32(WAIT_TIMEOUT));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        }
    }
    std::vector<Packet> encode(ID3D11Texture2D* frame,LONGLONG time,LONGLONG duration)override{
        amf::AMFSurfacePtr surface;check(context->CreateSurfaceFromDX11Native(frame,&surface,nullptr));surface->SetPts(time);surface->SetDuration(duration);check(component->SubmitInput(surface));return output(false);
    }
    std::vector<Packet> flush()override{if(ended)return {};check(component->Drain());ended=true;return output(true);}
};
}
std::unique_ptr<Encoder> amd(Options const& o,ID3D11Device* device,unsigned w,unsigned h){auto result=std::make_unique<Amf>();result->setup(o,device,w,h);return result;}
std::unique_ptr<Encoder> amd(Options const& o,Pipeline& p){return amd(o,p.device.Get(),p.width,p.height);}
}
