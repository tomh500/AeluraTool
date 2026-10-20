#include "Recording.Media.h"
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
struct Segment { std::filesystem::path path; double duration{}; std::atomic<bool> discard{};std::atomic<unsigned> preserve{}; ~Segment() { if(discard&&!preserve) { std::error_code ec; std::filesystem::remove(path,ec); } } };
HRESULT exceptionCode() { try { throw; } catch(winrt::hresult_error const& e) { return e.code(); } catch(std::filesystem::filesystem_error const& e) { return HRESULT_FROM_WIN32(e.code().value()); } catch(...) { return E_FAIL; } }
}
struct Session::Impl {
    mutable std::mutex mutex; Snapshot status;
    std::atomic<bool> stopping{},paused{},savingRequest{},cancelExport{};
    std::thread worker,exporter;
    ~Impl() { stopping=true; if(worker.joinable()) worker.join(); if(exporter.joinable()) exporter.join(); }
    template<class F> void update(F&& f) { std::lock_guard<std::mutex> lock(mutex); f(status); }
    Snapshot read() const { std::lock_guard<std::mutex> lock(mutex); return status; }
    void run(Options o,RECT r,bool replay) {
        std::deque<std::shared_ptr<Segment>> segments;
        std::shared_ptr<Segment> current;
        std::filesystem::path cache,partial,final;
        try {
            media::Runtime runtime;
            safeDirectory(o.directory);
            cache=localCache()/unique(); safeDirectory(cache);
            update([&](auto& s) { s.recovery=cache; });
            final=o.directory/(L"Recording-"+unique()+L".mp4"); partial=final; partial.replace_extension(L".partial.mp4");
            std::unique_ptr<gpu::Pipeline> pipeline;
            if(o.performanceBeta && o.encoder==Encoder::GPU) {
                try {pipeline=std::make_unique<gpu::Pipeline>(o,r);const bool cuda=pipeline->vendor==0x10de && gpu::cudaAvailable();update([&](auto& s){s.optimizedVendor=int(pipeline->vendor);s.adapter=pipeline->adapter;s.cuda=cuda;});}
                catch(...){const auto hr=exceptionCode();update([&](auto& s){s.optimizationError=hr;});}
            }
            std::unique_ptr<media::Frames> frames;
            if(!pipeline)frames=std::make_unique<media::Frames>(o,r);
            media::Microphone mic(o.microphone);
            const auto width=pipeline?pipeline->width:frames->width,height=pipeline?pipeline->height:frames->height;
            update([&](auto& s) { s.width=width; s.height=height; });
            std::unique_ptr<media::Writer> writer;
            uint64_t tick=0,startTick=0,audioFrames=0;
            std::deque<int16_t> audioQueue;
            auto journal=[&] {
                nlohmann::json j; j["version"]=1; j["replay"]=replay; j["output"]=winrt::to_string(partial.wstring()); j["segments"]=nlohmann::json::array();
                for(auto const& seg:segments) j["segments"].push_back({{"path",winrt::to_string(seg->path.wstring())},{"seconds",seg->duration}});
                if(current) j["active"]=winrt::to_string(current->path.wstring());
                auto temp=cache/L"session.tmp"; { std::ofstream f(temp,std::ios::binary); f.exceptions(std::ios::badbit|std::ios::failbit); f<<j.dump(2); f.flush(); }
                if(!MoveFileExW(temp.c_str(),(cache/L"session.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
            };
            auto begin=[&] {
                ULARGE_INTEGER available{};if(!GetDiskFreeSpaceExW((replay?cache:o.directory).c_str(),&available,nullptr,nullptr))winrt::throw_last_error();
                if(available.QuadPart<std::max(16777216ULL,uint64_t(o.bitrate)*2))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_DISK_FULL));
                current=std::make_shared<Segment>(); current->path=replay?cache/(unique()+L".mp4"):partial;
                journal(); // Recovery metadata exists before writing the first byte.
                HANDLE h=CreateFileW(current->path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(h==INVALID_HANDLE_VALUE) winrt::throw_last_error(); CloseHandle(h);
                try{writer=std::make_unique<media::Writer>(o,width,height,mic,current->path,pipeline.get());}
                catch(...){
                    if(!pipeline || tick)throw;
                    const auto hr=exceptionCode();pipeline.reset();frames=std::make_unique<media::Frames>(o,r);
                    update([&](auto& s){s.optimizationError=hr;s.optimizedVendor=0;});
                    writer=std::make_unique<media::Writer>(o,width,height,mic,current->path);
                }
                startTick=tick; audioFrames=0;
            };
            auto finish=[&] {
                if(tick==startTick){writer.reset();std::error_code ec;std::filesystem::remove(current->path,ec);current.reset();return;}
                writer->finish(); writer.reset(); current->duration=double(tick-startTick)/o.fps;
                if(replay) segments.push_back(current); current.reset();
            };
            auto prune=[&] {
                std::vector<double> lengths; for(auto const& s:segments) lengths.push_back(s->duration);
                auto first=replayFirst(lengths,o.replaySeconds);
                while(first--) { segments.front()->discard=true; segments.pop_front(); }
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
                const auto actual=uint64_t(duration_cast<nanoseconds>(now-epoch).count())*o.fps/1000000000ULL;
                if(actual>tick) { const auto dropped=actual-tick; tick=actual; update([&](auto& s){s.dropped+=dropped;}); }
                // Refresh the virtual desktop each frame; a removed monitor must stop capture.
                RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
                desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN); desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
                RECT clipped{}; if(!IntersectRect(&clipped,&r,&desktop) || !EqualRect(&clipped,&r)) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_MONITOR_HANDLE));
                const auto t=LONGLONG(tick-startTick)*10000000/o.fps, end=LONGLONG(tick-startTick+1)*10000000/o.fps;
                if(pipeline)writer->texture(pipeline->next(),t,end-t);else writer->frame(frames->next(),t,end-t);
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
                if(replay && (tick-startTick>=o.fps*5 || savingRequest.load())) {
                    finish(); prune(); journal();
                    if(savingRequest.exchange(false)) {
                        if(exporter.joinable()) exporter.join();
                        std::vector<std::shared_ptr<Segment>> snapshot(segments.begin(),segments.end());
                        for(auto const& seg:snapshot)++seg->preserve;
                        auto destination=o.directory/(L"Replay-"+unique()+L".mp4");
                        auto manifest=cache/(unique()+L".replay.json");
                        {nlohmann::json j;j["segments"]=nlohmann::json::array();for(auto const& seg:snapshot)j["segments"].push_back({{"path",winrt::to_string(seg->path.wstring())},{"seconds",seg->duration}});std::ofstream f(manifest,std::ios::binary);f.exceptions(std::ios::failbit|std::ios::badbit);f<<j.dump(2);}
                        exporter=std::thread([this,snapshot=std::move(snapshot),destination,manifest] {
                            auto temp=destination; temp.replace_extension(L".partial.mp4");
                            try {
                                std::vector<std::filesystem::path> paths; for(auto const& seg:snapshot) paths.push_back(seg->path);
                                join(paths,temp,[this](double p){update([&](auto& s){s.saveProgress=p;});return !cancelExport.load();}); if(!MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
                                for(auto const& seg:snapshot)--seg->preserve;std::error_code ec;std::filesystem::remove(manifest,ec);
                                update([&](auto& s){s.output=destination;++s.savedGeneration;s.error=S_OK;s.saving=false;});
                            } catch(...) { const auto hr=exceptionCode(); update([&](auto& s){s.error=hr;s.saving=false;}); }
                        });
                    }
                    begin();
                }
                if(replay) {
                    double buffered=double(tick-startTick)/o.fps; for(auto const& s:segments) buffered+=s->duration;
                    update([&](auto& s){s.bufferedSeconds=std::min(buffered,double(o.replaySeconds));});
                }
            }
            update([](auto& s){s.state=State::Stopping;});
            if(!tick){writer.reset();if(current){std::error_code ec;std::filesystem::remove(current->path,ec);}update([](auto& s){s.state=State::Completed;s.saving=false;});return;}
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
                    std::vector<std::filesystem::path> paths;for(auto const& seg:segments)paths.push_back(seg->path);
                    join(paths,temp,[this](double p){update([&](auto& s){s.saveProgress=p;});return !cancelExport.load();});
                    if(!MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH))winrt::throw_last_error();
                    update([&](auto& s){s.output=output;++s.savedGeneration;s.saving=false;});
                }
            }
            if(!replay) { std::error_code ec; std::filesystem::remove(cache/L"session.json",ec); std::filesystem::remove(cache,ec); }
            update([](auto& s){s.state=State::Completed;s.saving=false;});
        } catch(...) {
            const auto hr=exceptionCode();
            // Keep finalized segments and the partial file, together with the journal.
            for(auto& s:segments) s->discard=false;
            if(exporter.joinable()) exporter.join();
            update([&](auto& s){s.state=State::Failed;s.error=hr;s.saving=false;});
        }
    }
};
Session::Session():impl_(std::make_unique<Impl>()) {}
Session::~Session()=default;
Snapshot Session::snapshot() const { return impl_->read(); }
bool Session::busy() const { const auto s=snapshot(); return s.saving || s.state==State::Starting || s.state==State::Recording || s.state==State::Paused || s.state==State::Stopping; }
bool Session::start(Options const& o,RECT r,bool replay) {
    if(busy()) return false; validate(o,r);
    if(impl_->worker.joinable()) impl_->worker.join(); if(impl_->exporter.joinable()) impl_->exporter.join();
    impl_->stopping=false;impl_->paused=false;impl_->savingRequest=false;
    impl_->update([&](auto& s){s={};s.state=State::Starting;s.replay=replay;});
    try { impl_->worker=std::thread([p=impl_.get(),o,r,replay]{p->run(o,r,replay);}); }
    catch(...) { impl_->update([](auto& s){s.state=State::Failed;s.error=E_OUTOFMEMORY;}); throw; }
    return true;
}
void Session::pause() { auto s=snapshot(); if(s.state==State::Recording || s.state==State::Paused) impl_->paused=!impl_->paused.load(); }
void Session::stop() { impl_->stopping=true; }
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
