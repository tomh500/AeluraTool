#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace aelura::hash {
enum class Mode { Calculate, Expected, Compare };
enum class Status { Success, Cancelled, InvalidExpected, InvalidPath, UnsupportedFile, OpenFailed, ReadFailed, Changed, CryptoFailed };
struct Request {
    Mode mode{Mode::Calculate};
    std::filesystem::path first, second;
    std::wstring expected;
};
struct Result {
    Status status{Status::Success};
    std::wstring first, second;
    uint64_t bytes{};
    uint32_t error{};
    bool matches{};
};
using Progress = std::function<bool(uint64_t, uint64_t)>;
bool normalizeExpected(std::wstring value, std::wstring& normalized);
Result compute(Request const& request, Progress const& progress);

// The session is owned by the UI thread. Only Work's atomics cross threads.
enum class State { Idle, Running, Cancelling, Finished };
struct Work {
    uint64_t generation{};
    Request request;
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0}, total{0};
};
class Session {
public:
    std::shared_ptr<Work> begin(Request request);
    void invalidate();
    void cancel();
    bool finish(std::shared_ptr<Work> const& work, Result result);
    bool busy() const { return state_ == State::Running || state_ == State::Cancelling; }
    State state() const { return state_; }
    Result const& result() const { return result_; }
    std::shared_ptr<Work> const& work() const { return work_; }
private:
    uint64_t generation_{};
    State state_{State::Idle};
    Result result_;
    std::shared_ptr<Work> work_;
};
}
