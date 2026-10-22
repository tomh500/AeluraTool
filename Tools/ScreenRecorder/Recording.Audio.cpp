#include "Recording.Audio.h"
#include <winrt/base.h>
#include <deque>
#include <chrono>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
namespace aelura::recording::media {
namespace {
struct Source {
    bool enabled{},loopback{};std::unique_ptr<Microphone> capture;std::deque<int16_t> queue;HRESULT error{S_OK};
    std::chrono::steady_clock::time_point checkAt{};
    Source(bool on,bool loop):enabled(on),loopback(loop){}
    void poll(){
        if(!enabled)return;const auto now=std::chrono::steady_clock::now();
        try{
            if(now>=checkAt){
                checkAt=now+std::chrono::seconds(1);
                Ptr<IMMDeviceEnumerator> enumerator;Ptr<IMMDevice> endpoint;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)));
                winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(loopback?eRender:eCapture,loopback?eConsole:eCommunications,&endpoint));
                LPWSTR id{};winrt::check_hresult(endpoint->GetId(&id));std::wstring current=id;CoTaskMemFree(id);
                if(capture&&capture->deviceId!=current){capture.reset();queue.clear();}
                if(!capture)capture=std::make_unique<Microphone>(true,loopback);
            }
            if(capture){auto samples=capture->read();queue.insert(queue.end(),samples.begin(),samples.end());error=S_OK;}
            // Bound latency and storage independently for each clock/source.
            while(queue.size()>48000*2*2)queue.pop_front();
        }catch(winrt::hresult_error const& e){error=e.code();capture.reset();queue.clear();}
        catch(...){error=E_FAIL;capture.reset();queue.clear();}
    }
    std::vector<int16_t> take(size_t count){std::vector<int16_t> result(count);for(auto& sample:result)if(!queue.empty()){sample=queue.front();queue.pop_front();}return result;}
};
}
void mixPcm(std::span<int16_t const> first,std::span<int16_t const> second,std::span<int16_t> output){
    for(size_t i=0;i<output.size();++i){int sample=(i<first.size()?first[i]:0)+(i<second.size()?second[i]:0);output[i]=int16_t(std::clamp(sample,-32768,32767));}
}
struct AudioMix::Impl {
    Source system,microphone;std::mutex mutex;std::atomic<bool> stop{};std::thread worker;
    Impl(bool s,bool m):system(s,true),microphone(m,false){if(s||m)worker=std::thread([this]{
        auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr)){std::lock_guard lock(mutex);if(system.enabled)system.error=hr;if(microphone.enabled)microphone.error=hr;return;}
        while(!stop){{std::lock_guard lock(mutex);system.poll();microphone.poll();}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        {std::lock_guard lock(mutex);system.capture.reset();microphone.capture.reset();}CoUninitialize();
    });}
    ~Impl(){stop=true;if(worker.joinable())worker.join();}
};
AudioMix::AudioMix(bool s,bool m):impl_(std::make_unique<Impl>(s,m)){if(s||m){rate=48000;channels=2;}}
AudioMix::~AudioMix()=default;
std::vector<int16_t> AudioMix::take(size_t frames){
    if(!channels)return {};std::lock_guard lock(impl_->mutex);auto system=impl_->system.take(frames*channels),mic=impl_->microphone.take(frames*channels);std::vector<int16_t> output(frames*channels);mixPcm(system,mic,output);return output;
}
void AudioMix::discard(){std::lock_guard lock(impl_->mutex);impl_->system.queue.clear();impl_->microphone.queue.clear();}
HRESULT AudioMix::systemError()const{std::lock_guard lock(impl_->mutex);return impl_->system.error;}
HRESULT AudioMix::microphoneError()const{std::lock_guard lock(impl_->mutex);return impl_->microphone.error;}
}
