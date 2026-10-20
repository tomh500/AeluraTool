#include "Recording.Gpu.h"
#include "Recording.Window.h"
#include "Recording.Desktop.h"
#include <winrt/base.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
namespace aelura::recording::gpu {
using winrt::check_hresult;
namespace {
constexpr char shader[]=R"(
Texture2D frame:register(t0);Texture2D pointerImage:register(t1);SamplerState linearFilter:register(s0);SamplerState pointFilter:register(s1);
cbuffer Parameters:register(b0){float4 crop;float4 sizes;float4 pointerRect;float4 flags;};
struct Vertex{float4 position:SV_Position;float2 uv:TEXCOORD;};
Vertex vs(uint id:SV_VertexID){Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float cubic(float x){x=abs(x);if(x<=1)return 1.5*x*x*x-2.5*x*x+1;if(x<2)return -.5*x*x*x+2.5*x*x-4*x+2;return 0;}
float4 ps(Vertex v):SV_Target{
 float2 uv=crop.xy+v.uv*crop.zw;float4 color=0;
 if(flags.x<.5)color=frame.SampleLevel(pointFilter,uv,0);
 else if(flags.x<1.5)color=frame.SampleLevel(linearFilter,uv,0);
 else if(flags.x<2.5){float2 p=uv*sizes.xy-.5,base=floor(p),f=p-base;float sum=0;[unroll]for(int y=-1;y<=2;++y)[unroll]for(int x=-1;x<=2;++x){float weight=cubic(x-f.x)*cubic(y-f.y);color+=frame.SampleLevel(pointFilter,(base+float2(x,y)+.5)/sizes.xy,0)*weight;sum+=weight;}color/=sum;}
 else{float2 step=crop.zw/sizes.zw;[unroll]for(int y=0;y<4;++y)[unroll]for(int x=0;x<4;++x)color+=frame.SampleLevel(linearFilter,uv+(float2(x,y)/3-.5)*step,0)/16;}
 float2 cp=(v.position.xy-pointerRect.xy)/pointerRect.zw;if(flags.y>.5&&all(cp>=0)&&all(cp<1)){float4 c=pointerImage.SampleLevel(linearFilter,cp,0);color.rgb=c.rgb+color.rgb*(1-c.a);}
 return float4(saturate(color.rgb),1);
})";
}
struct Pipeline::Impl {
    Pipeline* owner;Options options;RECT region{},monitor{},originalRegion{},originalMonitor{};
    std::shared_ptr<DesktopCapture> capture;bool wholeMonitor{},waiting{},converted{},bgraOnly{};uint64_t generation{};
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> desktop,scaled,nv12,cursor;
    Ptr<ID3D11Texture2D> staging;
    std::unique_ptr<WindowCapture> window;
    Ptr<ID3D11ShaderResourceView> desktopView,cursorView;Ptr<ID3D11RenderTargetView> target;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11SamplerState> linear,point;Ptr<ID3D11VideoDevice> video;Ptr<ID3D11VideoContext> videoContext;
    Ptr<ID3D11VideoProcessorEnumerator> enumerator;Ptr<ID3D11VideoProcessor> processor;
    Ptr<ID3D11VideoProcessorInputView> input;Ptr<ID3D11VideoProcessorOutputView> output;
    HCURSOR lastCursor{};unsigned cursorWidth{1},cursorHeight{1};POINT hotspot{};
    explicit Impl(Pipeline* p):owner(p){}
    void setup(Options const& o,RECT r,HWND hwnd){
        options=o;region=r;
        Ptr<IDXGIFactory1> factory;check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        Ptr<IDXGIAdapter1> chosen;Ptr<IDXGIOutput1> chosenOutput;
        for(UINT a=0;!chosen;++a){Ptr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};check_hresult(candidate->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            for(UINT n=0;;++n){Ptr<IDXGIOutput> display;if(candidate->EnumOutputs(n,&display)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC d{};check_hresult(display->GetDesc(&d));RECT intersection{};
                if(d.AttachedToDesktop&&((hwnd&&MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST)==d.Monitor)||(!hwnd&&IntersectRect(&intersection,&r,&d.DesktopCoordinates)&&EqualRect(&intersection,&r)))){
                    if(!hwnd&&d.Rotation!=DXGI_MODE_ROTATION_IDENTITY)winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
                    chosen=candidate;check_hresult(display.As(&chosenOutput));monitor=d.DesktopCoordinates;owner->vendor=desc.VendorId;owner->adapter=desc.Description;break;
                }
            }
        }
        if(!chosen)winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
        D3D_FEATURE_LEVEL level{};check_hresult(D3D11CreateDevice(chosen.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&owner->device,&level,&context));
        if(hwnd){window=std::make_unique<WindowCapture>(hwnd,owner->device.Get(),o.cursor);region=monitor={0,0,LONG(window->width),LONG(window->height)};}
        else {capture=DesktopCapture::share(owner->device.Get(),chosenOutput.Get());owner->device=capture->device;context=capture->context;originalRegion=region;originalMonitor=monitor;wholeMonitor=EqualRect(&region,&monitor)!=FALSE;waiting=true;}
        owner->width=outputExtent(unsigned(region.right-region.left),o.scale);owner->height=outputExtent(unsigned(region.bottom-region.top),o.scale);
        D3D11_TEXTURE2D_DESC d{};d.Width=monitor.right-monitor.left;d.Height=monitor.bottom-monitor.top;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        check_hresult(owner->device->CreateTexture2D(&d,nullptr,&desktop));check_hresult(owner->device->CreateShaderResourceView(desktop.Get(),nullptr,&desktopView));
        d.Width=owner->width;d.Height=owner->height;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        check_hresult(owner->device->CreateTexture2D(&d,nullptr,&scaled));check_hresult(owner->device->CreateRenderTargetView(scaled.Get(),nullptr,&target));
        d.Format=DXGI_FORMAT_NV12;if(!bgraOnly)check_hresult(owner->device->CreateTexture2D(&d,nullptr,&nv12));
        Ptr<ID3DBlob> blob,error;check_hresult(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));check_hresult(owner->device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&vs));
        check_hresult(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));check_hresult(owner->device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&ps));
        D3D11_BUFFER_DESC b{};b.ByteWidth=64;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check_hresult(owner->device->CreateBuffer(&b,nullptr,&constants));
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;check_hresult(owner->device->CreateSamplerState(&s,&linear));s.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;check_hresult(owner->device->CreateSamplerState(&s,&point));
        if(!bgraOnly){check_hresult(owner->device.As(&video));check_hresult(context.As(&videoContext));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC c{};c.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;c.InputWidth=c.OutputWidth=owner->width;c.InputHeight=c.OutputHeight=owner->height;c.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;c.InputFrameRate=c.OutputFrameRate={o.fps,1};
        check_hresult(video->CreateVideoProcessorEnumerator(&c,&enumerator));check_hresult(video->CreateVideoProcessor(enumerator.Get(),0,&processor));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};iv.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;check_hresult(video->CreateVideoProcessorInputView(scaled.Get(),enumerator.Get(),&iv,&input));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};ov.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;check_hresult(video->CreateVideoProcessorOutputView(nv12.Get(),enumerator.Get(),&ov,&output));
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};rgb.RGB_Range=0;rgb.YCbCr_Matrix=0;rgb.Nominal_Range=2;videoContext->VideoProcessorSetStreamColorSpace(processor.Get(),0,&rgb);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};yuv.YCbCr_Matrix=0;yuv.Nominal_Range=1;videoContext->VideoProcessorSetOutputColorSpace(processor.Get(),&yuv);
        }
        // Make a valid transparent cursor resource even when pointer recording is off.
        BYTE transparent[4]{};D3D11_SUBRESOURCE_DATA init{transparent,4,0};d.Width=d.Height=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;check_hresult(owner->device->CreateTexture2D(&d,&init,&cursor));check_hresult(owner->device->CreateShaderResourceView(cursor.Get(),nullptr,&cursorView));
    }
    void cursorImage(HCURSOR handle){
        ICONINFO icon{};if(!GetIconInfo(handle,&icon))return;
        BITMAP size{};GetObjectW(icon.hbmColor?icon.hbmColor:icon.hbmMask,sizeof(size),&size);cursorWidth=std::clamp(unsigned(size.bmWidth),1u,256u);cursorHeight=std::clamp(unsigned(icon.hbmColor?size.bmHeight:size.bmHeight/2),1u,256u);hotspot={LONG(icon.xHotspot),LONG(icon.yHotspot)};
        HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),LONG(cursorWidth),-LONG(cursorHeight),1,32,BI_RGB};void* p{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&p,nullptr,0);
        std::vector<BYTE> black(size_t(cursorWidth)*cursorHeight*4),white(black.size());
        if(bitmap&&dc){auto old=SelectObject(dc,bitmap);memset(p,0,black.size());DrawIconEx(dc,0,0,handle,cursorWidth,cursorHeight,0,nullptr,DI_NORMAL);GdiFlush();memcpy(black.data(),p,black.size());memset(p,255,white.size());DrawIconEx(dc,0,0,handle,cursorWidth,cursorHeight,0,nullptr,DI_NORMAL);GdiFlush();memcpy(white.data(),p,white.size());SelectObject(dc,old);}
        if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);if(icon.hbmColor)DeleteObject(icon.hbmColor);if(icon.hbmMask)DeleteObject(icon.hbmMask);
        for(size_t i=0;i<black.size();i+=4)black[i+3]=BYTE(255-std::max({int(white[i])-black[i],int(white[i+1])-black[i+1],int(white[i+2])-black[i+2]}));
        D3D11_TEXTURE2D_DESC d{};d.Width=cursorWidth;d.Height=cursorHeight;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{black.data(),cursorWidth*4,0};
        cursorView.Reset();cursor.Reset();check_hresult(owner->device->CreateTexture2D(&d,&initial,&cursor));check_hresult(owner->device->CreateShaderResourceView(cursor.Get(),nullptr,&cursorView));lastCursor=handle;
    }
    void desktopFrame(ID3D11Texture2D* surface){
        D3D11_TEXTURE2D_DESC old{},current{};desktop->GetDesc(&old);surface->GetDesc(&current);
        if(old.Width!=current.Width||old.Height!=current.Height||old.Format!=current.Format){
            Ptr<ID3D11Texture2D> replacement;Ptr<ID3D11ShaderResourceView> view;
            current.BindFlags=D3D11_BIND_SHADER_RESOURCE;current.MiscFlags=0;current.Usage=D3D11_USAGE_DEFAULT;current.CPUAccessFlags=0;current.ArraySize=current.MipLevels=1;
            check_hresult(owner->device->CreateTexture2D(&current,nullptr,&replacement));check_hresult(owner->device->CreateShaderResourceView(replacement.Get(),nullptr,&view));desktop=replacement;desktopView=view;
        }
        context->CopyResource(desktop.Get(),surface);
    }
    ID3D11Texture2D* next(){
        if(window){
            if(window->closed())return nullptr;
            auto surface=window->next();if(!surface)return nullptr;
            D3D11_TEXTURE2D_DESC old{},current{};desktop->GetDesc(&old);surface->GetDesc(&current);
            if(old.Width!=current.Width||old.Height!=current.Height){desktopView.Reset();desktop=surface;check_hresult(owner->device->CreateShaderResourceView(desktop.Get(),nullptr,&desktopView));}
            else if(desktop.Get()!=surface)context->CopyResource(desktop.Get(),surface);
            region=monitor={0,0,LONG(current.Width),LONG(current.Height)};
        }else{
        capture->next();waiting=capture->waiting;
        if(waiting)return converted?(bgraOnly?scaled.Get():nv12.Get()):nullptr;
        monitor=capture->monitor;
        // Keep the encoder canvas fixed while the game changes display mode.
        if(wholeMonitor)region=monitor;
        else {region=originalRegion;OffsetRect(&region,monitor.left-originalMonitor.left,monitor.top-originalMonitor.top);RECT clipped{};if(!IntersectRect(&clipped,&region,&monitor))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_MONITOR_HANDLE));region=clipped;}
        if(generation!=capture->generation){desktopFrame(capture->texture.Get());generation=capture->generation;}
        }
        float values[16]{};auto mw=float(monitor.right-monitor.left),mh=float(monitor.bottom-monitor.top);
        values[0]=(region.left-monitor.left)/mw;values[1]=(region.top-monitor.top)/mh;values[2]=(region.right-region.left)/mw;values[3]=(region.bottom-region.top)/mh;
        values[4]=mw;values[5]=mh;values[6]=float(owner->width);values[7]=float(owner->height);values[12]=float(options.scaling);
        CURSORINFO pointer{sizeof(pointer)};
        if(!window&&options.cursor&&GetCursorInfo(&pointer)&&pointer.flags==CURSOR_SHOWING){if(lastCursor!=pointer.hCursor)cursorImage(pointer.hCursor);const auto sx=float(owner->width)/(region.right-region.left),sy=float(owner->height)/(region.bottom-region.top);values[8]=(pointer.ptScreenPos.x-region.left-hotspot.x)*sx;values[9]=(pointer.ptScreenPos.y-region.top-hotspot.y)*sy;values[10]=cursorWidth*sx;values[11]=cursorHeight*sy;values[13]=1;}
        context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);auto cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);auto t=target.Get();context->OMSetRenderTargets(1,&t,nullptr);
        D3D11_VIEWPORT viewport{0,0,float(owner->width),float(owner->height),0,1};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        ID3D11ShaderResourceView* textures[]{desktopView.Get(),cursorView.Get()};context->PSSetShaderResources(0,2,textures);ID3D11SamplerState* samplers[]{linear.Get(),point.Get()};context->PSSetSamplers(0,2,samplers);context->Draw(3,0);context->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView* empty[]{nullptr,nullptr};context->PSSetShaderResources(0,2,empty);
        if(bgraOnly){context->Flush();converted=true;return scaled.Get();}
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=input.Get();check_hresult(videoContext->VideoProcessorBlt(processor.Get(),output.Get(),0,1,&stream));context->Flush();converted=true;return nv12.Get();
    }
};
Pipeline::Pipeline(Options const& o,RECT r,HWND window,bool bgraOnly):impl_(std::make_unique<Impl>(this)){impl_->bgraOnly=bgraOnly;impl_->setup(o,r,window);}
Pipeline::~Pipeline()=default;
ID3D11Texture2D* Pipeline::next(){std::unique_lock<std::recursive_mutex> lock;if(impl_->capture)lock=std::unique_lock(impl_->capture->mutex);return impl_->next();}
bool Pipeline::sourceClosed()const{return impl_->window&&impl_->window->closed();}
bool Pipeline::captureWaiting()const{return impl_->waiting;}
uint64_t Pipeline::captureRecoveries()const{if(!impl_->capture)return 0;std::lock_guard lock(impl_->capture->mutex);return impl_->capture->recoveries;}
std::vector<BYTE> Pipeline::readback(){
    std::unique_lock<std::recursive_mutex> lock;if(impl_->capture)lock=std::unique_lock(impl_->capture->mutex);
    if(!impl_->staging){D3D11_TEXTURE2D_DESC d{};impl_->nv12->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;check_hresult(device->CreateTexture2D(&d,nullptr,&impl_->staging));}
    impl_->context->CopyResource(impl_->staging.Get(),impl_->nv12.Get());D3D11_MAPPED_SUBRESOURCE map{};check_hresult(impl_->context->Map(impl_->staging.Get(),0,D3D11_MAP_READ,0,&map));
    std::vector<BYTE> pixels;try{pixels.resize(size_t(width)*height*3/2);for(unsigned y=0;y<height+height/2;++y)memcpy(pixels.data()+size_t(y)*width,static_cast<BYTE*>(map.pData)+size_t(y)*map.RowPitch,width);}catch(...){impl_->context->Unmap(impl_->staging.Get(),0);throw;}impl_->context->Unmap(impl_->staging.Get(),0);return pixels;
}
bool cudaAvailable(){
    HMODULE dll=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)return false;
    using Init=int(__stdcall*)(unsigned);using Count=int(__stdcall*)(int*);auto init=reinterpret_cast<Init>(GetProcAddress(dll,"cuInit"));auto count=reinterpret_cast<Count>(GetProcAddress(dll,"cuDeviceGetCount"));int n{};bool ok=init&&count&&init(0)==0&&count(&n)==0&&n>0;FreeLibrary(dll);return ok;
}
std::vector<Packet> Encoder::encode12(ID3D12Resource*,LONGLONG,LONGLONG){winrt::throw_hresult(E_NOTIMPL);}
}
