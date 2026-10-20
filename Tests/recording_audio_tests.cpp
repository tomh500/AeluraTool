#include "../Tools/ScreenRecorder/Recording.Audio.h"
#include <winrt/base.h>
#include <cmath>
#include <iostream>
#include <thread>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
using namespace aelura::recording;
namespace {
void require(bool value,char const* text){if(!value)throw std::runtime_error(text);}
double toneAmplitude(std::span<int16_t const> pcm){double sine=0,cosine=0;size_t frames=pcm.size()/2;for(size_t i=0;i<frames;++i){auto phase=6.283185307179586*880*double(i)/48000;sine+=pcm[i*2]*sin(phase);cosine+=pcm[i*2]*cos(phase);}return frames?2*sqrt(sine*sine+cosine*cosine)/frames:0;}
struct Tone {
    media::Ptr<IAudioClient> client;media::Ptr<IAudioRenderClient> render;UINT32 capacity{};uint64_t frame{};
    Tone(){media::Ptr<IMMDeviceEnumerator> enumerator;media::Ptr<IMMDevice> endpoint;winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)));winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&endpoint));winrt::check_hresult(endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));WAVEFORMATEX format{WAVE_FORMAT_PCM,2,44100,176400,4,16,0};winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&format,nullptr));winrt::check_hresult(client->GetBufferSize(&capacity));winrt::check_hresult(client->GetService(IID_PPV_ARGS(&render)));fill();winrt::check_hresult(client->Start());}
    void fill(){UINT32 padding{};winrt::check_hresult(client->GetCurrentPadding(&padding));auto count=capacity-padding;if(!count)return;BYTE* bytes{};winrt::check_hresult(render->GetBuffer(count,&bytes));auto pcm=reinterpret_cast<int16_t*>(bytes);for(UINT32 i=0;i<count;++i){auto sample=int16_t(1200*sin(6.283185307179586*880*double(frame++)/44100));pcm[i*2]=pcm[i*2+1]=sample;}winrt::check_hresult(render->ReleaseBuffer(count,0));}
    ~Tone(){if(client)client->Stop();}
};
std::vector<int16_t> decodedAudio(std::filesystem::path const& file){
    media::Ptr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,&reader));winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),TRUE));
    media::Ptr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),nullptr,type.Get()));std::vector<int16_t> pcm;
    for(;;){DWORD flags{};media::Ptr<IMFSample> sample;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),0,nullptr,&flags,nullptr,&sample));if(sample){media::Ptr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));BYTE* bytes{};DWORD size{};winrt::check_hresult(buffer->Lock(&bytes,nullptr,&size));auto source=reinterpret_cast<int16_t*>(bytes);pcm.insert(pcm.end(),source,source+size/2);buffer->Unlock();}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}return pcm;
}
}
void audioTests(std::filesystem::path const& root,bool live){
    std::vector<int16_t> first{30000,-30000,1000,-1000},second{10000,-10000,-2000,2000},mixed(6);media::mixPcm(first,second,mixed);require(mixed==std::vector<int16_t>({32767,-32768,-1000,1000,0,0}),"PCM mixing/clipping/silence padding failed");
    Options o;o.directory=root;require(o.systemAudio,"system audio is not enabled by default");save(root/L"audio-settings.json",o);require(load(root/L"audio-settings.json").systemAudio,"system audio true did not persist");o.systemAudio=false;save(root/L"audio-settings.json",o);require(!load(root/L"audio-settings.json").systemAudio,"system audio false did not persist");
    {std::ifstream input(root/L"audio-settings.json");auto legacy=nlohmann::json::parse(input);legacy.erase("systemAudio");std::ofstream file(root/L"audio-legacy.json");file<<legacy;file.close();require(load(root/L"audio-legacy.json").systemAudio,"old settings did not enable system audio by default");}
    {media::AudioMix silence(false,false);require(!silence.channels&&silence.take(100).empty(),"disabled audio still generated a track");}
    std::cout<<"PASS: saturating PCM mix, zero padding, system-audio default and persistent disable, silent mode\n";if(!live)return;
    media::AudioMix audio(true,false);Tone tone;std::vector<int16_t> captured;auto start=GetTickCount64();uint64_t taken=0;
    while(GetTickCount64()-start<3000){tone.fill();auto due=(GetTickCount64()-start)*48;if(due>taken){auto part=audio.take(size_t(due-taken));captured.insert(captured.end(),part.begin(),part.end());taken=due;}Sleep(5);}
    winrt::check_hresult(audio.systemError());auto amplitude=toneAmplitude(captured);std::cout<<"CHECK: actual WASAPI loopback 880 Hz amplitude="<<amplitude<<"\n";require(amplitude>50,"generated system tone was not captured");
    auto file=root/L"system-audio.mp4";o.systemAudio=true;o.bitrate=1500000;media::Writer writer(o,320,240,audio,file);std::vector<BYTE> pixels(320*240*3/2,128);std::fill(pixels.begin(),pixels.begin()+320*240,BYTE(80));captured.resize(90*1600*2);
    for(unsigned n=0;n<90;++n){writer.frame(pixels,LONGLONG(n)*10000000/30,10000000/30);writer.sound(std::vector<int16_t>(captured.begin()+n*3200,captured.begin()+(n+1)*3200),LONGLONG(n)*10000000/30);}writer.finish();auto decoded=decodedAudio(file);require(toneAmplitude(decoded)>40,"AAC video lost the captured system tone");
    std::cout<<"PASS: real 44.1 kHz render / 48 kHz default-device loopback, independent capture thread, captured 880 Hz tone, MP4 AAC decode retains tone; microphone unopened\n";
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.lpszClassName=L"AeluraSystemAudioSession";RegisterClassW(&wc);
    struct Window{HWND hwnd{};~Window(){if(hwnd)DestroyWindow(hwnd);}} window;window.hwnd=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Aelura generated audio session",WS_POPUP,120,160,320,240,nullptr,nullptr,wc.hInstance,nullptr);winrt::check_bool(window.hwnd!=nullptr);ShowWindow(window.hwnd,SW_SHOW);UpdateWindow(window.hwnd);RECT region{};GetWindowRect(window.hwnd,&region);
    o.directory=root/L"session-audio";o.encoder=Encoder::GPU;o.performanceBeta=true;o.graphics=Graphics::Direct3D12;o.replayStorage=ReplayStorage::Memory;o.microphone=false;o.systemAudio=true;Session session;require(session.start(o,region,true),"system-audio replay session rejected");
    auto wait=[&](auto condition){auto deadline=GetTickCount64()+15000;while(!condition()){tone.fill();auto status=session.snapshot();winrt::check_hresult(status.error);require(GetTickCount64()<deadline,"system-audio session timeout");MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(5);}};
    wait([&]{return session.snapshot().frames>=75;});require(session.saveReplay(),"system-audio replay save rejected");wait([&]{return session.snapshot().savedGeneration>0;});auto saved=session.snapshot();require(saved.nativeDx12&&!saved.dx12Fallback,"system audio test silently fell back from DX12");winrt::check_hresult(saved.systemAudioError);require(toneAmplitude(decodedAudio(saved.output))>40,"actual replay session did not record system audio");session.stop(true);wait([&]{return !session.busy();});session.wait();
    std::cout<<"PASS: actual DX12 RAM replay session records generated system tone with microphone off, exports AAC, consumes cache and stops cleanly\n";
}
