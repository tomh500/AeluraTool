#pragma once
#include "Recording.Media.h"
#include <span>
namespace aelura::recording::media {
void mixPcm(std::span<int16_t const>,std::span<int16_t const>,std::span<int16_t>);
struct AudioMix : AudioFormat {
    AudioMix(bool system,bool microphone);~AudioMix();
    std::vector<int16_t> take(size_t frames);
    void discard();
    HRESULT systemError() const;HRESULT microphoneError() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
