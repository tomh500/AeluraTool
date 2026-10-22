#include "../Integration/AutoClicker.h"
#include <stdexcept>
#include <iostream>
#include <cmath>
void run_autoclicker_tests() {
    using namespace aelura::autoclicker;
    auto require=[](bool value){if(!value)throw std::runtime_error("AutoClicker validation or timing failed");};
    Options o; require(valid(o));
    for(auto trigger:{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2}) {
        o.trigger=trigger;o.target=trigger;require(valid(o));
        for(auto rate:{1,10,100})for(auto variation:{0,10,90}) {
            o.rate=rate;o.jitter=variation;require(valid(o));
            const double baseline=1000.0/rate;
            require(std::abs(interval(o,-1)-baseline*(1-variation/100.0))<1e-8);
            require(std::abs(interval(o,1)-baseline*(1+variation/100.0))<1e-8);
            require(interval(o,-1)>0 && std::abs(interval(o,0)-baseline)<1e-8);
        }
    }
    o=Options{};o.rate=0;require(!valid(o));o.rate=101;require(!valid(o));
    o=Options{};o.jitter=-1;require(!valid(o));o.jitter=91;require(!valid(o));
    o=Options{};o.trigger='A';require(!valid(o));o=Options{};o.target=VK_SHIFT;require(!valid(o));
    o.target='A';require(valid(o));o.target=VK_F24;require(valid(o));
    o=Options{};
    for(size_t i=0;i<samples.size();++i)for(double noise:{-3.0,0.0,3.0}) {
        auto period=sampleInterval(o,i,noise);
        require(period>=interval(o,-1)&&period<=interval(o,1));
        require(pressDuration(period,0)>0 && pressDuration(period,9)<period);
        o.jitter=0;require(sampleInterval(o,i,noise)==1000.0/o.rate);o.jitter=10;
    }
    auto keyboardDown=inputPacket('K',false,GetKeyboardLayout(0)),keyboardUp=inputPacket('K',true,GetKeyboardLayout(0));
    require(keyboardDown.type==INPUT_KEYBOARD&&keyboardDown.ki.wVk==0&&keyboardDown.ki.wScan!=0);
    require(keyboardDown.ki.dwFlags==KEYEVENTF_SCANCODE&&keyboardUp.ki.dwFlags==(KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP));
    require(keyboardDown.ki.wScan==keyboardUp.ki.wScan);
    auto mouseDown=inputPacket(VK_XBUTTON2,false,nullptr),mouseUp=inputPacket(VK_XBUTTON2,true,nullptr);
    require(mouseDown.type==INPUT_MOUSE&&mouseDown.mi.mouseData==XBUTTON2&&mouseDown.mi.dwFlags==MOUSEEVENTF_XDOWN&&mouseUp.mi.dwFlags==MOUSEEVENTF_XUP);
    require(pressDuration(100,0)==15&&pressDuration(100,9)==24&&pressDuration(10,9)==4.5);
    Engine engine;require(!engine.running());o.rate=0;require(!engine.start(o));require(!engine.running());engine.stop();engine.stop();
    std::cout<<"PASS: AutoClicker trigger/target validation, rate/jitter bounds, interval extrema and disabled lifecycle (no input emitted)\n";
}
