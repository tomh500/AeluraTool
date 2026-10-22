#include "Recording.Gpu.h"
#include "Vendor/nvenc/nvEncodeAPI.h"
#include <winrt/base.h>
namespace aelura::recording::gpu {
namespace {
void check(NVENCSTATUS status){if(status!=NV_ENC_SUCCESS)winrt::throw_hresult(MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,0x600+unsigned(status)));}
struct Nvenc final:Encoder {
    HMODULE dll{};NV_ENCODE_API_FUNCTION_LIST api{};void* handle{};NV_ENC_OUTPUT_PTR bitstream{};NV_ENC_REGISTERED_PTR registered{};
    Ptr<ID3D11Texture2D> texture;unsigned width{},height{};bool ended{};
    ~Nvenc(){
        if(handle){if(registered)api.nvEncUnregisterResource(handle,registered);if(bitstream)api.nvEncDestroyBitstreamBuffer(handle,bitstream);api.nvEncDestroyEncoder(handle);}if(dll)FreeLibrary(dll);
    }
    void setup(Options const& o,ID3D11Device* device,unsigned w,unsigned h){
        width=w;height=h;
        dll=LoadLibraryExW(L"nvEncodeAPI64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)winrt::throw_last_error();
        using Create=NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        auto create=reinterpret_cast<Create>(GetProcAddress(dll,"NvEncodeAPICreateInstance"));if(!create)winrt::throw_hresult(E_NOINTERFACE);
        api.version=NV_ENCODE_API_FUNCTION_LIST_VER;check(create(&api));
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};open.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;open.deviceType=NV_ENC_DEVICE_TYPE_DIRECTX;open.device=device;open.apiVersion=NVENCAPI_VERSION;check(api.nvEncOpenEncodeSessionEx(&open,&handle));
        const auto codec=o.codec==Codec::H264?NV_ENC_CODEC_H264_GUID:NV_ENC_CODEC_HEVC_GUID;
        NV_ENC_PRESET_CONFIG preset{};preset.version=NV_ENC_PRESET_CONFIG_VER;preset.presetCfg.version=NV_ENC_CONFIG_VER;
        check(api.nvEncGetEncodePresetConfigEx(handle,codec,NV_ENC_PRESET_P1_GUID,NV_ENC_TUNING_INFO_LOW_LATENCY,&preset));
        auto config=preset.presetCfg;config.gopLength=o.fps*2;config.frameIntervalP=1;config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CBR;config.rcParams.averageBitRate=o.bitrate;config.rcParams.maxBitRate=o.bitrate;config.rcParams.enableLookahead=0;config.rcParams.enableAQ=0;
        if(o.codec==Codec::H264){config.encodeCodecConfig.h264Config.repeatSPSPPS=1;config.encodeCodecConfig.h264Config.idrPeriod=config.gopLength;}
        else{config.encodeCodecConfig.hevcConfig.repeatSPSPPS=1;config.encodeCodecConfig.hevcConfig.idrPeriod=config.gopLength;}
        NV_ENC_INITIALIZE_PARAMS init{};init.version=NV_ENC_INITIALIZE_PARAMS_VER;init.encodeGUID=codec;init.presetGUID=NV_ENC_PRESET_P1_GUID;init.tuningInfo=NV_ENC_TUNING_INFO_LOW_LATENCY;init.encodeWidth=init.darWidth=width;init.encodeHeight=init.darHeight=height;init.frameRateNum=o.fps;init.frameRateDen=1;init.enablePTD=1;init.encodeConfig=&config;
        check(api.nvEncInitializeEncoder(handle,&init));NV_ENC_CREATE_BITSTREAM_BUFFER buffer{};buffer.version=NV_ENC_CREATE_BITSTREAM_BUFFER_VER;check(api.nvEncCreateBitstreamBuffer(handle,&buffer));bitstream=buffer.bitstreamBuffer;
    }
    std::vector<BYTE> headers()override{
        std::vector<BYTE> bytes(8192);uint32_t count{};NV_ENC_SEQUENCE_PARAM_PAYLOAD p{};p.version=NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;p.inBufferSize=unsigned(bytes.size());p.spsppsBuffer=bytes.data();p.outSPSPPSPayloadSize=&count;check(api.nvEncGetSequenceParams(handle,&p));bytes.resize(count);return bytes;
    }
    std::vector<Packet> encode(ID3D11Texture2D* frame,LONGLONG time,LONGLONG duration)override{
        if(!registered){texture=frame;NV_ENC_REGISTER_RESOURCE r{};r.version=NV_ENC_REGISTER_RESOURCE_VER;r.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;r.resourceToRegister=frame;r.width=width;r.height=height;r.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;r.bufferUsage=NV_ENC_INPUT_IMAGE;check(api.nvEncRegisterResource(handle,&r));registered=r.registeredResource;}
        NV_ENC_MAP_INPUT_RESOURCE map{};map.version=NV_ENC_MAP_INPUT_RESOURCE_VER;map.registeredResource=registered;check(api.nvEncMapInputResource(handle,&map));
        try{
            NV_ENC_PIC_PARAMS pic{};pic.version=NV_ENC_PIC_PARAMS_VER;pic.inputBuffer=map.mappedResource;pic.bufferFmt=map.mappedBufferFmt;pic.inputWidth=width;pic.inputHeight=height;pic.outputBitstream=bitstream;pic.pictureStruct=NV_ENC_PIC_STRUCT_FRAME;pic.inputTimeStamp=time;pic.inputDuration=duration;
            auto status=api.nvEncEncodePicture(handle,&pic);if(status==NV_ENC_ERR_NEED_MORE_INPUT)winrt::throw_hresult(E_UNEXPECTED);check(status);
            NV_ENC_LOCK_BITSTREAM lock{};lock.version=NV_ENC_LOCK_BITSTREAM_VER;lock.outputBitstream=bitstream;check(api.nvEncLockBitstream(handle,&lock));
            Packet packet;packet.time=LONGLONG(lock.outputTimeStamp);packet.duration=LONGLONG(lock.outputDuration);if(!packet.duration)packet.duration=duration;packet.key=lock.pictureType==NV_ENC_PIC_TYPE_IDR||lock.pictureType==NV_ENC_PIC_TYPE_I;
            try{auto p=static_cast<BYTE*>(lock.bitstreamBufferPtr);packet.data.assign(p,p+lock.bitstreamSizeInBytes);}catch(...){api.nvEncUnlockBitstream(handle,bitstream);throw;}
            check(api.nvEncUnlockBitstream(handle,bitstream));check(api.nvEncUnmapInputResource(handle,map.mappedResource));return {std::move(packet)};
        }catch(...){api.nvEncUnmapInputResource(handle,map.mappedResource);throw;}
    }
    std::vector<Packet> flush()override{if(!ended){NV_ENC_PIC_PARAMS p{};p.version=NV_ENC_PIC_PARAMS_VER;p.encodePicFlags=NV_ENC_PIC_FLAG_EOS;check(api.nvEncEncodePicture(handle,&p));ended=true;}return {};}
};
}
std::unique_ptr<Encoder> nvidia(Options const& o,ID3D11Device* device,unsigned w,unsigned h){auto result=std::make_unique<Nvenc>();result->setup(o,device,w,h);return result;}
std::unique_ptr<Encoder> nvidia(Options const& o,Pipeline& p){return nvidia(o,p.device.Get(),p.width,p.height);}
}
