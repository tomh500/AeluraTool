#include "Recording.Desktop.h"
#include <d3d11_4.h>
#include <map>
#include <winrt/base.h>
namespace aelura::recording::gpu {
using winrt::check_hresult;
namespace {
bool temporary(HRESULT hr){return hr==DXGI_ERROR_ACCESS_LOST||hr==DXGI_ERROR_NOT_CURRENTLY_AVAILABLE||hr==DXGI_ERROR_SESSION_DISCONNECTED||hr==DXGI_STATUS_MODE_CHANGE_IN_PROGRESS||hr==E_ACCESSDENIED;}
}
DesktopCapture::DesktopCapture(ID3D11Device* d,DXGI_OUTPUT_DESC const& desc):device(d),monitor(desc.DesktopCoordinates),displayName(desc.DeviceName){
    device->GetImmediateContext(&context);Ptr<ID3D11Multithread> protection;check_hresult(context.As(&protection));protection->SetMultithreadProtected(TRUE);
}
std::shared_ptr<DesktopCapture> DesktopCapture::share(ID3D11Device* device,IDXGIOutput1* output){
    static std::mutex registryMutex;static std::map<std::wstring,std::weak_ptr<DesktopCapture>> registry;
    DXGI_OUTPUT_DESC desc{};check_hresult(output->GetDesc(&desc));std::lock_guard lock(registryMutex);
    if(auto shared=registry[desc.DeviceName].lock())return shared;
    auto shared=std::shared_ptr<DesktopCapture>(new DesktopCapture(device,desc));registry[desc.DeviceName]=shared;return shared;
}
void DesktopCapture::lost(){duplication.Reset();waiting=true;retryAfter=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);}
bool DesktopCapture::reconnect(){
    if(std::chrono::steady_clock::now()<retryAfter)return false;check_hresult(device->GetDeviceRemovedReason());
    Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> original;check_hresult(device.As(&dxgi));check_hresult(dxgi->GetAdapter(&original));DXGI_ADAPTER_DESC originalDesc{};check_hresult(original->GetDesc(&originalDesc));
    Ptr<IDXGIFactory1> factory;check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Ptr<IDXGIAdapter1> adapter;
    for(UINT n=0;;++n){Ptr<IDXGIAdapter1> item;auto hr=factory->EnumAdapters1(n,&item);if(hr==DXGI_ERROR_NOT_FOUND)break;check_hresult(hr);DXGI_ADAPTER_DESC1 desc{};check_hresult(item->GetDesc1(&desc));if(desc.AdapterLuid.HighPart==originalDesc.AdapterLuid.HighPart&&desc.AdapterLuid.LowPart==originalDesc.AdapterLuid.LowPart){adapter=item;break;}}
    if(!adapter){lost();return false;}
    Ptr<IDXGIOutput1> candidate;DXGI_OUTPUT_DESC desc{};
    for(UINT n=0;;++n){Ptr<IDXGIOutput> output;auto hr=adapter->EnumOutputs(n,&output);if(hr==DXGI_ERROR_NOT_FOUND)break;check_hresult(hr);check_hresult(output->GetDesc(&desc));if(desc.AttachedToDesktop&&displayName==desc.DeviceName){check_hresult(output.As(&candidate));break;}}
    if(!candidate){lost();return false;}if(desc.Rotation!=DXGI_MODE_ROTATION_IDENTITY)winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
    auto hr=candidate->DuplicateOutput(device.Get(),&duplication);if(temporary(hr)){lost();return false;}check_hresult(hr);monitor=desc.DesktopCoordinates;return true;
}
void DesktopCapture::next(){
    std::lock_guard lock(mutex);if(!duplication&&!reconnect())return;
    DXGI_OUTDUPL_FRAME_INFO info{};Ptr<IDXGIResource> resource;auto hr=duplication->AcquireNextFrame(texture?0:20,&info,&resource);
    if(temporary(hr)){if(hr==DXGI_ERROR_ACCESS_LOST)++recoveries;lost();return;}if(hr==DXGI_ERROR_WAIT_TIMEOUT)return;check_hresult(hr);
    try{
        Ptr<ID3D11Texture2D> surface;check_hresult(resource.As(&surface));D3D11_TEXTURE2D_DESC current{},old{};surface->GetDesc(&current);if(texture)texture->GetDesc(&old);
        if(!texture||old.Width!=current.Width||old.Height!=current.Height||old.Format!=current.Format){
            current.BindFlags=D3D11_BIND_SHADER_RESOURCE;current.MiscFlags=0;current.Usage=D3D11_USAGE_DEFAULT;current.CPUAccessFlags=0;current.ArraySize=current.MipLevels=1;
            Ptr<ID3D11Texture2D> replacement;check_hresult(device->CreateTexture2D(&current,nullptr,&replacement));texture=replacement;
        }
        context->CopyResource(texture.Get(),surface.Get());
    }catch(...){duplication->ReleaseFrame();throw;}
    hr=duplication->ReleaseFrame();if(temporary(hr)){if(hr==DXGI_ERROR_ACCESS_LOST)++recoveries;lost();return;}check_hresult(hr);waiting=false;++generation;
}
}
