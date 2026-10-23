#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace aelura::recording::media {
// Compare sparse reference bars at several heights, respecting the driver's pitch.
inline bool nv12ProbeMatches(uint8_t const* actual,size_t pitch,uint8_t const* expected,unsigned w,unsigned h) {
    if(pitch<w)return false;
    for(unsigned band=0;band<6;++band)for(unsigned row:{h/4,h/2,3*h/4}) {
        unsigned x=((2*band+1)*w/12)&~1u;
        auto close=[](int a,int b){return a>=b-8&&a<=b+8;};
        if(!close(actual[size_t(row)*pitch+x],expected[size_t(row)*w+x]))return false;
        for(unsigned c=0;c<2;++c)if(!close(actual[size_t(h+row/2)*pitch+x+c],expected[size_t(h+row/2)*w+x+c]))return false;
    }
    return true;
}
// Top-down BGRA to tightly packed BT.601 limited-range NV12. Dimensions must be even.
inline std::vector<uint8_t> bgraToNv12(uint8_t const* bgra,unsigned width,unsigned height,size_t stride) {
    std::vector<uint8_t> nv12(size_t(width)*height*3/2);
    auto clip=[](int value){return static_cast<uint8_t>(std::clamp(value,0,255));};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        auto p=bgra+size_t(y)*stride+x*4;const int b=p[0],g=p[1],r=p[2];
        nv12[size_t(y)*width+x]=clip(((66*r+129*g+25*b+128)>>8)+16);
        if(!(x&1)&&!(y&1)) {
            int rr=0,gg=0,bb=0;
            for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){auto q=p+size_t(dy)*stride+dx*4;bb+=q[0];gg+=q[1];rr+=q[2];}
            auto uv=nv12.data()+size_t(width)*height+size_t(y/2)*width+x;
            uv[0]=clip(((-38*rr-74*gg+112*bb+512)>>10)+128);
            uv[1]=clip(((112*rr-94*gg-18*bb+512)>>10)+128);
        }
    }
    return nv12;
}
}
