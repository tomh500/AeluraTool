#include "Recording.Dx12.h"
#include "Recording.Gpu.h"
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"gdi32.lib")
namespace aelura::recording::gpu {
using winrt::check_hresult;
std::recursive_mutex& dx12LifetimeMutex(){static std::recursive_mutex mutex;return mutex;}
template<class T>using Com=Microsoft::WRL::ComPtr<T>;
namespace {
constexpr char compute[]=R"(
Texture2D<float4> Source:register(t0);RWByteAddressBuffer Output:register(u0);
cbuffer Params:register(b0){uint SW,SH,W,H,Pitch,UV,Mode,Unused;};
float3 pixel(int2 p){p=clamp(p,int2(0,0),int2(SW-1,SH-1));return Source.Load(int3(p,0)).rgb*255;}
float cubic(float v){v=abs(v);if(v<1)return 1.5*v*v*v-2.5*v*v+1;if(v<2)return -.5*v*v*v+2.5*v*v-4*v+2;return 0;}
float3 sampleLinear(float2 p){int2 a=int2(floor(p));float2 f=frac(p);return lerp(lerp(pixel(a),pixel(a+int2(1,0)),f.x),lerp(pixel(a+int2(0,1)),pixel(a+1),f.x),f.y);}
float3 sampleRGB(uint2 q){float2 p=(float2(q)+.5)*float2(SW,SH)/float2(W,H)-.5;
 if(Mode==0)return pixel(int2(floor(p+.5)));if(Mode==1)return sampleLinear(p);
 if(Mode==2){int2 a=int2(floor(p));float2 f=frac(p);float3 c=0;float total=0;[loop]for(int y=-1;y<=2;++y)[loop]for(int x=-1;x<=2;++x){float weight=cubic(x-f.x)*cubic(y-f.y);c+=pixel(a+int2(x,y))*weight;total+=weight;}return saturate(c/total/255)*255;}
 float3 c=0;[loop]for(int y=0;y<4;++y)[loop]for(int x=0;x<4;++x)c+=sampleLinear(p+(float2(x,y)/3-.5)*float2(SW,SH)/float2(W,H))/16;return c;
}
uint byte(float v){return (uint)clamp(floor(v+.5),0,255);}
[numthreads(16,8,1)]void main(uint3 tid:SV_DispatchThreadID){uint x=tid.x*4,y=tid.y;if(x>=W||y>=H)return;
 uint Y=0;[unroll]for(uint i=0;i<4;++i){float3 rgb=sampleRGB(uint2(min(x+i,W-1),y));Y|=byte(dot(rgb,float3(66,129,25))/256+16)<<(8*i);}Output.Store(y*Pitch+x,Y);
 if((y&1)==0){uint value=0;[unroll]for(uint block=0;block<2;++block){float3 rgb=0;[unroll]for(uint yy=0;yy<2;++yy)[unroll]for(uint xx=0;xx<2;++xx)rgb+=sampleRGB(uint2(min(x+block*2+xx,W-1),min(y+yy,H-1)))/4;
 value|=byte(dot(rgb,float3(-38,-74,112))/256+128)<<(16*block);value|=byte(dot(rgb,float3(112,-94,-18))/256+128)<<(16*block+8);}Output.Store(UV+(y/2)*Pitch+x,value);}
})";
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;return h;}
D3D12_RESOURCE_DESC buffer(UINT64 bytes,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
D3D12_RESOURCE_DESC texture(unsigned w,unsigned h,DXGI_FORMAT format){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;return d;}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);}
}
struct Dx12Pipeline::Impl {
    Dx12Pipeline* owner;Options options;RECT region{};unsigned sourceWidth{},sourceHeight{};
    Com<ID3D12CommandQueue> queue;Com<ID3D12CommandAllocator> allocator;Com<ID3D12GraphicsCommandList> commands;
    Com<ID3D12RootSignature> root;Com<ID3D12PipelineState> pipeline;Com<ID3D12DescriptorHeap> descriptors;
    Com<ID3D12Resource> upload,source,packed,nv12;Com<ID3D12Fence> fence;HANDLE completion{};UINT64 sequence{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sourceLayout{},planes[2]{};UINT64 sourceBytes{},outputBytes{};
    std::unique_ptr<Pipeline> capture;
    Com<ID3D12Resource> shared;Com<ID3D11Texture2D> shared11;
    Com<ID3D12Fence> sharedFence;Com<ID3D11Fence> sharedFence11;Com<ID3D11DeviceContext4> copyContext;
    UINT64 copySequence{};

    explicit Impl(Dx12Pipeline* p):owner(p){}
    ~Impl(){
        // Drain the queue even if the last frame threw after command submission.
        // Resources and the completion event must outlive all submitted work.
        if(queue&&fence&&completion&&SUCCEEDED(queue->Signal(fence.Get(),++sequence))&&fence->GetCompletedValue()<sequence&&SUCCEEDED(fence->SetEventOnCompletion(sequence,completion)))WaitForSingleObject(completion,5000);
        if(completion)CloseHandle(completion);
    }
    void setup(Options const& o,RECT r,HWND window){
        options=o;region=r;sourceWidth=unsigned(r.right-r.left);sourceHeight=unsigned(r.bottom-r.top);owner->width=outputExtent(sourceWidth,o.scale);owner->height=outputExtent(sourceHeight,o.scale);
        auto captureOptions=o;captureOptions.scale=100;capture=std::make_unique<Pipeline>(captureOptions,r,window,true);
        if(capture->vendor!=0x10de)winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
        if(window){owner->width=outputExtent(capture->width,o.scale);owner->height=outputExtent(capture->height,o.scale);}
        sourceWidth=capture->width;sourceHeight=capture->height;
        Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> original;check_hresult(capture->device.As(&dxgi));check_hresult(dxgi->GetAdapter(&original));DXGI_ADAPTER_DESC adapterDesc{};check_hresult(original->GetDesc(&adapterDesc));
        Com<IDXGIFactory4> factory;check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Com<IDXGIAdapter1> chosen;
        for(UINT n=0;;++n){Com<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(n,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};check_hresult(candidate->GetDesc1(&d));if(d.AdapterLuid.HighPart==adapterDesc.AdapterLuid.HighPart&&d.AdapterLuid.LowPart==adapterDesc.AdapterLuid.LowPart&&d.VendorId==0x10de&&SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&owner->device)))){chosen=candidate;owner->vendor=d.VendorId;owner->adapter=d.Description;break;}}
        if(!chosen)winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check_hresult(owner->device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        check_hresult(owner->device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));check_hresult(owner->device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));check_hresult(commands->Close());
        check_hresult(owner->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));completion=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completion)winrt::throw_last_error();
        auto gpuHeap=heap(D3D12_HEAP_TYPE_DEFAULT),cpuHeap=heap(D3D12_HEAP_TYPE_UPLOAD);
        auto src=texture(sourceWidth,sourceHeight,DXGI_FORMAT_B8G8R8A8_UNORM);check_hresult(owner->device->CreateCommittedResource(&gpuHeap,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&source)));
        owner->device->GetCopyableFootprints(&src,0,1,0,&sourceLayout,nullptr,nullptr,&sourceBytes);auto up=buffer(sourceBytes);check_hresult(owner->device->CreateCommittedResource(&cpuHeap,D3D12_HEAP_FLAG_NONE,&up,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        auto dest=texture(owner->width,owner->height,DXGI_FORMAT_NV12);check_hresult(owner->device->CreateCommittedResource(&gpuHeap,D3D12_HEAP_FLAG_NONE,&dest,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&nv12)));
        owner->device->GetCopyableFootprints(&dest,0,2,0,planes,nullptr,nullptr,&outputBytes);outputBytes=(outputBytes+255)&~255ULL;auto work=buffer(outputBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);check_hresult(owner->device->CreateCommittedResource(&gpuHeap,D3D12_HEAP_FLAG_NONE,&work,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&packed)));
        D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.NumDescriptors=2;dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;dh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;check_hresult(owner->device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&descriptors)));
        auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=src.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;owner->device->CreateShaderResourceView(source.Get(),&srv,handle);
        handle.ptr+=owner->device->GetDescriptorHandleIncrementSize(dh.Type);D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=UINT(outputBytes/4);uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;owner->device->CreateUnorderedAccessView(packed.Get(),nullptr,&uav,handle);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={2,ranges};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,8};D3D12_ROOT_SIGNATURE_DESC signature{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Com<ID3DBlob> blob,error;check_hresult(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));check_hresult(owner->device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        check_hresult(D3DCompile(compute,sizeof(compute),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));D3D12_COMPUTE_PIPELINE_STATE_DESC state{};state.pRootSignature=root.Get();state.CS={blob->GetBufferPointer(),blob->GetBufferSize()};check_hresult(owner->device->CreateComputePipelineState(&state,IID_PPV_ARGS(&pipeline)));bridge();
    }
    ID3D12Resource* process(std::span<BYTE const> bgra,unsigned stride,ID3D12Resource* gpuInput=nullptr){
        if(!gpuInput&&(stride<sourceWidth*4||bgra.size()<size_t(stride)*sourceHeight))winrt::throw_hresult(E_INVALIDARG);
        if(!gpuInput){BYTE* mapped{};D3D12_RANGE none{0,0};check_hresult(upload->Map(0,&none,reinterpret_cast<void**>(&mapped)));for(unsigned y=0;y<sourceHeight;++y)memcpy(mapped+sourceLayout.Offset+size_t(y)*sourceLayout.Footprint.RowPitch,bgra.data()+size_t(y)*stride,size_t(sourceWidth)*4);upload->Unmap(0,nullptr);}
        check_hresult(allocator->Reset());check_hresult(commands->Reset(allocator.Get(),pipeline.Get()));
        D3D12_TEXTURE_COPY_LOCATION from{},to{};
        if(gpuInput){transition(commands.Get(),gpuInput,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        else {
            transition(commands.Get(),source.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=sourceLayout;to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);transition(commands.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        owner->device->CreateShaderResourceView(gpuInput?gpuInput:source.Get(),&srv,descriptors->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[]{descriptors.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(root.Get());commands->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());UINT constants[]{sourceWidth,sourceHeight,owner->width,owner->height,planes[0].Footprint.RowPitch,UINT(planes[1].Offset),UINT(options.scaling),0};commands->SetComputeRoot32BitConstants(1,8,constants,0);commands->Dispatch((owner->width+63)/64,(owner->height+7)/8,1);
        transition(commands.Get(),packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(commands.Get(),nv12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT plane=0;plane<2;++plane){from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.pResource=packed.Get();from.PlacedFootprint=planes[plane];to.pResource=nv12.Get();to.SubresourceIndex=plane;commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
        transition(commands.Get(),nv12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);transition(commands.Get(),packed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);if(gpuInput)transition(commands.Get(),gpuInput,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);check_hresult(commands->Close());ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);check_hresult(queue->Signal(fence.Get(),++sequence));
        if(fence->GetCompletedValue()<sequence){check_hresult(fence->SetEventOnCompletion(sequence,completion));if(WaitForSingleObject(completion,5000)!=WAIT_OBJECT_0)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));}
        check_hresult(owner->device->GetDeviceRemovedReason());return nv12.Get();
    }
    void bridge(){
        if(!shared){
            auto h=heap(D3D12_HEAP_TYPE_DEFAULT);auto d=texture(sourceWidth,sourceHeight,DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            check_hresult(owner->device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_SHARED,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&shared)));
            Com<ID3D11Device5> device11;check_hresult(capture->device.As(&device11));Com<ID3D11DeviceContext> context;capture->device->GetImmediateContext(&context);check_hresult(context.As(&copyContext));
            HANDLE handle{};check_hresult(owner->device->CreateSharedHandle(shared.Get(),nullptr,GENERIC_ALL,nullptr,&handle));auto hr=device11->OpenSharedResource1(handle,IID_PPV_ARGS(&shared11));CloseHandle(handle);if(FAILED(hr))throw winrt::hresult_error(hr,L"DX11 opening shared DX12 texture");
            check_hresult(owner->device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&sharedFence)));check_hresult(owner->device->CreateSharedHandle(sharedFence.Get(),nullptr,GENERIC_ALL,nullptr,&handle));hr=device11->OpenSharedFence(handle,IID_PPV_ARGS(&sharedFence11));CloseHandle(handle);if(FAILED(hr))throw winrt::hresult_error(hr,L"DX11 opening shared DX12 fence");
        }
    }
    ID3D12Resource* next(){
        auto frame=capture->next();if(!frame)return nullptr;
        copyContext->CopyResource(shared11.Get(),frame);check_hresult(copyContext->Signal(sharedFence11.Get(),++copySequence));copyContext->Flush();check_hresult(queue->Wait(sharedFence.Get(),copySequence));
        return process({},0,shared.Get());
    }

};
Dx12Pipeline::Dx12Pipeline(Options const& o,RECT r,HWND window):impl_(std::make_unique<Impl>(this)){std::lock_guard lock(dx12LifetimeMutex());validate(o,r);impl_->setup(o,r,window);}
Dx12Pipeline::~Dx12Pipeline(){std::lock_guard lock(dx12LifetimeMutex());impl_.reset();device.Reset();}
ID3D12Resource* Dx12Pipeline::next(){return impl_->next();}
bool Dx12Pipeline::captureWaiting()const{return impl_->capture->captureWaiting();}
bool Dx12Pipeline::sourceClosed()const{return impl_->capture->sourceClosed();}
ID3D12Resource* Dx12Pipeline::process(std::span<BYTE const> data,unsigned stride){return impl_->process(data,stride);}
}
