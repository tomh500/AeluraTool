#include "../Tools/ScreenRecorder/Recording.Media.h"
#include "../Tools/ScreenRecorder/Recording.Color.h"
#include <winrt/base.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")
using namespace aelura::recording;
namespace fs = std::filesystem;
namespace {
// Black, white, red, green, blue, and gray in BT.601 limited-range NV12.
constexpr std::array<std::array<int,3>,6> colors{{{16,128,128},{235,128,128},{82,90,240},{144,54,34},{41,240,110},{126,128,128}}};
LRESULT CALLBACK colorWindow(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_PAINT) {
        PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT client{};GetClientRect(hwnd,&client);
        constexpr COLORREF rgb[]{RGB(0,0,0),RGB(255,255,255),RGB(255,0,0),RGB(0,255,0),RGB(0,0,255),RGB(128,128,128)};
        for(int i=0;i<6;++i){RECT band{client.right*i/6,0,client.right*(i+1)/6,client.bottom};auto brush=CreateSolidBrush(rgb[i]);FillRect(dc,&band,brush);DeleteObject(brush);}
        EndPaint(hwnd,&ps);return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
void verify(fs::path const& path,unsigned w,unsigned h) {
    media::Ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader));
    media::Ptr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));
    winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
    winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
    winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()));
    unsigned frames=0;
    for(;;) {
        DWORD flags{};media::Ptr<IMFSample> sample;
        winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample));
        if(sample) {
            media::Ptr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));
            media::Ptr<IMF2DBuffer> image;winrt::check_hresult(buffer.As(&image));
            BYTE* pixels{};LONG stride{};winrt::check_hresult(image->Lock2D(&pixels,&stride));
            bool valid=true;
            for(unsigned band=0;band<colors.size();++band) for(unsigned row:{h/4,h/2,3*h/4}) {
                unsigned x=((2*band+1)*w/12)&~1u;
                auto uv=pixels+size_t(stride)*h+size_t(row/2)*stride+x;
                std::array<int,3> actual{pixels[size_t(row)*stride+x],uv[0],uv[1]};
                for(unsigned c=0;c<3;++c) if(abs(actual[c]-colors[band][c])>12) {
                    std::cout<<"MISMATCH "<<w<<"x"<<h<<" frame="<<frames<<" band="<<band<<" row="<<row<<" component="<<c<<" expected="<<colors[band][c]<<" actual="<<actual[c]<<" decoderStride="<<stride<<"\n";valid=false;
                }
            }
            winrt::check_hresult(image->Unlock2D());
            if(!valid)throw std::runtime_error("decoded NV12 color bands differ");
            ++frames;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
    }
    if(frames<12)throw std::runtime_error("color video lost frames");
}
}
void colorTests(fs::path const& root) {
    for(auto size:{std::array<unsigned,2>{320,240},{2358,1238},{642,478}}) {
        auto [w,h]=size;
        std::vector<BYTE> pixels(size_t(w)*h*3/2);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;x+=2) {
            auto const& color=colors[std::min(5u,x*6/w)];
            pixels[size_t(y)*w+x]=pixels[size_t(y)*w+x+1]=BYTE(color[0]);
            if(!(y&1)) {auto uv=pixels.data()+size_t(w)*h+size_t(y/2)*w+x;uv[0]=BYTE(color[1]);uv[1]=BYTE(color[2]);}
        }
        if(!media::nv12ProbeMatches(pixels.data(),w,pixels.data(),w,h))throw std::runtime_error("color probe rejected correct NV12");
        auto corrupt=pixels;std::fill(corrupt.begin()+size_t(w)*h,corrupt.end(),BYTE(255));
        if(media::nv12ProbeMatches(corrupt.data(),w,pixels.data(),w,h))throw std::runtime_error("color probe accepted purple chroma corruption");
        std::fill(corrupt.begin()+size_t(w)*h,corrupt.end(),BYTE(0));
        if(media::nv12ProbeMatches(corrupt.data(),w,pixels.data(),w,h))throw std::runtime_error("color probe accepted green chroma corruption");
        Options o;o.systemAudio=false;o.encoder=Encoder::CPU;o.bitrate=12000000;
        media::Microphone audio(false);auto path=root/(L"colors-"+std::to_wstring(w)+L".mp4");
        {media::Writer writer(o,w,h,audio,path);for(unsigned i=0;i<12;++i)writer.frame(pixels,LONGLONG(i)*10000000/30,10000000/30);writer.finish();}
        verify(path,w,h);
        auto replay=root/(L"colors-replay-"+std::to_wstring(w)+L".mp4");join({path,path},replay);verify(replay,w,h);
        auto budget=std::make_shared<media::MemoryBudget>(64*1048576);auto memory=std::make_shared<media::MemoryFile>(budget);
        {media::Writer writer(o,w,h,audio,fs::path{},nullptr,nullptr,memory.get());for(unsigned i=0;i<12;++i)writer.frame(pixels,LONGLONG(i)*10000000/30,10000000/30);writer.finish();}
        memory->seal();auto ramReplay=root/(L"colors-ram-replay-"+std::to_wstring(w)+L".mp4");media::joinMemory({memory,memory},ramReplay);verify(ramReplay,w,h);
        std::cout<<"PASS: CPU encoded, disk and memory replay remux colors "<<w<<"x"<<h<<"\n";
    }
    WNDCLASSW wc{};wc.lpfnWndProc=colorWindow;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"AeluraColorRegression";
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)winrt::throw_last_error();
    struct Window {HWND value{};~Window(){if(value)DestroyWindow(value);}} window;
    window.value=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Aelura generated color regression",WS_POPUP,0,0,642,478,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window.value)winrt::throw_last_error();ShowWindow(window.value,SW_SHOWNORMAL);UpdateWindow(window.value);
    Options o;o.systemAudio=false;o.encoder=Encoder::CPU;o.cursor=false;o.bitrate=4000000;
    for(auto size:{std::array<unsigned,2>{642,478},{2358,1238}})for(unsigned backend=0;backend<3;++backend) {
        auto [w,h]=size;
        SetWindowPos(window.value,HWND_TOPMOST,0,0,int(w-1),int(h-1),SWP_SHOWWINDOW);InvalidateRect(window.value,nullptr,FALSE);UpdateWindow(window.value);
        winrt::check_hresult(DwmFlush());
        const bool gpuCapture=backend!=0;
        RECT region{0,0,LONG(w-1),LONG(h-1)};
        media::Frames gdi(o,region);std::unique_ptr<gpu::Pipeline> pipeline;
        if(gpuCapture){
            auto captureOptions=o;captureOptions.encoder=Encoder::GPU;
            try{pipeline=std::make_unique<gpu::Pipeline>(captureOptions,region,window.value,false,backend==2?gpu::ColorConversion::Software:gpu::ColorConversion::Automatic);}
            catch(winrt::hresult_error const& e){if(e.code()!=DXGI_ERROR_UNSUPPORTED)throw;std::cout<<"SKIP: D3D capture unavailable (DXGI_ERROR_UNSUPPORTED), backend "<<backend<<"\n";continue;}
            std::cout<<"CHECK: software color conversion="<<pipeline->usesSoftwareColorConversion()<<"\n";
        }
        auto name=std::to_wstring(w)+L"-"+std::to_wstring(backend);
        auto path=root/(L"capture-"+name+L".mp4");media::Microphone audio(false);
        media::Writer writer(o,w,h,audio,path);
        for(unsigned i=0;i<12;++i) {
            ID3D11Texture2D* texture{};
            if(pipeline) {auto until=GetTickCount64()+5000;while(!(texture=pipeline->next())){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}InvalidateRect(window.value,nullptr,FALSE);UpdateWindow(window.value);if(GetTickCount64()>until)throw std::runtime_error("capture color timeout");Sleep(10);}}
            auto pixels=pipeline?pipeline->readback():gdi.next();
            if(backend==2&&!i){
                D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                gpu::Ptr<ID3D11Texture2D> staging;winrt::check_hresult(pipeline->device->CreateTexture2D(&desc,nullptr,&staging));gpu::Ptr<ID3D11DeviceContext> context;pipeline->device->GetImmediateContext(&context);context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE map{};winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
                bool correct=media::nv12ProbeMatches(static_cast<BYTE const*>(map.pData),map.RowPitch,pixels.data(),w,h);context->Unmap(staging.Get(),0);if(!correct)throw std::runtime_error("software fallback GPU texture differs from CPU NV12");
            }
            writer.frame(pixels,LONGLONG(i)*10000000/30,10000000/30);
        }
        writer.finish();verify(path,w,h);
        auto replay=root/(L"capture-replay-"+name+L".mp4");join({path,path},replay);verify(replay,w,h);
        std::cout<<"PASS: capture backend "<<backend<<" (GDI / automatic D3D11 / software conversion), odd source, encoded and replay colors "<<w<<"x"<<h<<"\n";
    }
}
