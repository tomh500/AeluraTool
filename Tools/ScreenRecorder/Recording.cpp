#include "Recording.Media.h"
#include "Recording.Dx12.h"
#include <winrt/base.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <algorithm>
#include "../ScreenShotPlus/include/nlohmann/json.hpp"
#pragma comment(lib,"shell32.lib")
namespace aelura::recording {
using namespace std::chrono;
namespace {
std::wstring unique() { GUID guid{}; winrt::check_hresult(CoCreateGuid(&guid)); wchar_t s[40]{}; StringFromGUID2(guid,s,40); return s; }
void safeDirectory(std::filesystem::path const& directory) {
    if(!directory.is_absolute()) winrt::throw_hresult(E_INVALIDARG);
    for(auto p=directory; !p.empty(); p=p.parent_path()) {
        const auto a=GetFileAttributesW(p.c_str());
        if(a!=INVALID_FILE_ATTRIBUTES && ((a&FILE_ATTRIBUTE_REPARSE_POINT) || !(a&FILE_ATTRIBUTE_DIRECTORY))) winrt::throw_hresult(E_ACCESSDENIED);
        if(p==p.parent_path()) break;
    }
    std::filesystem::create_directories(directory);
}
std::filesystem::path localCache() {
    PWSTR p{}; winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&p));
    std::filesystem::path result(p); CoTaskMemFree(p); return result/L"AeluraTool"/L"Recordings";
}
struct Segment { std::filesystem::path path;std::shared_ptr<media::MemoryFile> memory; double duration{}; std::atomic<bool> discard{};std::atomic<unsigned> preserve{}; ~Segment() { if(!path.empty()&&discard&&!preserve) { std::error_code ec; std::filesystem::remove(path,ec); } } };
struct ReplayRing {std::mutex mutex;std::deque<std::shared_ptr<Segment>> segments;std::atomic<bool> limited{};};
HRESULT exceptionCode() { try { throw; } catch(winrt::hresult_error const& e) { return e.code(); } catch(std::filesystem::filesystem_error const& e) { return HRESULT_FROM_WIN32(e.code().value()); } catch(...) { return E_FAIL; } }
}
struct Session::Impl {
    mutable std::mutex mutex; Snapshot status;
    std::atomic<bool> stopping{},paused{},savingRequest{},cancelExport{},discardOnStop{};
    std::atomic<bool> finished{true};
    std::thread worker,exporter;
    ~Impl() { discardOnStop=true;stopping=true; if(worker.joinable()) worker.join(); if(exporter.joinable()) exporter.join(); }
    template<class F> void update(F&& f) { std::lock_guard<std::mutex> lock(mutex); f(status); }
    Snapshot read() const { std::lock_guard<std::mutex> lock(mutex); return status; }
    void run(Options o,RECT r,bool replay,HWND window) {
        const bool memoryReplay=replay&&o.replayStorage==ReplayStorage::Memory;
        auto ring=std::make_shared<ReplayRing>();auto& segments=ring->segments;
        auto budget=memoryReplay?std::make_shared<media::MemoryBudget>(uint64_t(o.memoryLimitMB)*1048576):nullptr;
        if(budget)budget->reclaim=[weak=std::weak_ptr<ReplayRing>(ring),quota=std::weak_ptr<media::MemoryBudget>(budget)](uint64_t bytes){
            auto queue=weak.lock();auto memory=quota.lock();if(!queue||!memory)return;std::lock_guard lock(queue->mutex);
            while(!queue->segments.empty()&&memory->used.load()>memory->limit-bytes){queue->limited=true;queue->segments.pop_front();}
        };
        std::shared_ptr<Segment> current;
        std::filesystem::path cache,partial,final;
        try {
            media::Runtime runtime;
            if(!memoryReplay){safeDirectory(o.directory);cache=localCache()/unique();safeDirectory(cache);update([&](auto& s){s.recovery=cache;});}
            final=o.directory/(L"Recording-"+unique()+L".mp4"); partial=final; partial.replace_extension(L".partial.mp4");
            std::unique_ptr<gpu::Pipeline> pipeline;
            std::unique_ptr<gpu::Dx12Pipeline> pipeline12;
            bool native=o.performanceBeta&&o.encoder==Encoder::GPU;
            if(o.graphics==Graphics::Direct3D12){
                if(window||!native)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
                pipeline12=std::make_unique<gpu::Dx12Pipeline>(o,r);update([&](auto& s){s.nativeDx12=true;s.optimizedVendor=int(pipeline12->vendor);s.adapter=pipeline12->adapter;});
            }
            if(!pipeline12&&(window||native)) {
                try {pipeline=std::make_unique<gpu::Pipeline>(o,r,window);const bool cuda=pipeline->vendor==0x10de && gpu::cudaAvailable();update([&](auto& s){s.optimizedVendor=native?int(pipeline->vendor):0;s.adapter=pipeline->adapter;s.cuda=cuda;});}
                catch(...){const auto hr=exceptionCode();update([&](auto& s){s.optimizationError=hr;});}
            }
            if(window&&!pipeline)winrt::throw_hresult(read().optimizationError);
            std::unique_ptr<media::Frames> frames;
            if(!pipeline&&!pipeline12)frames=std::make_unique<media::Frames>(o,r);
            media::Microphone mic(o.microphone);
            const auto width=pipeline12?pipeline12->width:pipeline?pipeline->width:frames->width,height=pipeline12?pipeline12->height:pipeline?pipeline->height:frames->height;
            update([&](auto& s) { s.width=width; s.height=height; });
            std::unique_ptr<media::Writer> writer;
            uint64_t tick=0,startTick=0,audioFrames=0;
            std::deque<int16_t> audioQueue;
            const uint64_t segmentTicks=memoryReplay?std::clamp<uint64_t>(budget->limit/10*8*o.fps/(uint64_t(o.bitrate)+256000),1,uint64_t(o.fps)*5):uint64_t(o.fps)*5;
            auto journal=[&] {
                if(memoryReplay)return;
                std::lock_guard lock(ring->mutex);
                nlohmann::json j; j["version"]=1; j["replay"]=replay; j["output"]=winrt::to_string(partial.wstring()); j["segments"]=nlohmann::json::array();
                for(auto const& seg:segments) j["segments"].push_back({{"path",winrt::to_string(seg->path.wstring())},{"seconds",seg->duration}});
                if(current) j["active"]=winrt::to_string(current->path.wstring());
                auto temp=cache/L"session.tmp"; { std::ofstream f(temp,std::ios::binary); f.exceptions(std::ios::badbit|std::ios::failbit); f<<j.dump(2); f.flush(); }
                if(!MoveFileExW(temp.c_str(),(cache/L"session.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
            };
            auto begin=[&] {
                current=std::make_shared<Segment>();
                if(memoryReplay)current->memory=std::make_shared<media::MemoryFile>(budget);
                else{
                    ULARGE_INTEGER available{};if(!GetDiskFreeSpaceExW((replay?cache:o.directory).c_str(),&available,nullptr,nullptr))winrt::throw_last_error();
                    if(available.QuadPart<std::max(16777216ULL,uint64_t(o.bitrate)*2))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_DISK_FULL));
                    current->path=replay?cache/(unique()+L".mp4"):partial;journal();
                    HANDLE h=CreateFileW(current->path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                    if(h==INVALID_HANDLE_VALUE)winrt::throw_last_error();CloseHandle(h);
                }
                try{writer=std::make_unique<media::Writer>(o,width,height,mic,current->path,native?pipeline.get():nullptr,pipeline12?gpu::nvidia12(o,pipeline12->device.Get(),width,height):nullptr,current->memory.get());}
                catch(...){
                    if(!pipeline || tick)throw;
                    const auto hr=exceptionCode();native=false;if(!window){pipeline.reset();frames=std::make_unique<media::Frames>(o,r);}
                    update([&](auto& s){s.optimizationError=hr;s.optimizedVendor=0;});
                    if(memoryReplay)current->memory=std::make_shared<media::MemoryFile>(budget);
                    writer=std::make_unique<media::Writer>(o,width,height,mic,current->path,nullptr,nullptr,current->memory.get());
                }
                startTick=tick; audioFrames=0;
            };
            auto finish=[&] {
                if(tick==startTick){writer.reset();if(!memoryReplay){std::error_code ec;std::filesystem::remove(current->path,ec);}current.reset();return;}
                writer->finish(); writer.reset(); current->duration=double(tick-startTick)/o.fps;
                if(current->memory)current->memory->seal();
                if(replay){std::lock_guard lock(ring->mutex);segments.push_back(current);}current.reset();
            };
            auto prune=[&] {
                std::lock_guard lock(ring->mutex);
                std::vector<double> lengths; for(auto const& s:segments) lengths.push_back(s->duration);
                auto first=replayFirst(lengths,o.replaySeconds);
                while(first--) { segments.front()->discard=true; segments.pop_front(); }
                if(memoryReplay){uint64_t bytes{};for(auto const& s:segments)bytes+=s->memory->allocated();while(!segments.empty()&&bytes>budget->limit*4/5){ring->limited=true;bytes-=segments.front()->memory->allocated();segments.pop_front();}}
            };
            begin();
            mic.read();
            auto epoch=steady_clock::now(); auto pauseStart=epoch; bool wasPaused=false;
            update([](auto& s) { s.state=State::Recording; });
            while(!stopping) {
                const auto now=steady_clock::now();
                if(paused) {
                    if(!wasPaused) { pauseStart=now; wasPaused=true; update([](auto& s){s.state=State::Paused;}); }
                    mic.read(); audioQueue.clear(); std::this_thread::sleep_for(milliseconds(10)); continue;
                }
                if(wasPaused) { epoch+=now-pauseStart; wasPaused=false; update([](auto& s){s.state=State::Recording;}); }
                auto due=epoch+nanoseconds(tick*1000000000ULL/o.fps);
                if(now<due) { std::this_thread::sleep_for(std::min(milliseconds(5),duration_cast<milliseconds>(due-now)+milliseconds(1))); continue; }
                // Refresh the virtual desktop each frame; a removed monitor must stop capture.
                RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
                desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN); desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
                RECT clipped{}; if(!window&&(!IntersectRect(&clipped,&r,&desktop) || !EqualRect(&clipped,&r))) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_MONITOR_HANDLE));
                ID3D11Texture2D* texture{};
                ID3D12Resource* texture12{};
                if(pipeline12)texture12=pipeline12->next();
                if(window)update([&](auto& s){s.windowMinimized=IsIconic(window)!=FALSE;});
                if(pipeline){if(pipeline->sourceClosed())break;texture=pipeline->next();if(!texture){mic.read();std::this_thread::sleep_for(milliseconds(10));continue;}}
                if(!read().frames){epoch=now;tick=0;}
                const auto actual=uint64_t(duration_cast<nanoseconds>(now-epoch).count())*o.fps/1000000000ULL;
                if(actual>tick) { const auto dropped=actual-tick; tick=actual; update([&](auto& s){s.dropped+=dropped;}); }
                const auto t=LONGLONG(tick-startTick)*10000000/o.fps, end=LONGLONG(tick-startTick+1)*10000000/o.fps;
                if(pipeline12)writer->texture12(texture12,t,end-t);else if(pipeline&&native)writer->texture(texture,t,end-t);else writer->frame(pipeline?pipeline->readback():frames->next(),t,end-t);
                if(mic.channels) {
                    auto pcm=mic.read(); audioQueue.insert(audioQueue.end(),pcm.begin(),pcm.end());
                    const auto target=uint64_t(end)*mic.rate/10000000, needed=target-audioFrames;
                    if(needed>uint64_t(mic.rate)*30) winrt::throw_hresult(E_ABORT);
                    std::vector<int16_t> chunk(size_t(needed)*mic.channels);
                    for(auto& sample:chunk) { if(!audioQueue.empty()) { sample=audioQueue.front(); audioQueue.pop_front(); } }
                    writer->sound(chunk,LONGLONG(audioFrames)*10000000/mic.rate); audioFrames=target;
                    // Limit device clock drift to 250 ms; never let a backlog grow unbounded.
                    while(audioQueue.size()>size_t(mic.rate)*mic.channels/4) audioQueue.pop_front();
                }
                ++tick;
                update([&](auto& s){++s.frames;s.seconds=double(tick)/o.fps;});
                if(replay && (tick-startTick>=segmentTicks || savingRequest.load())) {
                    finish(); prune(); journal();
                    if(savingRequest.exchange(false)) {
                        if(exporter.joinable()) exporter.join();
                        std::vector<std::shared_ptr<Segment>> snapshot;{std::lock_guard lock(ring->mutex);snapshot.assign(segments.begin(),segments.end());}
                        if(!memoryReplay)for(auto const& seg:snapshot)++seg->preserve;
                        auto destination=o.directory/(L"Replay-"+unique()+L".mp4");
                        auto manifest=cache/(unique()+L".replay.json");
                        if(!memoryReplay){nlohmann::json j;j["segments"]=nlohmann::json::array();for(auto const& seg:snapshot)j["segments"].push_back({{"path",winrt::to_string(seg->path.wstring())},{"seconds",seg->duration}});std::ofstream f(manifest,std::ios::binary);f.exceptions(std::ios::failbit|std::ios::badbit);f<<j.dump(2);}
                        exporter=std::thread([this,snapshot=std::move(snapshot),destination,manifest,memoryReplay] {
                            auto temp=destination; temp.replace_extension(L".partial.mp4");
                            try {
                                auto progress=[this](double p){update([&](auto& s){s.saveProgress=p;});return !cancelExport.load();};
                                safeDirectory(destination.parent_path());
                                if(memoryReplay){std::vector<std::shared_ptr<media::MemoryFile>> files;for(auto const& seg:snapshot)files.push_back(seg->memory);media::joinMemory(files,temp,progress);}
                                else{std::vector<std::filesystem::path> paths;for(auto const& seg:snapshot)paths.push_back(seg->path);join(paths,temp,progress);}
                                if(!MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
                                if(!memoryReplay){for(auto const& seg:snapshot)--seg->preserve;std::error_code ec;std::filesystem::remove(manifest,ec);}
                                update([&](auto& s){s.output=destination;++s.savedGeneration;s.error=S_OK;s.saving=false;});
                            } catch(...) { const auto hr=exceptionCode(); update([&](auto& s){s.error=hr;s.saving=false;}); }
                        });
                    }
                    begin();
                }
                if(replay) {
                    std::lock_guard lock(ring->mutex);
                    double buffered=double(tick-startTick)/o.fps; for(auto const& s:segments) buffered+=s->duration;
                    update([&](auto& s){s.bufferedSeconds=std::min(buffered,double(o.replaySeconds));if(budget){s.cacheBytes=budget->used.load();s.memoryLimited=ring->limited;}});
                }
            }
            update([](auto& s){s.state=State::Stopping;});
            if(!tick){writer.reset();if(!memoryReplay){if(current){std::error_code ec;std::filesystem::remove(current->path,ec);}std::error_code ec;std::filesystem::remove(cache/L"session.json",ec);std::filesystem::remove(cache,ec);}current.reset();update([](auto& s){s.state=State::Completed;s.saving=false;s.cacheBytes=0;s.recovery.clear();});return;}
            finish();
            if(exporter.joinable()) exporter.join();
            if(!replay) {
                if(!MoveFileExW(partial.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
                update([&](auto& s){s.output=final;++s.savedGeneration;});
            } else {
                // Retain the final rolling buffer for recovery/export after stopping.
                prune(); journal();
                if(savingRequest.exchange(false)){
                    auto output=o.directory/(L"Replay-"+unique()+L".mp4");auto temp=output;temp.replace_extension(L".partial.mp4");
                    auto progress=[this](double p){update([&](auto& s){s.saveProgress=p;});return !cancelExport.load();};safeDirectory(o.directory);
                    if(memoryReplay){std::vector<std::shared_ptr<media::MemoryFile>> files;{std::lock_guard lock(ring->mutex);for(auto const& seg:segments)files.push_back(seg->memory);}media::joinMemory(files,temp,progress);}
                    else{std::vector<std::filesystem::path> paths;{std::lock_guard lock(ring->mutex);for(auto const& seg:segments)paths.push_back(seg->path);}join(paths,temp,progress);}
                    if(!MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH))winrt::throw_last_error();
                    update([&](auto& s){s.output=output;++s.savedGeneration;s.saving=false;});
                }
                if(memoryReplay){std::lock_guard lock(ring->mutex);segments.clear();update([](auto& s){s.cacheBytes=0;s.bufferedSeconds=0;});}
                else if(discardOnStop){
                    std::lock_guard lock(ring->mutex);
                    bool preserved=false;
                    for(auto& seg:segments){if(seg->preserve)preserved=true;else seg->discard=true;}
                    segments.clear();
                    if(!preserved){std::error_code ec;std::filesystem::remove(cache/L"session.json",ec);std::filesystem::remove(cache/L"session.tmp",ec);const bool removed=std::filesystem::remove(cache,ec);if(removed)update([](auto& s){s.recovery.clear();});}
                }
            }
            if(!replay) { std::error_code ec; std::filesystem::remove(cache/L"session.json",ec); std::filesystem::remove(cache,ec); }
            update([](auto& s){s.state=State::Completed;s.saving=false;});
        } catch(...) {
            const auto hr=exceptionCode();
            // Keep finalized segments and the partial file, together with the journal.
            if(exporter.joinable()) exporter.join();
            {std::lock_guard lock(ring->mutex);for(auto& s:segments)s->discard=false;if(memoryReplay)segments.clear();}
            update([&](auto& s){s.state=State::Failed;s.error=hr;s.saving=false;if(memoryReplay)s.cacheBytes=0;});
        }
    }
};
Session::Session():impl_(std::make_unique<Impl>()) {}
Session::~Session()=default;
Snapshot Session::snapshot() const { return impl_->read(); }
bool Session::busy() const { const auto s=snapshot(); return !impl_->finished || s.saving || s.state==State::Starting || s.state==State::Recording || s.state==State::Paused || s.state==State::Stopping; }
bool Session::start(Options const& o,RECT r,bool replay,HWND window) {
    if(busy()) return false; validate(o,r);
    if(window&&(!IsWindow(window)||replay))winrt::throw_hresult(E_INVALIDARG);
    if(impl_->worker.joinable()) impl_->worker.join(); if(impl_->exporter.joinable()) impl_->exporter.join();
    impl_->stopping=false;impl_->paused=false;impl_->savingRequest=false;impl_->discardOnStop=false;impl_->finished=false;
    impl_->update([&](auto& s){s={};s.state=State::Starting;s.replay=replay;s.memoryReplay=replay&&o.replayStorage==ReplayStorage::Memory;s.windowCapture=window!=nullptr;if(window){GetWindowThreadProcessId(window,&s.processId);wchar_t title[1024]{};GetWindowTextW(window,title,1024);s.windowTitle=title;}});
    try { impl_->worker=std::thread([p=impl_.get(),o,r,replay,window]{p->run(o,r,replay,window);p->finished=true;}); }
    catch(...) { impl_->finished=true;impl_->update([](auto& s){s.state=State::Failed;s.error=E_OUTOFMEMORY;}); throw; }
    return true;
}
void Session::pause() { auto s=snapshot(); if(s.state==State::Recording || s.state==State::Paused) impl_->paused=!impl_->paused.load(); }
void Session::stop(bool discard) { if(discard)impl_->discardOnStop=true;impl_->stopping=true; }
void Session::wait(){if(impl_->worker.joinable())impl_->worker.join();if(impl_->exporter.joinable())impl_->exporter.join();}
bool Session::saveReplay() {
    std::lock_guard<std::mutex> lock(impl_->mutex); auto& s=impl_->status;
    if(!s.replay || s.saving || s.state!=State::Recording || !s.frames) return false;
    s.saving=true;s.saveProgress=0;impl_->cancelExport=false; impl_->savingRequest=true; return true;
}
void Session::cancelSave(){impl_->cancelExport=true;}
bool Session::recover(std::filesystem::path const& cache,std::filesystem::path const& directory){
    if(busy())return false;
    if(impl_->worker.joinable())impl_->worker.join();if(impl_->exporter.joinable())impl_->exporter.join();
    impl_->cancelExport=false;impl_->update([&](auto& s){s={};s.state=State::Stopping;s.saving=true;s.recovery=cache;});
    impl_->worker=std::thread([p=impl_.get(),selected=cache,directory]{
        try{
            const auto cache=selected.extension()==L".json"?selected.parent_path():selected;
            const auto manifest=selected.extension()==L".json"?selected:cache/L"session.json";
            safeDirectory(directory);
            if(GetFileAttributesW(cache.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)winrt::throw_hresult(E_ACCESSDENIED);
            if(GetFileAttributesW(manifest.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)winrt::throw_hresult(E_ACCESSDENIED);
            std::ifstream stream(manifest);auto json=nlohmann::json::parse(stream);auto items=json.at("segments");
            if(!items.is_array()||items.empty()||items.size()>256)winrt::throw_hresult(E_INVALIDARG);
            std::vector<std::filesystem::path> files;
            for(auto const& item:items){std::filesystem::path recorded(winrt::to_hstring(item.at("path").get<std::string>()).c_str());
                auto name=recorded.filename();if(name.empty()||name.extension()!=L".mp4"||name.wstring().find_first_of(L":/\\")!=std::wstring::npos)winrt::throw_hresult(E_ACCESSDENIED);
                auto path=cache/name;
                if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES||(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT))winrt::throw_hresult(E_ACCESSDENIED);files.push_back(path);
            }
            auto output=directory/(L"Recovered-"+unique()+L".mp4"),temp=output;temp.replace_extension(L".partial.mp4");
            join(files,temp,[p](double percent){p->update([&](auto& s){s.saveProgress=percent;});return !p->cancelExport.load();});
            if(!MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH))winrt::throw_last_error();
            p->update([&](auto& s){s.output=output;++s.savedGeneration;s.state=State::Completed;s.saving=false;});
        }catch(...){auto hr=exceptionCode();p->update([&](auto& s){s.state=State::Failed;s.error=hr;s.saving=false;});}
    });return true;
}
}
