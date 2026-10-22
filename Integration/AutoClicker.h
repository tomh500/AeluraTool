#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <future>
#include <random>
#include <chrono>
#include <algorithm>
#include <cmath>
#include "../Tools/DearM16A4/Timing.h"

namespace aelura::autoclicker {
struct Options { UINT trigger{VK_XBUTTON1}, target{VK_LBUTTON}; int rate{10}, jitter{10}; };
inline bool valid(Options const& o) {
    auto mouse = [](UINT k) { return k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON || k == VK_XBUTTON1 || k == VK_XBUTTON2; };
    return mouse(o.trigger) && (mouse(o.target) || (o.target >= 'A' && o.target <= 'Z') || (o.target >= '0' && o.target <= '9') || (o.target >= VK_F1 && o.target <= VK_F24)) && o.rate >= 1 && o.rate <= 100 && o.jitter >= 0 && o.jitter <= 90;
}
inline double interval(Options const& o, double random) { return 1000.0 / o.rate * (1.0 + o.jitter / 100.0 * random); }
inline double sampleInterval(Options const& o, size_t index, double noise) {
    double mean=0;for(auto sample:samples)mean+=sample;mean/=samples.size();
    double deviation=0;for(auto sample:samples)deviation=(std::max)(deviation,std::abs(sample-mean));
    double factor=(samples[index%samples.size()]-mean+noise)/(deviation+3.0);
    return interval(o,(std::clamp)(factor,-1.0,1.0));
}
inline double pressDuration(double period,unsigned random) {return (std::min)(15.0+random%10,period*0.45);}
inline INPUT inputPacket(UINT key,bool up,HKL layout) {
    INPUT input{};
    if(key==VK_LBUTTON||key==VK_RBUTTON||key==VK_MBUTTON||key==VK_XBUTTON1||key==VK_XBUTTON2) {
        input.type=INPUT_MOUSE;
        input.mi.dwFlags=key==VK_LBUTTON?(up?MOUSEEVENTF_LEFTUP:MOUSEEVENTF_LEFTDOWN):key==VK_RBUTTON?(up?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_RIGHTDOWN):key==VK_MBUTTON?(up?MOUSEEVENTF_MIDDLEUP:MOUSEEVENTF_MIDDLEDOWN):(up?MOUSEEVENTF_XUP:MOUSEEVENTF_XDOWN);
        input.mi.mouseData=key==VK_XBUTTON1?XBUTTON1:key==VK_XBUTTON2?XBUTTON2:0;
    } else {
        input.type=INPUT_KEYBOARD;auto scan=MapVirtualKeyExW(key,MAPVK_VK_TO_VSC_EX,layout);
        input.ki.wScan=static_cast<WORD>(scan&0xff);input.ki.dwFlags=KEYEVENTF_SCANCODE|(up?KEYEVENTF_KEYUP:0)|((scan&0xff00)?KEYEVENTF_EXTENDEDKEY:0);
    }
    return input;
}
class Engine {
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, failed_{false};
    Options options_;
    HWND excluded_{};
    bool held_{};
    inline static thread_local Engine* current_{};
    static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) {
        auto self = current_;
        if (code == HC_ACTION && self) {
            auto const& event = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
            if (!(event.flags & LLMHF_INJECTED)) {
                if(self->excluded_ && GetAncestor(GetForegroundWindow(),GA_ROOT)==self->excluded_) {
                    self->held_=false;
                    return CallNextHookEx(nullptr,code,message,data);
                }
                UINT key{}; bool down{};
                switch (message) {
                case WM_LBUTTONDOWN: key=VK_LBUTTON; down=true; break;
                case WM_LBUTTONUP: key=VK_LBUTTON; break;
                case WM_RBUTTONDOWN: key=VK_RBUTTON; down=true; break;
                case WM_RBUTTONUP: key=VK_RBUTTON; break;
                case WM_MBUTTONDOWN: key=VK_MBUTTON; down=true; break;
                case WM_MBUTTONUP: key=VK_MBUTTON; break;
                case WM_XBUTTONDOWN: down=true; [[fallthrough]];
                case WM_XBUTTONUP: key=HIWORD(event.mouseData)==XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
                }
                if (key && key == self->options_.trigger) {
                    self->held_ = down;
                    // Consume only the configured physical trigger while enabled.
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
public:
    ~Engine() { stop(); }
    bool running() const { return running_; }
    bool failed() const { return failed_; }
    void stop() { stop_=true; if(worker_.joinable())worker_.join(); running_=false; }
    bool start(Options options, HWND excluded = nullptr) {
        if(!valid(options))return false;
        stop(); options_=options; excluded_=excluded; stop_=false; failed_=false;
        std::promise<bool> initialized; auto ready=initialized.get_future();
        try { worker_=std::thread([this, init=std::move(initialized)]() mutable {
            current_=this; held_=false;
            auto hook=SetWindowsHookExW(WH_MOUSE_LL,mouse,GetModuleHandleW(nullptr),0);
            running_=hook!=nullptr; init.set_value(hook!=nullptr);
            if(!hook) { current_=nullptr; return; }
            std::mt19937 random(static_cast<unsigned>(GetTickCount64())); std::uniform_real_distribution<double> noise(-3,3);std::uniform_int_distribution<size_t> select(0,samples.size()-1);
            auto next=std::chrono::steady_clock::now();
            HWND foreground=GetForegroundWindow();
            bool pressed=false;INPUT releaseInput{};auto releaseAt=next;
            while(!stop_) {
                MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                auto active=GetForegroundWindow();
                if(!active || active!=foreground) { held_=false; foreground=active; }
                auto now=std::chrono::steady_clock::now();
                if(pressed && (!held_ || now>=releaseAt)) {
                    if(SendInput(1,&releaseInput,sizeof(INPUT))!=1){failed_=true;break;}
                    pressed=false;
                }
                if(held_ && !pressed && now>=next) {
                    auto period=sampleInterval(options_,select(random),noise(random));
                    auto layout=GetKeyboardLayout(GetWindowThreadProcessId(foreground,nullptr));
                    auto down=inputPacket(options_.target,false,layout);releaseInput=inputPacket(options_.target,true,layout);
                    if((down.type==INPUT_KEYBOARD&&!down.ki.wScan)||SendInput(1,&down,sizeof(INPUT))!=1){failed_=true;break;}
                    pressed=true;releaseAt=now+std::chrono::microseconds(static_cast<long long>(pressDuration(period,random())*1000));
                    next=now+std::chrono::microseconds(static_cast<long long>(period*1000));
                }else if(!held_)next=now;
                MsgWaitForMultipleObjectsEx(0,nullptr,2,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            }
            if(pressed && SendInput(1,&releaseInput,sizeof(INPUT))!=1)failed_=true;
            UnhookWindowsHookEx(hook); held_=false; current_=nullptr; running_=false;
        }); } catch(...) { failed_=true; return false; }
        return ready.get();
    }
};
}
