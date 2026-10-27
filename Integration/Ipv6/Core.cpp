#include <winsock2.h>
#include "Core.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <stdexcept>
#pragma comment(lib,"ws2_32.lib")
namespace aelura::ipv6 {
namespace {
bool parse(std::wstring value, IN6_ADDR& address) { auto scope=value.find(L'%'); if(scope!=value.npos)value.resize(scope); return InetPtonW(AF_INET6,value.c_str(),&address)==1; }
std::wstring format(IN6_ADDR const& address) { wchar_t out[64]{}; InetNtopW(AF_INET6,&address,out,64); return out; }
}
Kind classify(std::wstring const& text) {
    IN6_ADDR a{}; if(!parse(text,a))return Kind::Invalid;
    if(IN6_IS_ADDR_UNSPECIFIED(&a))return Kind::Unspecified;
    if(IN6_IS_ADDR_LOOPBACK(&a))return Kind::Loopback;
    if(IN6_IS_ADDR_V4MAPPED(&a))return Kind::Mapped;
    if(IN6_IS_ADDR_LINKLOCAL(&a))return Kind::LinkLocal;
    if(IN6_IS_ADDR_MULTICAST(&a))return Kind::Multicast;
    if((a.u.Byte[0]&0xfe)==0xfc || IN6_IS_ADDR_SITELOCAL(&a))return Kind::Local;
    return (a.u.Byte[0]&0xe0)==0x20 ? Kind::Global : Kind::Local;
}
std::wstring calculate(std::wstring const& text) {
    auto slash=text.find(L'/'); auto literal=text.substr(0,slash); IN6_ADDR a{};
    if(literal.find(L'%')!=literal.npos || !parse(literal,a))return {};
    unsigned prefix=128;
    if(slash!=text.npos) {
        auto p=text.substr(slash+1); if(p.empty() || p.size()>3 || p.find_first_not_of(L"0123456789")!=p.npos)return {};
        prefix=static_cast<unsigned>(std::stoul(p)); if(prefix>128)return {};
    }
    auto first=a,last=a;
    for(unsigned bit=prefix;bit<128;++bit) { first.u.Byte[bit/8]&=static_cast<UCHAR>(~(1u<<(7-bit%8))); last.u.Byte[bit/8]|=static_cast<UCHAR>(1u<<(7-bit%8)); }
    return format(a)+L"\n"+format(first)+L"/"+std::to_wstring(prefix)+L"\n"+format(last);
}
int choose(std::vector<Adapter> const& list) {
    int best=-1;
    for(size_t i=0;i<list.size();++i) if(list[i].up && (best<0 ||
        (list[i].route && !list[best].route) || (list[i].route==list[best].route && list[i].metric<list[best].metric)))best=static_cast<int>(i);
    return best<0 && !list.empty()?0:best;
}
bool operator==(Config const& a,Config const& b) { return a.present==b.present && (!a.present || a.value==b.value); }
Config enabled(Config c) { c.value&=~0x10u; return c; }
bool Work::stopped() const { return cancel || std::chrono::steady_clock::now()>=deadline; }
Conclusion conclude(Adapter const& a, Config c,std::vector<Check> const& checks,bool cancelled) {
    if(cancelled)return Conclusion::Cancelled;
    bool microsoft=false,cloudflare=false;for(auto const& check:checks)if(check.state==State::Pass) {if(check.key=="ipv6.microsoft")microsoft=true;if(check.key=="ipv6.cloudflare")cloudflare=true;}
    if(microsoft||cloudflare)return microsoft&&cloudflare?Conclusion::Verified:Conclusion::Partial;
    if(a.binding==0 || (c.value&0x10))return Conclusion::Configuration;
    if(!a.up || !a.global || !a.route)return Conclusion::NoNetwork;
    return Conclusion::Unknown;
}
char const* conclusionKey(Conclusion c) {
    switch(c) {case Conclusion::Verified:return "ipv6.verified";case Conclusion::Partial:return "ipv6.partial";case Conclusion::Configuration:return "ipv6.configuration";case Conclusion::NoNetwork:return "ipv6.no_network";case Conclusion::Cancelled:return "ipv6.cancelled";default:return "ipv6.unknown";}
}
Report diagnose(Adapter const& a,Work& work,Probe const& test) {
    Report r; r.adapter=a;r.config=configuration();
    auto execute=test?test:Probe(probe);
    if(!test && !work.stopped()) {r.checks.push_back(dnsProbe(work));++work.progress;}
    for(bool microsoft:{true,false}) {
        if(work.stopped())r.checks.push_back({microsoft?"ipv6.microsoft":"ipv6.cloudflare",work.cancel?State::Cancelled:State::Unknown,{}});
        else {r.checks.push_back(execute(microsoft,a,work));work.progress+=1;}
    }
    r.conclusion=conclude(a,r.config,r.checks,work.cancel);return r;
}
RepairResult transact(Store& s,Snapshot const& old,Snapshot const& next,bool restoring) {
    auto current=s.read();
    if(current.adapter!=old.adapter || current.binding!=old.binding || !(current.config==old.config))return RepairResult::Conflict;
    if(!restoring && !s.journal(old,next))return RepairResult::BackupFailed;
    bool bindingChanged=old.binding!=next.binding,configChanged=!(old.config==next.config);
    if((bindingChanged && !s.binding(next.binding)) || (configChanged && !s.config(next.config))) {
        bool rollback=s.config(old.config);rollback=s.binding(old.binding)&&rollback;
        if(rollback && !restoring)s.clear();
        return rollback?RepairResult::Failed:RepairResult::RollbackFailed;
    }
    if(restoring && !s.clear())return RepairResult::RollbackFailed;
    return configChanged?RepairResult::Restart:RepairResult::Success;
}
RepairResult recover(Store& s,Snapshot const& original,Snapshot const& intended) {
    auto current=s.read();
    // A process may have stopped between the binding and registry changes.
    // Accept only the recorded pre/post values of each individual setting.
    if(current.adapter!=original.adapter || intended.adapter!=original.adapter ||
        (current.binding!=original.binding && current.binding!=intended.binding) ||
        (!(current.config==original.config) && !(current.config==intended.config)))return RepairResult::Conflict;
    return transact(s,current,original,true);
}
}
