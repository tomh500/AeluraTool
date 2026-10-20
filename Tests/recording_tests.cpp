#include "../Tools/ScreenRecorder/Recording.Media.h"
#include "../Tools/ScreenRecorder/Recording.Dx12.h"
#include <winrt/base.h>
#include <mferror.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace aelura::recording;
namespace fs=std::filesystem;
void fullscreenTests(fs::path const&);
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
void memoryTests(fs::path const& root){
    fs::create_directory(root);
    {auto budget=std::make_shared<media::MemoryBudget>(4*65536);auto first=std::make_shared<media::MemoryFile>(budget);auto a=first->stream();std::vector<BYTE> bytes(2*65536,42);ULONG count{};
        winrt::check_hresult(a->Write(bytes.data(),ULONG(bytes.size()),&count));check(count==bytes.size()&&budget->used==2*65536,"memory quota accounting failed");
        auto second=std::make_shared<media::MemoryFile>(budget);auto b=second->stream();bytes.resize(3*65536,77);check(FAILED(b->Write(bytes.data(),ULONG(bytes.size()),&count))&&budget->used<=budget->limit,"memory quota exceeded");
        auto pinned=first;budget->reclaim=[&](uint64_t){a.Reset();first.reset();};check(FAILED(b->Write(bytes.data(),ULONG(bytes.size()),&count))&&budget->used<=budget->limit,"export snapshot escaped shared memory quota");pinned.reset();winrt::check_hresult(b->Write(bytes.data(),ULONG(bytes.size()),&count));check(budget->used==3*65536,"memory eviction did not reclaim payload");budget->reclaim={};
        LARGE_INTEGER zero{};winrt::check_hresult(b->Seek(zero,STREAM_SEEK_SET,nullptr));media::Ptr<IStream> clone;winrt::check_hresult(b->Clone(&clone));BYTE value{};winrt::check_hresult(clone->Read(&value,1,&count));check(value==42,"memory clone content differs");
        winrt::check_hresult(b->Read(&value,1,&count));check(value==42,"memory clone shared cursor");ULARGE_INTEGER length{};length.QuadPart=1;winrt::check_hresult(b->SetSize(length));length.QuadPart=100;winrt::check_hresult(b->SetSize(length));LARGE_INTEGER one{};one.QuadPart=1;winrt::check_hresult(b->Seek(one,STREAM_SEEK_SET,nullptr));winrt::check_hresult(b->Read(&value,1,&count));check(value==0,"memory stream exposed bytes after truncate/extend");
        second->seal();check(b->Write(&value,1,&count)==STG_E_ACCESSDENIED,"sealed memory clip remained writable");
    }
    check(fs::is_empty(root),"memory stream created files");
    for(auto codec:{Codec::H264,Codec::HEVC})for(auto mode:{Encoder::CPU,Encoder::GPU}){
        if(codec==Codec::HEVC&&mode==Encoder::CPU)continue;
        Options o;o.directory=root;o.encoder=mode;o.codec=codec;o.bitrate=1500000;auto budget=std::make_shared<media::MemoryBudget>(16*1048576);auto file=std::make_shared<media::MemoryFile>(budget);
        media::Microphone audio(false);audio.rate=48000;audio.channels=2;
        {media::Writer writer(o,320,240,audio,{},nullptr,nullptr,file.get());std::vector<BYTE> pixels(320*240*3/2,128);std::fill(pixels.begin(),pixels.begin()+320*240,BYTE(80));
            for(unsigned frame=0;frame<30;++frame){writer.frame(pixels,LONGLONG(frame)*10000000/30,10000000/30);writer.sound(std::vector<int16_t>(3200),LONGLONG(frame)*10000000/30);}writer.finish();}
        file->seal();check(file->size()>0&&budget->used<=budget->limit,"in-memory MP4 missing or over quota");
        auto output=root/(L"ram-"+std::to_wstring(int(codec))+L"-"+std::to_wstring(int(mode))+L".mp4");
        const auto before=std::distance(fs::directory_iterator(root),fs::directory_iterator());media::joinMemory({file,file},output);check(decode(output,true)==60,"memory MP4 remux lost video/audio");
        check(std::distance(fs::directory_iterator(root),fs::directory_iterator())==before+1,"memory encode created unexpected temporary files");
        bool canceled=false;try{media::joinMemory({file},root/L"cancel.mp4",[](double){return false;});}catch(winrt::hresult_error const& e){canceled=e.code()==E_ABORT;}check(canceled&&file->size()>0&&!fs::exists(root/L"cancel.mp4"),"memory cancellation lost source or left partial output");
        bool refused=false;try{media::joinMemory({file},output);}catch(...){refused=true;}check(refused&&decode(output)==60,"memory export overwrote existing video");
        file.reset();check(budget->used==0,"memory stream retained payload after writer, export and clips were released");
    }
    std::cout<<"PASS: bounded RAM streams, eviction, independent clones, sealed clips, CPU/GPU memory MP4, AAC, remux, cancel and no overwrite\n";
}
void checkBlueFrame(fs::path const& file){
    media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,&reader));
    media::Ptr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
    winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()));
    DWORD flags{};media::Ptr<IMFSample> sample;while(!sample&&!(flags&MF_SOURCE_READERF_ENDOFSTREAM))winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample));check(bool(sample),"missing decoded color frame");
    media::Ptr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));BYTE* data{};DWORD size{};winrt::check_hresult(buffer->Lock(&data,nullptr,&size));
    const bool correct=size>=320*240*3/2&&abs(int(data[100*320+100])-84)<7&&abs(int(data[320*240+50*320+100])-168)<7&&abs(int(data[320*240+50*320+101])-98)<7;
    buffer->Unlock();check(correct,"native DX12 decoded color/plane packing mismatch");
}
void native12Tests(fs::path const& directory){
    Options options;options.directory=directory;options.encoder=Encoder::GPU;options.performanceBeta=true;options.graphics=Graphics::Direct3D12;options.bitrate=1500000;options.scale=50;
    std::unique_ptr<gpu::Dx12Pipeline> pipeline;
    try{pipeline=std::make_unique<gpu::Dx12Pipeline>(options,RECT{0,0,638,478});}
    catch(winrt::hresult_error const& e){if(e.code()==DXGI_ERROR_UNSUPPORTED){std::cout<<"LIMIT: native DX12 requires a compatible NVIDIA adapter\n";return;}throw;}
    std::vector<BYTE> source(638*478*4);for(size_t n=0;n<source.size();n+=4){source[n]=160;source[n+1]=88;source[n+2]=32;source[n+3]=255;}
    for(auto filter:{Scaling::Nearest,Scaling::Linear,Scaling::Cubic,Scaling::Fant})for(auto codec:{Codec::H264,Codec::HEVC}){
        options.scaling=filter;pipeline=std::make_unique<gpu::Dx12Pipeline>(options,RECT{0,0,638,478});
        options.codec=codec;auto file=directory/(L"native12-"+std::to_wstring(int(codec))+L"-"+std::to_wstring(int(filter))+L".mp4");media::Microphone mic(false);
        std::cout<<"CHECK: native D3D12 compute and NVENC "<<int(codec)<<" scaling "<<int(filter)<<std::endl;
        media::Writer writer(options,320,240,mic,file,nullptr,gpu::nvidia12(options,pipeline->device.Get(),320,240));
        for(unsigned frame=0;frame<12;++frame)writer.texture12(pipeline->process(source,638*4),LONGLONG(frame)*10000000/30,10000000/30);
        writer.finish();check(decode(file)==12,"native DX12 encoded video did not decode to all frames");checkBlueFrame(file);std::cout<<"PASS: native D3D12 compute / NVENC / decoded frames and NV12 colors "<<int(codec)<<"\n";
    }
}
LRESULT CALLBACK ownedWindow(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);auto brush=CreateSolidBrush(COLORREF(GetWindowLongPtrW(hwnd,GWLP_USERDATA)));FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(hwnd,&ps);return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
void windowSessionTests(fs::path const& directory){
    WNDCLASSW wc{};wc.lpfnWndProc=ownedWindow;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"AeluraOwnedSessionTest";check(RegisterClassW(&wc)!=0,"owned test class failed");
    struct Windows{HWND target{},cover{};~Windows(){if(target)DestroyWindow(target);if(cover)DestroyWindow(cover);}} windows;
    auto create=[&](wchar_t const* title,COLORREF color){auto hwnd=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,title,WS_OVERLAPPEDWINDOW,100,150,400,300,nullptr,nullptr,wc.hInstance,nullptr);check(hwnd!=nullptr,"owned window failed");SetWindowLongPtrW(hwnd,GWLP_USERDATA,color);ShowWindow(hwnd,SW_SHOWNORMAL);UpdateWindow(hwnd);return hwnd;};
    windows.target=create(L"Aelura generated blue target",RGB(32,88,160));
    auto pump=[&](auto condition){auto deadline=GetTickCount64()+15000;while(!condition()){check(GetTickCount64()<deadline,"owned session timeout");MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);}};
    Options o;o.directory=directory;o.fps=15;o.bitrate=1500000;o.cursor=false;o.encoder=Encoder::GPU;o.performanceBeta=true;
    RECT bounds{};GetWindowRect(windows.target,&bounds);
    {gpu::Pipeline pipeline(o,bounds,windows.target);pump([&]{return pipeline.next()!=nullptr;});
        windows.cover=create(L"Aelura generated red cover",RGB(240,20,20));
        auto assertBlue=[&]{auto until=GetTickCount64()+400;pump([&]{pipeline.next();return GetTickCount64()>=until;});auto pixels=pipeline.readback();auto y=pixels[size_t(pipeline.height/2)*pipeline.width+pipeline.width/2];check(y>77&&y<92,"covered window capture included another window");};
        assertBlue();SetWindowPos(windows.target,HWND_BOTTOM,600,200,520,360,SWP_NOACTIVATE);assertBlue();ShowWindow(windows.target,SW_MINIMIZE);assertBlue();ShowWindow(windows.target,SW_SHOWNOACTIVATE);
    }
    std::cout<<"PASS: owned WGC target remains blue when covered, moved, resized and minimized\n";
    // Replay's desktop region is entirely inside our own opaque topmost cover.
    SetWindowPos(windows.cover,HWND_TOPMOST,100,150,400,300,SWP_SHOWWINDOW);UpdateWindow(windows.cover);RECT client{30,40,350,230};MapWindowPoints(windows.cover,nullptr,reinterpret_cast<POINT*>(&client),2);
    Session manual,replay;GetWindowRect(windows.target,&bounds);check(manual.start(o,bounds,false,windows.target),"window session rejected");check(replay.start(o,client,true),"parallel replay rejected");
    pump([&]{winrt::check_hresult(manual.snapshot().error);winrt::check_hresult(replay.snapshot().error);return manual.snapshot().frames>=15&&replay.snapshot().frames>=15;});
    auto cache=replay.snapshot().recovery;check(replay.saveReplay(),"concurrent replay save rejected");pump([&]{winrt::check_hresult(replay.snapshot().error);return replay.snapshot().savedGeneration>0;});check(decode(replay.snapshot().output)>0,"parallel replay output corrupt");
    const auto before=replay.snapshot().frames;DestroyWindow(windows.target);windows.target=nullptr;pump([&]{return !manual.busy();});winrt::check_hresult(manual.snapshot().error);check(decode(manual.snapshot().output)>0,"closed-window recording corrupt");
    pump([&]{winrt::check_hresult(replay.snapshot().error);return replay.snapshot().frames>before+3;});replay.stop(true);pump([&]{return !replay.busy();});manual.wait();replay.wait();winrt::check_hresult(replay.snapshot().error);check(!fs::exists(cache),"replay stop did not remove owned rolling cache");
    std::cout<<"PASS: concurrent window recording / replay save / source close / replay continues / exit cache cleanup\n";
    o.graphics=Graphics::Direct3D12;
    Session area12,replay12;check(area12.start(o,client,false)&&replay12.start(o,client,true),"native12 sessions rejected");
    pump([&]{winrt::check_hresult(area12.snapshot().error);winrt::check_hresult(replay12.snapshot().error);return area12.snapshot().frames>10&&replay12.snapshot().frames>10;});
    check(area12.snapshot().nativeDx12&&replay12.snapshot().nativeDx12,"native12 session silently changed backend");
    check(replay12.saveReplay(),"native12 replay save rejected");pump([&]{winrt::check_hresult(replay12.snapshot().error);return replay12.snapshot().savedGeneration>0;});
    auto cache12=replay12.snapshot().recovery;area12.stop();replay12.stop(true);pump([&]{return !area12.busy()&&!replay12.busy();});area12.wait();replay12.wait();winrt::check_hresult(area12.snapshot().error);winrt::check_hresult(replay12.snapshot().error);
    check(decode(area12.snapshot().output)>0&&decode(replay12.snapshot().output)>0&&!fs::exists(cache12),"native12 parallel output or cleanup failed");
    std::cout<<"PASS: actual native12 area / parallel replay / export / decoded videos / cleanup\n";
    o.replayStorage=ReplayStorage::Memory;o.memoryLimitMB=128;o.directory=directory/L"memory-saved";Session ram;
    check(ram.start(o,client,true),"RAM replay start rejected");pump([&]{winrt::check_hresult(ram.snapshot().error);return ram.snapshot().frames>15;});
    check(ram.snapshot().memoryReplay&&ram.snapshot().cacheBytes>0&&ram.snapshot().cacheBytes<=128*1048576ULL&&ram.snapshot().recovery.empty()&&!fs::exists(o.directory),"RAM replay wrote a directory or failed quota/status checks");
    check(ram.saveReplay(),"RAM replay save rejected");pump([&]{winrt::check_hresult(ram.snapshot().error);return ram.snapshot().savedGeneration>0;});check(decode(ram.snapshot().output)>0,"RAM session output corrupt");
    auto frames=ram.snapshot().frames;pump([&]{winrt::check_hresult(ram.snapshot().error);return ram.snapshot().frames>frames+5;});ram.stop(true);pump([&]{return !ram.busy();});ram.wait();winrt::check_hresult(ram.snapshot().error);
    check(ram.snapshot().cacheBytes==0&&ram.snapshot().recovery.empty()&&std::distance(fs::directory_iterator(o.directory),fs::directory_iterator())==1,"RAM stop leaked clips or temporary disk files");
    std::cout<<"PASS: native12 RAM replay buffers without output/cache directory, exports, continues and releases memory\n";
}
LRESULT CALLBACK patternWindow(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT r{};GetClientRect(hwnd,&r);auto brush=CreateSolidBrush(RGB(32,88,160));FillRect(dc,&r,brush);DeleteObject(brush);SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,L"Generated recording test pattern",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);EndPaint(hwnd,&ps);return 0;}
    if(message==WM_TIMER){DestroyWindow(hwnd);return 0;}if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,message,wp,lp);
}
int wmain(int argc,wchar_t** argv){
    std::cout<<std::unitbuf;
    fs::path root=fs::temp_directory_path()/(L"AeluraRecordingTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        media::Runtime runtime;
        if(argc==2&&std::wstring(argv[1])==L"--fullscreen-only"){fs::create_directory(root);fullscreenTests(root);fs::remove_all(root);return 0;}
        if(argc==2&&std::wstring(argv[1])==L"--native12-only"){fs::create_directory(root);native12Tests(root);fs::remove_all(root);return 0;}
        if(argc==2&&std::wstring(argv[1])==L"--memory-only"){fs::create_directory(root);memoryTests(root/L"memory");fs::remove_all(root);return 0;}
        if(argc==2&&std::wstring(argv[1])==L"--window-session-only"){fs::create_directory(root);windowSessionTests(root);fs::remove_all(root);return 0;}
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
        check(outputExtent(2560,100)==2560&&outputExtent(2559,100)==2560&&outputExtent(1439,100)==1440&&outputExtent(2560,75)==1920,"NV12 edge alignment lost source pixels");
        check(replayFirst({5,5,5},10)==1&&replayFirst({5,5},300)==0&&replayFirst({},300)==0,"replay retention failed");std::vector<double> longRun(1000,5);check(replayFirst(longRun,300)==940,"five minute retention failed");
        o.performanceBeta=true;o.encoder=Encoder::GPU;o.replayEnabled=true;save(root/L"settings.json",o);auto loaded=load(root/L"settings.json");check(loaded.directory==o.directory&&loaded.performanceBeta&&loaded.encoder==Encoder::GPU&&loaded.replayEnabled,"settings roundtrip and persistent replay intent failed");
        {std::ifstream input(root/L"settings.json");std::string legacy((std::istreambuf_iterator<char>(input)),{});for(auto key:{"\"replayEnabled\"","\"replayStorage\"","\"memoryLimitMB\""}){auto position=legacy.find(key);auto end=legacy.find(',',position);legacy.erase(position,end-position+1);}std::ofstream output(root/L"legacy.json");output<<legacy;}auto legacy=load(root/L"legacy.json");check(!legacy.replayEnabled&&legacy.directory==o.directory&&legacy.replayStorage==ReplayStorage::Disk&&legacy.memoryLimitMB==1024,"legacy replay cache defaults failed");
        {auto memory=o;memory.replayStorage=ReplayStorage::Memory;memory.memoryLimitMB=128;save(root/L"memory-settings.json",memory);auto read=load(root/L"memory-settings.json");check(read.replayStorage==ReplayStorage::Memory&&read.memoryLimitMB==128,"memory settings persistence failed");memory.memoryLimitMB=127;check(rejects(memory,{0,0,320,240}),"unsafe memory limit accepted");}
        {auto disabled=o;disabled.replayEnabled=false;disabled.graphics=Graphics::Direct3D12;save(root/L"disabled.json",disabled);auto reloaded=load(root/L"disabled.json");check(!reloaded.replayEnabled&&reloaded.graphics==Graphics::Direct3D12,"disabled replay/native graphics persistence failed");}
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
        native12Tests(root);memoryTests(root/L"memory");fs::remove_all(root);std::cout<<"PASS: recording tests completed\n";return 0;
    }catch(winrt::hresult_error const& e){std::wcerr<<L"FAIL: 0x"<<std::hex<<unsigned(e.code())<<L" "<<e.message().c_str()<<L" sandbox "<<root<<L"\n";return 1;}
    catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<" sandbox "<<root<<"\n";return 1;}
}
