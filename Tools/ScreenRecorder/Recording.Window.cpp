#include "Recording.Window.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <algorithm>
#pragma comment(lib,"windowsapp.lib")
namespace aelura::recording::gpu {
using namespace winrt;
namespace Capture=winrt::Windows::Graphics::Capture;
namespace Dx=winrt::Windows::Graphics::DirectX;
using SizeInt32=winrt::Windows::Graphics::SizeInt32;
struct WindowCapture::Impl {
    WindowCapture* owner; HWND hwnd{}; DWORD processId{};
    Capture::GraphicsCaptureItem item{nullptr};
    Capture::Direct3D11CaptureFramePool pool{nullptr};
    Capture::GraphicsCaptureSession session{nullptr};
    Dx::Direct3D11::IDirect3DDevice runtimeDevice{nullptr};
    Ptr<ID3D11Device> device; Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> cached; SizeInt32 poolSize{};
    std::shared_ptr<std::atomic<bool>> ended{std::make_shared<std::atomic<bool>>(false)};
    event_token closedToken{};
    explicit Impl(WindowCapture* p) : owner(p) {}
    void setup(HWND window, ID3D11Device* d, bool cursor) {
        if(!IsWindow(window) || !Capture::GraphicsCaptureSession::IsSupported()) throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        hwnd=window;device=d;device->GetImmediateContext(&context);GetWindowThreadProcessId(hwnd,&processId);
        auto interop=get_activation_factory<Capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(hwnd,guid_of<Capture::GraphicsCaptureItem>(),put_abi(item)));
        poolSize=item.Size();
        if(poolSize.Width<2||poolSize.Height<2||poolSize.Width>16384||poolSize.Height>16384)throw_hresult(E_INVALIDARG);
        owner->width=unsigned(poolSize.Width);owner->height=unsigned(poolSize.Height);
        Ptr<IDXGIDevice> dxgi;check_hresult(device.As(&dxgi));
        com_ptr<IInspectable> inspectable;check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()));
        runtimeDevice=inspectable.as<Dx::Direct3D11::IDirect3DDevice>();
        auto state=ended;closedToken=item.Closed([state](auto const&,auto const&){*state=true;});
        pool=Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice,Dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,poolSize);
        session=pool.CreateCaptureSession(item);session.IsCursorCaptureEnabled(cursor);session.StartCapture();
    }
    ~Impl() {
        try{if(item)item.Closed(closedToken);}catch(...){}
        try{if(session)session.Close();}catch(...){}
        try{if(pool)pool.Close();}catch(...){}
    }
    ID3D11Texture2D* next() {
        DWORD pid{};if(!IsWindow(hwnd)||!GetWindowThreadProcessId(hwnd,&pid)||pid!=processId){*ended=true;return nullptr;}
        if(IsIconic(hwnd))return cached.Get();
        auto frame=pool.TryGetNextFrame();if(!frame)return cached.Get();
        // Poll on the recording worker; no UI dispatcher or frame event retains
        // this object. Keep a private surface while WGC recycles its own pool.
        for(int n=0;n<3;++n){auto newer=pool.TryGetNextFrame();if(!newer)break;frame.Close();frame=newer;}
        auto size=frame.ContentSize();Ptr<ID3D11Texture2D> surface;
        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();check_hresult(access->GetInterface(IID_PPV_ARGS(&surface)));
        D3D11_TEXTURE2D_DESC desc{};surface->GetDesc(&desc);
        auto frameWidth=std::min(unsigned(std::max(0,size.Width)),desc.Width),frameHeight=std::min(unsigned(std::max(0,size.Height)),desc.Height);
        if(frameWidth>=2&&frameHeight>=2){
            if(!cached||frameWidth!=owner->width||frameHeight!=owner->height){cached.Reset();desc.Width=frameWidth;desc.Height=frameHeight;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.ArraySize=desc.MipLevels=1;check_hresult(device->CreateTexture2D(&desc,nullptr,&cached));}
            D3D11_BOX box{0,0,0,frameWidth,frameHeight,1};context->CopySubresourceRegion(cached.Get(),0,0,0,0,surface.Get(),0,&box);owner->width=frameWidth;owner->height=frameHeight;
        }
        frame.Close();
        if(size.Width>=2&&size.Height>=2&&size.Width<=16384&&size.Height<=16384&&(size.Width!=poolSize.Width||size.Height!=poolSize.Height)){
            pool.Recreate(runtimeDevice,Dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);poolSize=size;
        }
        return cached.Get();
    }
};
WindowCapture::WindowCapture(HWND hwnd,ID3D11Device* device,bool cursor):impl_(std::make_unique<Impl>(this)){impl_->setup(hwnd,device,cursor);}
WindowCapture::~WindowCapture()=default;
ID3D11Texture2D* WindowCapture::next(){return impl_->next();}
bool WindowCapture::closed()const{return *impl_->ended;}
}
