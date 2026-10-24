#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
namespace aelura::ipv6 {
enum class Kind { Invalid, Unspecified, Loopback, LinkLocal, Local, Multicast, Global, Mapped };
Kind classify(std::wstring const&);
std::wstring calculate(std::wstring const&); // canonical address, network and last address
struct Address { std::wstring value; Kind kind{Kind::Invalid}; bool usable{}; };
struct Adapter {
    std::wstring id, name; ULONG index{}; bool up{}, route{}, global{}; int binding{-1};
    ULONG metric{~0ul}; std::vector<std::wstring> addresses, dns;
    std::vector<Address> addressInfo;
};
int choose(std::vector<Adapter> const&);
struct Config { bool present{}; DWORD value{}; };
bool operator==(Config const&, Config const&);
Config enabled(Config);
enum class State { Pass, Fail, Unknown, Cancelled };
struct Check { std::string key; State state{State::Unknown}; std::wstring detail; };
enum class Conclusion { Verified, Partial, Configuration, NoNetwork, Unknown, Cancelled };
Conclusion conclude(Adapter const&, Config, std::vector<Check> const&, bool cancelled);
char const* conclusionKey(Conclusion);
struct Work {
    std::atomic<bool> cancel{false}; std::atomic<unsigned> progress{0};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now()+std::chrono::seconds(20)};
    bool stopped() const;
};
struct Report { Adapter adapter; Config config; std::vector<Check> checks; Conclusion conclusion{Conclusion::Unknown}; };
std::vector<Adapter> adapters();
Config configuration();
using Probe = std::function<Check(bool, Adapter const&, Work&)>;
Report diagnose(Adapter const&, Work&, Probe const& = {});
enum class Operation { Dns, Ping, Trace, Tcp, Calculate };
std::vector<Check> advanced(Operation, std::wstring, unsigned port, Adapter const&, Work&);
Check probe(bool microsoft, Adapter const&, Work&);
Check dnsProbe(Work&);
// The injectable store permits transaction tests without changing the host network.
struct Snapshot { std::wstring adapter; bool binding{}; Config config; };
struct Store {
    virtual ~Store() = default;
    virtual Snapshot read() = 0;
    virtual bool journal(Snapshot const&, Snapshot const&) = 0;
    virtual bool binding(bool) = 0;
    virtual bool config(Config) = 0;
    virtual bool clear() = 0;
};
enum class RepairResult : DWORD { Success=0, Failed=1, BackupFailed=2, Conflict=3, RollbackFailed=4, Restart=5, Denied=6, NoRecord=7 };
RepairResult transact(Store&, Snapshot const&, Snapshot const&, bool restoring);
RepairResult recover(Store&, Snapshot const& original, Snapshot const& intended);
bool recoveryAvailable();
std::wstring recoveryAdapter();
RepairResult repair(std::wstring const& id, bool restore);
RepairResult launchRepair(HWND, std::wstring const&, bool restore);
bool helperLaunch(); // dispatch fixed helper arguments before app singleton
}
