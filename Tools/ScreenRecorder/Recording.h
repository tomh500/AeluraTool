#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace aelura::recording {
enum class Codec { H264, HEVC };
enum class Encoder { CPU, GPU };
enum class Scaling { Nearest, Linear, Cubic, Fant };
enum class State { Idle, Starting, Recording, Paused, Stopping, Completed, Failed };
struct Options {
    unsigned fps{30}, bitrate{12000000}, scale{100}, replaySeconds{300};
    Codec codec{Codec::H264}; Encoder encoder{Encoder::CPU}; Scaling scaling{Scaling::Cubic};
    bool microphone{}, cursor{true}, performanceBeta{};
    std::filesystem::path directory;
};
struct Snapshot {
    State state{State::Idle}; bool replay{}, saving{};double saveProgress{};
    unsigned width{}, height{}; double seconds{}, bufferedSeconds{};
    uint64_t frames{}, dropped{}; HRESULT error{S_OK};
    std::filesystem::path output, recovery;
    unsigned savedGeneration{};
    int optimizedVendor{};bool cuda{};HRESULT optimizationError{S_OK};std::wstring adapter;
};
Options load(std::filesystem::path const& file);
void save(std::filesystem::path const& file, Options const& options);
void validate(Options const&, RECT region);
RECT selectionRect(POINT a, POINT b, RECT desktop);
unsigned outputExtent(unsigned pixels, unsigned scale);
// Segment starts are independent keyframes. Keep only complete segments that fit.
size_t replayFirst(std::vector<double> const& durations, double limit);
// Lossless remux of complete, independently decodable MP4 segments.
void join(std::vector<std::filesystem::path> const&, std::filesystem::path const& output,std::function<bool(double)> progress = {});
class Session {
public:
    Session(); ~Session();
    Session(Session const&) = delete; Session& operator=(Session const&) = delete;
    bool start(Options const&, RECT, bool replay);
    void pause(); void stop(); bool saveReplay();
    void cancelSave(); bool recover(std::filesystem::path const& cache,std::filesystem::path const& directory);
    Snapshot snapshot() const;
    bool busy() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
