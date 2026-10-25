#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
namespace aelura::network {
enum class State { Success, NoRecords, DnsFailure, Refused, Unreachable, Timeout, Cancelled, Failed, Unsupported };
struct Work {
    std::atomic_bool cancel{};
    std::atomic_uint64_t bytes{};
    std::atomic_int stage{};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now()+std::chrono::seconds(20);
    bool stopped() const { return cancel || std::chrono::steady_clock::now()>=deadline; }
};
struct Result {
    State state=State::Failed;
    int family{};
    unsigned long error{};
    std::wstring address;
    double milliseconds{}, mbps{};
    uint64_t bytes{};
};
struct Record { std::wstring address; unsigned long ttl{}; int family{}; };
struct Lookup { Result result; std::vector<Record> records; };
bool validHost(std::wstring const&);
char const* stateKey(State);
double megabits(uint64_t bytes,double seconds);
Lookup dns(std::wstring const&,int family,Work&);
Result tcp(std::wstring const&,unsigned port,int family,Work&);
// Endpoint parameters also allow deterministic loopback HTTP integration tests.
struct Endpoint { std::wstring host=L"speed.cloudflare.com"; unsigned short port=443; bool secure=true; std::wstring down=L"/__down",up=L"/__up"; };
Result transfer(int family,bool upload,uint64_t bytes,Work&,Endpoint const& = {});
}
