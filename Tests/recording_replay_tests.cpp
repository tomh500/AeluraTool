#include "../Tools/ScreenRecorder/Recording.Media.h"
#include <winrt/base.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace aelura::recording;
namespace {
void require(bool value,char const* message){if(!value)throw std::runtime_error(message);}
LRESULT CALLBACK paint(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){if(msg==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT rect{};GetClientRect(hwnd,&rect);auto brush=CreateSolidBrush(RGB(32,88,160));FillRect(dc,&rect,brush);DeleteObject(brush);EndPaint(hwnd,&ps);return 0;}return DefWindowProcW(hwnd,msg,wp,lp);}
uint64_t countFrames(std::filesystem::path const& file){media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,&reader));uint64_t count=0;for(;;){DWORD flags{};media::Ptr<IMFSample> sample;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample));if(sample)++count;if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}return count;}
}
void replayConsumeTests(std::filesystem::path const& root){
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=paint;wc.lpszClassName=L"AeluraReplayConsumeTest";RegisterClassW(&wc);
    struct Window {HWND hwnd{};~Window(){if(hwnd)DestroyWindow(hwnd);}} window;
    window.hwnd=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Aelura generated replay test",WS_POPUP,120,160,320,240,nullptr,nullptr,wc.hInstance,nullptr);winrt::check_bool(window.hwnd!=nullptr);ShowWindow(window.hwnd,SW_SHOW);UpdateWindow(window.hwnd);RECT region{};GetWindowRect(window.hwnd,&region);
    for(auto storage:{ReplayStorage::Disk,ReplayStorage::Memory}){
        Options o;o.systemAudio=false;o.microphone=false;o.directory=root/(storage==ReplayStorage::Disk?L"disk":L"ram");o.encoder=Encoder::GPU;o.performanceBeta=true;o.graphics=Graphics::Direct3D12;o.replayStorage=storage;o.memoryLimitMB=128;o.fps=15;o.bitrate=1500000;o.cursor=false;
        Session session;require(session.start(o,region,true),"replay consumption session rejected");
        auto wait=[&](auto condition){auto deadline=GetTickCount64()+20000;while(!condition()){auto s=session.snapshot();if(s.state==State::Failed)winrt::check_hresult(s.error);require(GetTickCount64()<deadline,"replay consumption timeout");MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}};
        wait([&]{return session.snapshot().bufferedSeconds>=6;});auto before=session.snapshot();require(before.nativeDx12&&!before.dx12Fallback,"replay consumption test did not exercise DX12");
        if(storage==ReplayStorage::Memory){
            {std::ofstream blocker(o.directory);blocker<<"Generated test blocker";}
            require(session.saveReplay(),"failure-path replay request rejected");wait([&]{return !session.snapshot().saving;});require(FAILED(session.snapshot().error)&&session.snapshot().bufferedSeconds>=6&&session.snapshot().cacheBytes>0,"failed export consumed unsaved RAM replay");std::filesystem::remove(o.directory);
            std::cout<<"PASS: failed replay export retains unsaved history and permits retry\n";
        }
        std::vector<std::filesystem::path> oldFiles;if(storage==ReplayStorage::Disk)for(auto const& item:std::filesystem::directory_iterator(before.recovery))if(item.path().extension()==L".mp4")oldFiles.push_back(item.path());
        require(session.saveReplay(),"first replay save rejected");wait([&]{return session.snapshot().savedGeneration==1;});auto first=session.snapshot().output;auto firstFrames=countFrames(first);require(firstFrames>=85,"initial replay output missing history");wait([&]{return session.snapshot().bufferedSeconds<1;});
        if(storage==ReplayStorage::Disk){for(auto const& file:oldFiles)require(!std::filesystem::exists(file),"saved disk replay fragment was not released");}
        else require(session.snapshot().cacheBytes<before.cacheBytes,"saved RAM payload was not released");
        wait([&]{return session.snapshot().bufferedSeconds>=2;});require(session.saveReplay(),"second replay save rejected");wait([&]{return session.snapshot().savedGeneration==2;});auto second=session.snapshot().output;auto secondFrames=countFrames(second);require(second!=first&&secondFrames>15&&secondFrames<firstFrames/2,"second replay duplicated previously saved history");
        auto cache=session.snapshot().recovery;session.stop(true);wait([&]{return !session.busy();});session.wait();winrt::check_hresult(session.snapshot().error);require(std::filesystem::exists(first)&&std::filesystem::exists(second),"temporary cleanup removed saved videos");if(!cache.empty())require(!std::filesystem::exists(cache),"replay cache cleanup left owned files");
        std::cout<<"PASS: storage="<<int(storage)<<" consumes only committed snapshot, releases old cache, second export="<<secondFrames<<" vs first="<<firstFrames<<" frames; saved outputs survive cleanup\n";
    }
}
