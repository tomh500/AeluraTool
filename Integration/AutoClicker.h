#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <future>
#include <random>
#include <chrono>

namespace aelura::autoclicker {
struct Options { UINT trigger{VK_XBUTTON1}, target{VK_LBUTTON}; int rate{10}, jitter{10}; };
inline bool valid(Options const& o) {
    auto mouse = [](UINT k) { return k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON || k == VK_XBUTTON1 || k == VK_XBUTTON2; };
    return mouse(o.trigger) && (mouse(o.target) || (o.target >= 'A' && o.target <= 'Z') || (o.target >= '0' && o.target <= '9') || (o.target >= VK_F1 && o.target <= VK_F24)) && o.rate >= 1 && o.rate <= 100 && o.jitter >= 0 && o.jitter <= 90;
}
inline double interval(Options const& o, double random) { return 1000.0 / o.rate * (1.0 + o.jitter / 100.0 * random); }
class Engine {
    std::thread worker_;
    std::atomic<bool> stop_{false}, running_{false}, failed_{false};
    Options options_;
    bool held_{};
    inline static thread_local Engine* current_{};
    static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) {
        auto self = current_;
        if (code == HC_ACTION && self) {
            auto const& event = *reinterpret_cast<MSLLHOOKSTRUCT*>(data);
            if (!(event.flags & LLMHF_INJECTED)) {
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
    bool click() {
        INPUT input[2]{}; auto key=options_.target;
        if (key==VK_LBUTTON || key==VK_RBUTTON || key==VK_MBUTTON || key==VK_XBUTTON1 || key==VK_XBUTTON2) {
            input[0].type=input[1].type=INPUT_MOUSE;
            input[0].mi.dwFlags=key==VK_LBUTTON?MOUSEEVENTF_LEFTDOWN:key==VK_RBUTTON?MOUSEEVENTF_RIGHTDOWN:key==VK_MBUTTON?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_XDOWN;
            input[1].mi.dwFlags=key==VK_LBUTTON?MOUSEEVENTF_LEFTUP:key==VK_RBUTTON?MOUSEEVENTF_RIGHTUP:key==VK_MBUTTON?MOUSEEVENTF_MIDDLEUP:MOUSEEVENTF_XUP;
            input[0].mi.mouseData=input[1].mi.mouseData=key==VK_XBUTTON1?XBUTTON1:key==VK_XBUTTON2?XBUTTON2:0;
        } else {
            input[0].type=input[1].type=INPUT_KEYBOARD;
            input[0].ki.wVk=input[1].ki.wVk=static_cast<WORD>(key); input[1].ki.dwFlags=KEYEVENTF_KEYUP;
        }
        auto sent=SendInput(2,input,sizeof(INPUT));
        if(sent==1) SendInput(1,&input[1],sizeof(INPUT));
        return sent==2;
    }
public:
    ~Engine() { stop(); }
    bool running() const { return running_; }
    bool failed() const { return failed_; }
    void stop() { stop_=true; if(worker_.joinable())worker_.join(); running_=false; }
    bool start(Options options) {
        if(!valid(options))return false;
        stop(); options_=options; stop_=false; failed_=false;
        std::promise<bool> initialized; auto ready=initialized.get_future();
        try { worker_=std::thread([this, init=std::move(initialized)]() mutable {
            current_=this; held_=false;
            auto hook=SetWindowsHookExW(WH_MOUSE_LL,mouse,GetModuleHandleW(nullptr),0);
            running_=hook!=nullptr; init.set_value(hook!=nullptr);
            if(!hook) { current_=nullptr; return; }
            std::mt19937 random(static_cast<unsigned>(GetTickCount64())); std::uniform_real_distribution<double> jitter(-1,1);
            auto next=std::chrono::steady_clock::now();
            HWND foreground=GetForegroundWindow();
            while(!stop_) {
                MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                if(GetAsyncKeyState(VK_ESCAPE)&0x8000)break;
                auto active=GetForegroundWindow();
                if(!active || active!=foreground) { held_=false; foreground=active; }
                auto now=std::chrono::steady_clock::now();
                if(held_ && now>=next) {
                    if(!click()) { failed_=true; break; }
                    next=now+std::chrono::microseconds(static_cast<long long>(interval(options_,jitter(random))*1000));
                } else if(!held_)next=now;
                MsgWaitForMultipleObjectsEx(0,nullptr,2,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            }
            UnhookWindowsHookEx(hook); held_=false; current_=nullptr; running_=false;
        }); } catch(...) { failed_=true; return false; }
        return ready.get();
    }
};
}
