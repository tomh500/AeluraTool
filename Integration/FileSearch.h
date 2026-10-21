#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace aelura::filesearch {
namespace ntfs { struct Journal; }
inline constexpr size_t ResultLimit = 200;
struct Options { bool path{}, matchCase{}, wholeWord{}, regex{}; };
struct Item { std::wstring name, path; bool folder{}; };
void preserveResultOrder(std::vector<Item> const& previous, std::vector<Item>& incoming);
struct Progress {
    uint64_t revision{}, indexed{}, skipped{};
    bool running{}, cancelled{}, failed{}, cached{}, cacheFailed{};
};
struct Entry { Item item; std::wstring foldedName; };
using Batch = std::vector<Entry>;
struct IndexSnapshot { Progress progress; std::vector<std::shared_ptr<Batch const>> batches; };
// Shared by the WinUI page and desktop input box. Only file metadata is indexed.
class Index {
public:
    explicit Index(std::filesystem::path cache, std::vector<std::filesystem::path> roots = {});
    ~Index();
    void refresh();
    void cancel();
    Progress progress() const;
    IndexSnapshot snapshot() const;
    bool everything() const { return everything_; }
    bool setEverything(bool enabled);
    void updatePaths(std::vector<std::filesystem::path> paths);
private:
    void run(); void build(); void update(std::vector<std::filesystem::path> const& paths, bool persist = true); bool load(); bool save();
    void publish(std::shared_ptr<Batch> const& batch, uint64_t skipped);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false}, cancel_{false};
    std::atomic<bool> everything_{false}, ledgerInvalid_{false};
    bool pending_{}, loaded_{}, restartBuiltin_{};
    std::filesystem::path cache_;
    std::vector<std::filesystem::path> roots_;
    std::vector<std::filesystem::path> changedPaths_;
    std::vector<std::shared_ptr<ntfs::Journal>> journals_; // Worker-owned metadata graphs.
    bool cacheDirty_{}; ULONGLONG lastCacheSave_{};
    IndexSnapshot data_;
    std::thread worker_;
};
enum class State { Idle, Searching, Ready, InvalidQuery, Failed, EngineUnavailable, TimedOut };
struct Snapshot {
    uint64_t generation{}, sourceRevision{}, total{};
    State state{State::Idle}; std::vector<Item> items;
};
Snapshot match(IndexSnapshot const& source, std::wstring const& query, Options options,
    std::function<bool()> const& cancelled = {});
class Session {
public:
    explicit Session(std::shared_ptr<Index> index);
    ~Session();
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    uint64_t search(std::wstring query, Options options = {});
    void cancel();
    Snapshot snapshot() const;
private:
    void run();
    std::shared_ptr<Index> index_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> generation_{0};
    std::wstring query_;
    Options options_;
    Snapshot result_;
    bool pending_{};
    std::thread worker_;
};
}
