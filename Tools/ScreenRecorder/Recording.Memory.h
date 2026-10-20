#pragma once
#include "Recording.h"
#include <mfidl.h>
#include <objidl.h>
#include <wrl/client.h>
#include <atomic>
namespace aelura::recording::media {
// Quota counts allocated 64 KiB payload blocks across the rolling buffer and
// any export snapshots. It does not count encoder/Media Foundation working RAM.
struct MemoryBudget {
    explicit MemoryBudget(uint64_t maximum):limit(maximum){}
    uint64_t const limit;
    std::atomic<uint64_t> used{};
    std::function<void(uint64_t)> reclaim;
    bool acquire(uint64_t);
    void release(uint64_t bytes){used.fetch_sub(bytes);}
};
struct MemoryStorage;
class MemoryFile {
public:
    explicit MemoryFile(std::shared_ptr<MemoryBudget>);
    Microsoft::WRL::ComPtr<IStream> stream() const;
    Microsoft::WRL::ComPtr<IMFByteStream> byteStream() const;
    uint64_t size() const;
    uint64_t allocated() const;
    void seal();
private:
    std::shared_ptr<MemoryStorage> storage_;
};
void joinMemory(std::vector<std::shared_ptr<MemoryFile>> const&,
    std::filesystem::path const&,std::function<bool(double)> = {});
}
