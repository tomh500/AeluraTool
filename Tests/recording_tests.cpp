#include "../Tools/ScreenRecorder/Recording.Media.h"
#include <winrt/base.h>
#include <mferror.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace aelura::recording;
namespace fs=std::filesystem;
void check(bool value,char const* reason){if(!value)throw std::runtime_error(reason);}
uint64_t decode(fs::path const& file,bool audio=false){
    media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,&reader));
    media::Ptr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
    winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE));winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()));
    uint64_t count=0;LONGLONG previous=-1;
    while(true){DWORD flags{};LONGLONG time{};media::Ptr<IMFSample>s;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,&time,&s));if(s){check(time>previous,"non-monotonic video timestamps");previous=time;++count;}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}
    if(audio){media::Ptr<IMFMediaType>a;winrt::check_hresult(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),0,&a));GUID subtype{};winrt::check_hresult(a->GetGUID(MF_MT_SUBTYPE,&subtype));check(subtype==MFAudioFormat_AAC,"missing AAC track");
        winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),TRUE));PROPVARIANT position{};position.vt=VT_I8;position.hVal.QuadPart=0;winrt::check_hresult(reader->SetCurrentPosition(GUID_NULL,position));
        LONGLONG previousEnd=-1;unsigned packets=0;while(true){DWORD flags{};LONGLONG time{};media::Ptr<IMFSample>sample;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),0,nullptr,&flags,&time,&sample));if(sample){LONGLONG duration{};winrt::check_hresult(sample->GetSampleDuration(&duration));check(time+1000>=previousEnd,"overlapping AAC timestamps after remux");previousEnd=time+duration;++packets;}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}check(packets>0,"empty AAC track");
    }
    return count;
}
void writeVideo(fs::path const& path,Options o,bool audio=false,gpu::Pipeline* gpu=nullptr){
    media::Microphone mic(false);if(audio){mic.rate=48000;mic.channels=2;}
    media::Writer writer(o,320,240,mic,path,gpu);
    std::vector<BYTE> pixels(320*240*3/2,BYTE(128));std::fill(pixels.begin(),pixels.begin()+320*240,BYTE(80));
    gpu::Ptr<ID3D11Texture2D> texture;
    if(gpu){D3D11_TEXTURE2D_DESC d{};d.Width=320;d.Height=240;d.Format=DXGI_FORMAT_NV12;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{pixels.data(),320,0};winrt::check_hresult(gpu->device->CreateTexture2D(&d,&initial,&texture));}
    for(unsigned i=0;i<30;++i){if(gpu)writer.texture(texture.Get(),LONGLONG(i)*10000000/30,10000000/30);else writer.frame(pixels,LONGLONG(i)*10000000/30,10000000/30);if(audio){std::vector<int16_t> samples(1600*2);writer.sound(samples,LONGLONG(i)*10000000/30);}}
    writer.finish();
}
void inspect(fs::path const& path){
    media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader));PROPVARIANT duration{};winrt::check_hresult(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration));
    media::Ptr<IMFMediaType> type;winrt::check_hresult(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&type));UINT32 w{},h{};winrt::check_hresult(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h));
    std::cout<<"VIDEO "<<w<<"x"<<h<<" seconds="<<double(duration.uhVal.QuadPart)/10000000<<" frames="<<decode(path)<<"\n";PropVariantClear(&duration);
}
LRESULT CALLBACK patternWindow(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);auto brush=CreateSolidBrush(RGB(32,88,160));FillRect(dc,&r,brush);DeleteObject(brush);SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,L"Generated recording test pattern",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);EndPaint(hwnd,&ps);return 0;}
    if(message==WM_TIMER){DestroyWindow(hwnd);return 0;}if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,message,wp,lp);
}
int wmain(int argc,wchar_t** argv){
    fs::path root=fs::temp_directory_path()/(L"AeluraRecordingTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        media::Runtime runtime;
        if(argc==2&&std::wstring(argv[1])==L"--pattern"){
            WNDCLASSW wc{};wc.lpfnWndProc=patternWindow;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"AeluraRecordingTestPattern";check(RegisterClassW(&wc)!=0,"test window registration failed");
            auto window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Aelura recording test pattern",WS_OVERLAPPEDWINDOW,20,150,480,320,nullptr,nullptr,wc.hInstance,nullptr);check(window!=nullptr,"test window creation failed");ShowWindow(window,SW_SHOWNORMAL);ShowWindow(window,SW_SHOW);UpdateWindow(window);SetTimer(window,1,180000,nullptr);MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return 0;
        }
        if(argc==2&&std::wstring(argv[1])==L"--probe-microphone"){
            media::Microphone mic(true);std::this_thread::sleep_for(std::chrono::milliseconds(150));auto pcm=mic.read();std::cout<<"MICROPHONE rate="<<mic.rate<<" channels="<<mic.channels<<" samples="<<pcm.size()<<" (discarded, not saved)\n";return 0;
        }
        if(argc==3&&std::wstring(argv[1])==L"--inspect"){inspect(argv[2]);return 0;}
        if(argc==4&&std::wstring(argv[1])==L"--recover"){
            Session recovery;check(recovery.recover(argv[2],argv[3]),"recovery rejected");while(recovery.busy())std::this_thread::sleep_for(std::chrono::milliseconds(10));auto s=recovery.snapshot();winrt::check_hresult(s.error);check(s.state==State::Completed,"recovery failed");std::cout<<"RECOVERED "<<s.output<<"\n";inspect(s.output);return 0;
        }
        check(argc==1,"unknown arguments");check(fs::create_directory(root),"temporary sandbox creation failed");
        Options o;o.directory=root/L"中文视频";fs::create_directory(o.directory);
        auto rejects=[&](Options const& value,RECT r){try{validate(value,r);return false;}catch(...){return true;}};
        validate(o,{0,0,320,240});auto bad=o;bad.fps=0;check(rejects(bad,{0,0,320,240}),"zero fps accepted");bad=o;bad.bitrate=0;check(rejects(bad,{0,0,320,240}),"zero bitrate accepted");check(rejects(o,{0,0,1,240}),"one pixel region accepted");
        RECT r=selectionRect({200,300},{-500,-400},{-100,-100,100,100});check(r.left==-100&&r.top==-100&&r.right==100&&r.bottom==100,"negative-monitor selection failed");
        check(replayFirst({5,5,5},10)==1&&replayFirst({5,5},300)==0&&replayFirst({},300)==0,"replay retention failed");std::vector<double> longRun(1000,5);check(replayFirst(longRun,300)==940,"five minute retention failed");
        o.performanceBeta=true;o.encoder=Encoder::GPU;save(root/L"settings.json",o);auto loaded=load(root/L"settings.json");check(loaded.directory==o.directory&&loaded.performanceBeta&&loaded.encoder==Encoder::GPU,"settings roundtrip failed");
        {std::ofstream f(root/L"corrupt.json");f<<"broken";}check(load(root/L"corrupt.json").fps==30,"corrupt settings fallback failed");
        o.encoder=Encoder::CPU;o.performanceBeta=false;o.fps=30;o.bitrate=1500000;
        std::cout<<"CHECK: CPU encode "<<root<<std::endl;writeVideo(root/L"one.mp4",o);std::cout<<"CHECK: CPU decode"<<std::endl;check(decode(root/L"one.mp4")==30,"CPU H264 decode failed");
        writeVideo(root/L"two.mp4",o);std::cout<<"CHECK: remux"<<std::endl;join({root/L"one.mp4",root/L"two.mp4"},root/L"joined.mp4");check(decode(root/L"joined.mp4")==60,"replay remux lost frames");
        bool refused=false;try{join({root/L"one.mp4"},root/L"joined.mp4");}catch(...){refused=true;}check(refused&&decode(root/L"joined.mp4")==60,"existing target overwritten");
        bool canceled=false;try{join({root/L"one.mp4",root/L"two.mp4"},root/L"canceled.mp4",[](double){return false;});}catch(winrt::hresult_error const& e){canceled=e.code()==E_ABORT;}check(canceled&&fs::exists(root/L"one.mp4"),"cancel lost source");
        std::cout<<"CHECK: AAC"<<std::endl;writeVideo(root/L"audio.mp4",o,true);check(decode(root/L"audio.mp4",true)==30,"AAC mux failed");join({root/L"audio.mp4",root/L"audio.mp4"},root/L"audio-joined.mp4");check(decode(root/L"audio-joined.mp4",true)==60,"AAC remux failed");
        std::cout<<"PASS: CPU H264, AAC, decoded frame counts, remux, timestamps, cancellation, no overwrite, settings, five minute retention\n";
        {auto cache=root/L"recovery";fs::create_directory(cache);fs::copy_file(root/L"one.mp4",cache/L"one.mp4");fs::copy_file(root/L"two.mp4",cache/L"two.mp4");
            {std::ofstream journal(cache/L"session.json");journal<<R"({"segments":[{"path":"C:/previous-location/one.mp4"},{"path":"C:/previous-location/two.mp4"}]})";}
            Session recover;check(recover.recover(cache,root),"recovery start failed");while(recover.busy())std::this_thread::sleep_for(std::chrono::milliseconds(10));auto result=recover.snapshot();winrt::check_hresult(result.error);check(result.state==State::Completed&&decode(result.output)==60,"relocated replay recovery failed");
            check(fs::exists(cache/L"one.mp4"),"recovery removed source");std::cout<<"PASS: relocated recovery journal and retained originals\n";
        }
        for(auto codec:{Codec::H264,Codec::HEVC}){
            auto hardware=o;hardware.encoder=Encoder::GPU;hardware.codec=codec;
            try{auto file=root/(codec==Codec::H264?L"gpu-h264.mp4":L"gpu-hevc.mp4");writeVideo(file,hardware);check(decode(file)==30,"GPU decode mismatch");std::cout<<"PASS: standard GPU "<<(codec==Codec::H264?"H264":"HEVC")<<"\n";}
            catch(winrt::hresult_error const& e){std::cout<<"LIMIT: standard GPU "<<int(codec)<<" HRESULT 0x"<<std::hex<<unsigned(e.code())<<std::dec<<"\n";}
        }
        try{
            auto optimized=o;optimized.encoder=Encoder::GPU;optimized.performanceBeta=true;gpu::Pipeline pipeline(optimized,{0,0,320,240});
            for(auto codec:{Codec::H264,Codec::HEVC}){optimized.codec=codec;auto file=root/(codec==Codec::H264?L"native-h264.mp4":L"native-hevc.mp4");writeVideo(file,optimized,false,&pipeline);check(decode(file)==30,"native encode decode mismatch");std::cout<<"PASS: vendor "<<std::hex<<pipeline.vendor<<std::dec<<" native "<<int(codec)<<" synthetic texture encoding\n";}
        }catch(winrt::hresult_error const& e){std::cout<<"LIMIT: native GPU HRESULT 0x"<<std::hex<<unsigned(e.code())<<std::dec<<"\n";}
        media::Ptr<IDXGIFactory1> factory;winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(UINT index=0;;++index){media::Ptr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};winrt::check_hresult(adapter->GetDesc1(&desc));if(desc.VendorId!=0x1002)continue;
            for(auto format:{Codec::H264,Codec::HEVC}){
                try{media::Ptr<ID3D11Device> device;winrt::check_hresult(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr));
                    auto options=o;options.codec=format;options.encoder=Encoder::GPU;auto encoder=gpu::amd(options,device.Get(),320,240);media::Microphone mic(false);auto file=root/(format==Codec::H264?L"amf-h264.mp4":L"amf-hevc.mp4");media::Writer writer(options,320,240,mic,file,nullptr,std::move(encoder));
                    std::vector<BYTE> pixels(320*240*3/2,BYTE(128));std::fill(pixels.begin(),pixels.begin()+320*240,BYTE(80));D3D11_TEXTURE2D_DESC d{};d.Width=320;d.Height=240;d.Format=DXGI_FORMAT_NV12;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{pixels.data(),320,0};media::Ptr<ID3D11Texture2D> texture;winrt::check_hresult(device->CreateTexture2D(&d,&initial,&texture));
                    for(unsigned frame=0;frame<30;++frame)writer.texture(texture.Get(),LONGLONG(frame)*10000000/30,10000000/30);writer.finish();check(decode(file)==30,"AMF decode mismatch");std::cout<<"PASS: AMD AMF "<<int(format)<<" synthetic texture encoding and decode\n";
                }catch(winrt::hresult_error const& e){std::cout<<"LIMIT: AMD AMF "<<int(format)<<" HRESULT 0x"<<std::hex<<unsigned(e.code())<<std::dec<<"\n";}
            }
        }
        fs::remove_all(root);std::cout<<"PASS: recording tests completed\n";return 0;
    }catch(winrt::hresult_error const& e){std::wcerr<<L"FAIL: 0x"<<std::hex<<unsigned(e.code())<<L" "<<e.message().c_str()<<L" sandbox "<<root<<L"\n";return 1;}
    catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<" sandbox "<<root<<"\n";return 1;}
}
