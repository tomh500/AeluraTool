#include "Recording.Gpu.h"
#include "Recording.Dx12.h"
#include "Vendor/nvenc/nvEncodeAPI.h"
#include <d3d12.h>
#include <winrt/base.h>
#include <algorithm>
#include <source_location>
#include <string>
namespace aelura::recording::gpu {
namespace {
struct Nvenc12 final:Encoder {
    HMODULE dll{};NV_ENCODE_API_FUNCTION_LIST api{};void* handle{};
    Ptr<ID3D12Device> device;Ptr<ID3D12Resource> output,texture;Ptr<ID3D12Fence> fence;
    HANDLE event{};UINT64 sequence{};unsigned width{},height{};bool ended{},initialized{};
    NV_ENC_REGISTERED_PTR inputRegistration{},outputRegistration{};
    // The driver may retain the D3D12 wrappers through its submission/teardown
    // work. Keep their addresses valid for the complete encoder lifetime.
    NV_ENC_INPUT_RESOURCE_D3D12 input{};
    NV_ENC_OUTPUT_RESOURCE_D3D12 result{};
    void check12(NVENCSTATUS code,std::source_location where=std::source_location::current()){
        if(code==NV_ENC_SUCCESS)return;
        std::string message="NVENC D3D12 line "+std::to_string(where.line());
        if(handle&&api.nvEncGetLastErrorString){auto detail=api.nvEncGetLastErrorString(handle);if(detail){message+=": ";message+=detail;}}
        throw winrt::hresult_error(MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,0x600+unsigned(code)),winrt::to_hstring(message));
    }
    ~Nvenc12(){
        std::lock_guard lock(dx12LifetimeMutex());
        if(handle){if(initialized&&!ended){NV_ENC_PIC_PARAMS eos{};eos.version=NV_ENC_PIC_PARAMS_VER;eos.encodePicFlags=NV_ENC_PIC_FLAG_EOS;api.nvEncEncodePicture(handle,&eos);}if(inputRegistration)api.nvEncUnregisterResource(handle,inputRegistration);if(outputRegistration)api.nvEncUnregisterResource(handle,outputRegistration);api.nvEncDestroyEncoder(handle);}
        fence.Reset();texture.Reset();output.Reset();device.Reset();
        if(event)CloseHandle(event);if(dll)FreeLibrary(dll);
    }
    void setup(Options const& o,ID3D12Device* d,unsigned w,unsigned h){
        device=d;width=w;height=h;dll=LoadLibraryExW(L"nvEncodeAPI64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)winrt::throw_last_error();
        using Create=NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);auto create=reinterpret_cast<Create>(GetProcAddress(dll,"NvEncodeAPICreateInstance"));if(!create)winrt::throw_hresult(E_NOINTERFACE);
        api.version=NV_ENCODE_API_FUNCTION_LIST_VER;check12(create(&api));NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};open.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;open.deviceType=NV_ENC_DEVICE_TYPE_DIRECTX;open.device=device.Get();open.apiVersion=NVENCAPI_VERSION;check12(api.nvEncOpenEncodeSessionEx(&open,&handle));
        auto codec=o.codec==Codec::H264?NV_ENC_CODEC_H264_GUID:NV_ENC_CODEC_HEVC_GUID;NV_ENC_PRESET_CONFIG preset{};preset.version=NV_ENC_PRESET_CONFIG_VER;preset.presetCfg.version=NV_ENC_CONFIG_VER;check12(api.nvEncGetEncodePresetConfigEx(handle,codec,NV_ENC_PRESET_P1_GUID,NV_ENC_TUNING_INFO_LOW_LATENCY,&preset));
        auto config=preset.presetCfg;config.gopLength=o.fps*2;config.frameIntervalP=1;config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CBR;config.rcParams.averageBitRate=config.rcParams.maxBitRate=o.bitrate;config.rcParams.enableLookahead=config.rcParams.enableAQ=0;
        if(o.codec==Codec::H264){config.encodeCodecConfig.h264Config.repeatSPSPPS=1;config.encodeCodecConfig.h264Config.idrPeriod=config.gopLength;}else{config.encodeCodecConfig.hevcConfig.repeatSPSPPS=1;config.encodeCodecConfig.hevcConfig.idrPeriod=config.gopLength;}
        NV_ENC_INITIALIZE_PARAMS init{};init.version=NV_ENC_INITIALIZE_PARAMS_VER;init.encodeGUID=codec;init.presetGUID=NV_ENC_PRESET_P1_GUID;init.tuningInfo=NV_ENC_TUNING_INFO_LOW_LATENCY;init.encodeWidth=init.darWidth=init.maxEncodeWidth=w;init.encodeHeight=init.darHeight=init.maxEncodeHeight=h;init.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;init.frameRateNum=o.fps;init.frameRateDen=1;init.enablePTD=1;init.encodeConfig=&config;check12(api.nvEncInitializeEncoder(handle,&init));
        initialized=true;
        winrt::check_hresult(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)winrt::throw_last_error();
        const auto bytes=std::max(2097152ULL,(UINT64(w)*h*4+65535)&~65535ULL);D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        winrt::check_hresult(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&output)));
        NV_ENC_REGISTER_RESOURCE reg{};reg.version=NV_ENC_REGISTER_RESOURCE_VER;reg.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;reg.resourceToRegister=output.Get();reg.width=w;reg.height=h;reg.bufferFormat=NV_ENC_BUFFER_FORMAT_U8;reg.bufferUsage=NV_ENC_OUTPUT_BITSTREAM;check12(api.nvEncRegisterResource(handle,&reg));outputRegistration=reg.registeredResource;
    }
    std::vector<BYTE> headers()override{std::vector<BYTE> data(8192);uint32_t size{};NV_ENC_SEQUENCE_PARAM_PAYLOAD p{};p.version=NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;p.inBufferSize=unsigned(data.size());p.spsppsBuffer=data.data();p.outSPSPPSPayloadSize=&size;check12(api.nvEncGetSequenceParams(handle,&p));data.resize(size);return data;}
    std::vector<Packet> encode(ID3D11Texture2D*,LONGLONG,LONGLONG)override{winrt::throw_hresult(E_INVALIDARG);}
    std::vector<Packet> encode12(ID3D12Resource* resource,LONGLONG time,LONGLONG duration)override{
        if(!inputRegistration){texture=resource;NV_ENC_REGISTER_RESOURCE reg{};reg.version=NV_ENC_REGISTER_RESOURCE_VER;reg.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;reg.resourceToRegister=resource;reg.width=width;reg.height=height;reg.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;reg.bufferUsage=NV_ENC_INPUT_IMAGE;check12(api.nvEncRegisterResource(handle,&reg));inputRegistration=reg.registeredResource;}
        if(resource!=texture.Get())winrt::throw_hresult(E_INVALIDARG);
            input={};input.version=NV_ENC_INPUT_RESOURCE_D3D12_VER;input.pInputBuffer=inputRegistration;input.inputFencePoint.version=NV_ENC_FENCE_POINT_D3D12_VER;input.inputFencePoint.pFence=fence.Get();input.inputFencePoint.waitValue=sequence;input.inputFencePoint.bWait=1;
            result={};result.version=NV_ENC_OUTPUT_RESOURCE_D3D12_VER;result.pOutputBuffer=outputRegistration;result.outputFencePoint.version=NV_ENC_FENCE_POINT_D3D12_VER;result.outputFencePoint.pFence=fence.Get();result.outputFencePoint.signalValue=++sequence;result.outputFencePoint.bSignal=1;
            NV_ENC_PIC_PARAMS picture{};picture.version=NV_ENC_PIC_PARAMS_VER;picture.inputBuffer=&input;picture.outputBitstream=&result;picture.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;picture.inputWidth=width;picture.inputHeight=height;picture.pictureStruct=NV_ENC_PIC_STRUCT_FRAME;picture.inputTimeStamp=time;picture.inputDuration=duration;check12(api.nvEncEncodePicture(handle,&picture));
            if(fence->GetCompletedValue()<sequence){winrt::check_hresult(fence->SetEventOnCompletion(sequence,event));if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));}
            NV_ENC_LOCK_BITSTREAM lock{};lock.version=NV_ENC_LOCK_BITSTREAM_VER;lock.outputBitstream=&result;check12(api.nvEncLockBitstream(handle,&lock));
            Packet packet;packet.time=LONGLONG(lock.outputTimeStamp);packet.duration=lock.outputDuration?LONGLONG(lock.outputDuration):duration;packet.key=lock.pictureType==NV_ENC_PIC_TYPE_IDR||lock.pictureType==NV_ENC_PIC_TYPE_I;
            try{auto data=static_cast<BYTE*>(lock.bitstreamBufferPtr);packet.data.assign(data,data+lock.bitstreamSizeInBytes);}catch(...){api.nvEncUnlockBitstream(handle,&result);throw;}
            check12(api.nvEncUnlockBitstream(handle,&result));return {std::move(packet)};
    }
    std::vector<Packet> flush()override{if(!ended){NV_ENC_PIC_PARAMS picture{};picture.version=NV_ENC_PIC_PARAMS_VER;picture.encodePicFlags=NV_ENC_PIC_FLAG_EOS;check12(api.nvEncEncodePicture(handle,&picture));ended=true;}return {};}
};
}
std::unique_ptr<Encoder> nvidia12(Options const& o,ID3D12Device* device,unsigned w,unsigned h){std::lock_guard lock(dx12LifetimeMutex());auto encoder=std::make_unique<Nvenc12>();encoder->setup(o,device,w,h);return encoder;}
}
