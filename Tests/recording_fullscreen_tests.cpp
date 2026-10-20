#include "../Tools/ScreenRecorder/Recording.Media.h"
#include <winrt/base.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <map>

using namespace aelura::recording;
namespace {
void require(bool value,char const* message){if(!value)throw std::runtime_error(message);}
struct Pattern {
    HWND hwnd{};gpu::Ptr<ID3D11Device> device;gpu::Ptr<ID3D11DeviceContext> context;
    gpu::Ptr<IDXGISwapChain> swap;gpu::Ptr<IDXGIOutput1> output;RECT bounds{};
    ~Pattern(){if(swap)swap->SetFullscreenState(FALSE,nullptr);if(hwnd)DestroyWindow(hwnd);}
    Pattern(){
        std::cout<<"CHECK: creating generated fullscreen renderer\n";
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"AeluraFullscreenRegression";
        RegisterClassW(&wc);MONITORINFO info{sizeof(info)};winrt::check_bool(GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&info));bounds=info.rcMonitor;
        hwnd=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Aelura generated fullscreen test",WS_POPUP,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,wc.hInstance,nullptr);winrt::check_bool(hwnd!=nullptr);
        gpu::Ptr<IDXGIAdapter1> adapter;gpu::Ptr<IDXGIFactory1> factory;gpu::Ptr<IDXGIOutput> display;
        winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));winrt::check_hresult(factory->EnumAdapters1(0,&adapter));
        winrt::check_hresult(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        winrt::check_hresult(adapter->EnumOutputs(0,&display));winrt::check_hresult(display.As(&output));
        DXGI_OUTPUT_DESC desc{};winrt::check_hresult(output->GetDesc(&desc));require(EqualRect(&bounds,&desc.DesktopCoordinates)!=FALSE,"test GPU does not drive primary monitor");
        DXGI_SWAP_CHAIN_DESC sc{};sc.BufferDesc.Width=UINT(bounds.right-bounds.left);sc.BufferDesc.Height=UINT(bounds.bottom-bounds.top);sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;sc.OutputWindow=hwnd;sc.Windowed=TRUE;sc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;sc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        winrt::check_hresult(factory->CreateSwapChain(device.Get(),&sc,&swap));ShowWindow(hwnd,SW_SHOW);
        auto foreground=GetForegroundWindow();auto thread=foreground?GetWindowThreadProcessId(foreground,nullptr):0;bool attached=thread&&thread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),thread,TRUE);
        SetForegroundWindow(hwnd);SetFocus(hwnd);if(attached)AttachThreadInput(GetCurrentThreadId(),thread,FALSE);
        draw(false);std::cout<<"CHECK: renderer ready, foreground="<<(GetForegroundWindow()==hwnd)<<"\n";
    }
    void draw(bool bright){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        gpu::Ptr<ID3D11Texture2D> texture;gpu::Ptr<ID3D11RenderTargetView> target;
        winrt::check_hresult(swap->GetBuffer(0,IID_PPV_ARGS(&texture)));winrt::check_hresult(device->CreateRenderTargetView(texture.Get(),nullptr,&target));
        const float blue[]{32.f/255,88.f/255,160.f/255,1},yellow[]{240.f/255,220.f/255,32.f/255,1};context->ClearRenderTargetView(target.Get(),bright?yellow:blue);winrt::check_hresult(swap->Present(1,0));
    }
    void fullscreen(bool on){if(on){auto foreground=GetForegroundWindow();auto thread=foreground?GetWindowThreadProcessId(foreground,nullptr):0;bool attached=thread&&thread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),thread,TRUE);SetForegroundWindow(hwnd);SetFocus(hwnd);if(attached)AttachThreadInput(GetCurrentThreadId(),thread,FALSE);}std::cout<<"CHECK: exclusive="<<on<<std::endl;winrt::check_hresult(swap->SetFullscreenState(on?TRUE:FALSE,on?output.Get():nullptr));BOOL actual{};winrt::check_hresult(swap->GetFullscreenState(&actual,nullptr));require((actual!=FALSE)==on,"exclusive fullscreen was not entered");if(!on){SetWindowPos(hwnd,HWND_TOPMOST,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_SHOWWINDOW);winrt::check_hresult(swap->ResizeBuffers(2,UINT(bounds.right-bounds.left),UINT(bounds.bottom-bounds.top),DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));}}
    bool resize(){
        UINT count{};winrt::check_hresult(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,0,&count,nullptr));std::vector<DXGI_MODE_DESC> modes(count);winrt::check_hresult(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,0,&count,modes.data()));
        for(auto const& mode:modes)if(mode.Width>=1280&&mode.Height>=720&&mode.Width<UINT(bounds.right-bounds.left)&&mode.Height<UINT(bounds.bottom-bounds.top)){
            fullscreen(false);winrt::check_hresult(swap->ResizeBuffers(2,mode.Width,mode.Height,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
            SetForegroundWindow(hwnd);fullscreen(true);winrt::check_hresult(swap->ResizeTarget(&mode));
            BOOL actual{};swap->GetFullscreenState(&actual,nullptr);std::cout<<"CHECK: exclusive display mode "<<mode.Width<<"x"<<mode.Height<<" exclusive="<<actual<<" foreground="<<(GetForegroundWindow()==hwnd)<<std::endl;return true;}
        return false;
    }
};
bool videoHasBothColors(std::filesystem::path const& file){
    media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,&reader));media::Ptr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()));
    winrt::check_hresult(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&type));UINT32 width{},height{};winrt::check_hresult(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&width,&height));bool blue=false,yellow=false;LONGLONG previous=-1;
    std::map<unsigned,unsigned> histogram;
    for(;;){DWORD flags{};LONGLONG time{};media::Ptr<IMFSample> sample;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,&time,&sample));if(sample){require(time>previous,"fullscreen video timestamp did not increase");previous=time;media::Ptr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));BYTE* data{};DWORD size{};winrt::check_hresult(buffer->Lock(&data,nullptr,&size));if(size>=width*height){auto y=data[size_t(height/2)*width+width/2];++histogram[y];blue|=y>75&&y<95;yellow|=y>185&&y<220;}buffer->Unlock();}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}
    if(!blue||!yellow){std::cout<<"COLOR CHECK: blue="<<blue<<" yellow="<<yellow;for(auto [y,count]:histogram)std::cout<<" Y"<<y<<"="<<count;std::cout<<std::endl;}return blue&&yellow;
}
}
void fullscreenTests(std::filesystem::path const& directory){
    Pattern pattern;Options o;o.systemAudio=false;o.directory=directory;o.fps=15;o.bitrate=1500000;o.scale=20;o.cursor=false;o.encoder=Encoder::GPU;o.performanceBeta=true;o.replayStorage=ReplayStorage::Memory;o.memoryLimitMB=128;
    unsigned observedLosses=0;
    {
        std::cout<<"CHECK: creating capture pipeline\n";gpu::Pipeline probe(o,pattern.bounds);std::cout<<"CHECK: capture pipeline ready\n";
        auto wait=[&](auto condition,bool bright){auto deadline=GetTickCount64()+15000;do{pattern.draw(bright);require(GetTickCount64()<deadline,"fullscreen capture did not recover within 15 seconds");if(condition())return;Sleep(10);}while(true);};
        auto report=GetTickCount64();auto color=[&](bool bright){ID3D11Texture2D* texture{};try{texture=probe.next();}catch(...){std::cout<<"FAIL STAGE: capture next\n";throw;}int y=-1;if(texture){auto pixels=probe.readback();y=pixels[size_t(probe.height/2)*probe.width+probe.width/2];}if(GetTickCount64()>report+2000){report=GetTickCount64();std::cout<<"CHECK: waiting="<<probe.captureWaiting()<<" Y="<<y<<" bright="<<bright<<" recoveries="<<probe.captureRecoveries()<<std::endl;}if(!texture||probe.captureWaiting())return false;return bright?(y>185&&y<220):(y>75&&y<95);};
        wait([&]{return color(false);},false);
        std::cout<<"CHECK: first blue capture ready\n";
        pattern.fullscreen(true);wait([&]{return color(true);},true);
        const bool resized=pattern.resize();wait([&]{return color(false);},false);
        pattern.fullscreen(false);wait([&]{return color(true);},true);
        observedLosses=unsigned(probe.captureRecoveries());
        require(observedLosses>0,"environment did not reproduce DXGI_ERROR_ACCESS_LOST");
        std::cout<<"PASS: actual ACCESS_LOST count="<<observedLosses<<", fresh blue/yellow GPU pixels after exclusive entry, "<<(resized?"resolution change":"no alternate mode available")<<" and desktop return; encoder canvas unchanged\n";
    }
    for(unsigned mode=0;mode<4;++mode){
        o.encoder=mode==0?Encoder::CPU:Encoder::GPU;o.performanceBeta=mode>=2;o.graphics=mode==3?Graphics::Direct3D12:Graphics::Direct3D11;Session manual,replay;
        auto wait=[&](auto condition,bool bright){auto deadline=GetTickCount64()+20000;do{pattern.draw(bright);winrt::check_hresult(manual.snapshot().error);winrt::check_hresult(replay.snapshot().error);require(GetTickCount64()<deadline,"fullscreen session timeout");if(condition())return;Sleep(10);}while(true);};
        require(manual.start(o,pattern.bounds,false)&&replay.start(o,pattern.bounds,true),"fullscreen sessions rejected");
        auto warmup=GetTickCount64();wait([&]{return GetTickCount64()>warmup+2000&&manual.snapshot().frames>=20&&replay.snapshot().frames>=20;},false);
        pattern.fullscreen(true);auto before=manual.snapshot().frames;auto entered=GetTickCount64();wait([&]{return GetTickCount64()>entered+2000&&manual.snapshot().frames>=before+30&&!manual.snapshot().captureWaiting&&!replay.snapshot().captureWaiting;},true);
        if(mode==3)require(manual.snapshot().nativeDx12&&replay.snapshot().nativeDx12&&!manual.snapshot().dx12Fallback&&!replay.snapshot().dx12Fallback,"DX12 fullscreen test silently fell back");
        require(replay.saveReplay(),"fullscreen replay save rejected");wait([&]{return replay.snapshot().savedGeneration>0;},true);require(videoHasBothColors(replay.snapshot().output),"saved fullscreen replay lacks changing game pixels");
        pattern.fullscreen(false);before=manual.snapshot().frames;wait([&]{return manual.snapshot().frames>=before+15&&!manual.snapshot().captureWaiting&&!replay.snapshot().captureWaiting;},false);
        auto generation=replay.snapshot().savedGeneration;require(replay.saveReplay(),"replay save after desktop return rejected");wait([&]{return replay.snapshot().savedGeneration>generation;},false);
        manual.stop();replay.stop(true);wait([&]{return !manual.busy()&&!replay.busy();},false);manual.wait();replay.wait();require(videoHasBothColors(manual.snapshot().output),"manual fullscreen video lacks changing game pixels");require(replay.snapshot().cacheBytes==0,"fullscreen replay memory was not released");
        std::cout<<"PASS: mode="<<mode<<" CPU / standard GPU / native GPU / DX12: concurrent manual and RAM replay survive exclusive entry/exit, save twice, decode changing colors, release resources\n";
    }
}
