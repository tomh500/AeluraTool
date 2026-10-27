#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "../Integration/Ipv6/Core.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace {
using namespace aelura::ipv6;
void require(bool value,char const* reason) {if(!value)throw std::runtime_error(reason);}
struct Fake:Store {
    Snapshot current{L"adapter",false,{true,0x31}},old,next;bool backup=true,failBinding=false,failConfigOnce=false,failRollback=false,record=false;unsigned mutations{};
    Snapshot read()override{return current;}
    bool journal(Snapshot const& a,Snapshot const& b)override{if(!backup)return false;old=a;next=b;record=true;return true;}
    bool binding(bool value)override{++mutations;if(failBinding || (failRollback && !value))return false;current.binding=value;return true;}
    bool config(Config value)override{++mutations;if(failConfigOnce){failConfigOnce=false;return false;}current.config=value;return true;}
    bool clear()override{record=false;return true;}
};
}
void run_ipv6_tests(std::filesystem::path const&) {
    using namespace aelura::ipv6;
    WSADATA data{};require(!WSAStartup(MAKEWORD(2,2),&data),"Winsock init");
    require(classify(L"::")==Kind::Unspecified,"unspecified classification");
    require(classify(L"::1")==Kind::Loopback,"loopback classification");
    require(classify(L"fe80::123%3")==Kind::LinkLocal,"scoped link local");
    require(classify(L"fd00::1")==Kind::Local,"ULA classification");
    require(classify(L"ff02::1")==Kind::Multicast,"multicast classification");
    require(classify(L"::ffff:127.0.0.1")==Kind::Mapped,"IPv4 mapped classification");
    require(classify(L"2606:4700:4700::1111")==Kind::Global,"global classification");
    require(classify(L"127.0.0.1")==Kind::Invalid,"IPv4 rejected");
    require(calculate(L"2001:0db8:0000::1/64")==L"2001:db8::1\n2001:db8::/64\n2001:db8::ffff:ffff:ffff:ffff","CIDR /64");
    require(calculate(L"::/0")==L"::\n::/0\nffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff","CIDR /0");
    require(calculate(L"::1/128")==L"::1\n::1/128\n::1","CIDR /128");
    require(calculate(L"::2/127")==L"::2\n::2/127\n::3","CIDR /127");
    for(auto text:{L"::/129",L"::/-1",L"::/",L"::/64x",L"::/64/1",L"nonsense",L"fe80::1%3/64"})require(calculate(text).empty(),"invalid CIDR rejected");
    Adapter off,up,vpn;up.up=true;up.metric=20;vpn.up=true;vpn.route=true;vpn.metric=50;
    require(choose({off,up,vpn})==2,"default route preferred");up.route=true;require(choose({off,up,vpn})==1,"route metric preferred");require(choose({})==-1,"empty adapter list");
    Adapter a;a.up=a.route=a.global=true;a.binding=1;
    std::vector<Check> probes{{"ipv6.microsoft",State::Pass,{}},{"ipv6.cloudflare",State::Pass,{}}};
    require(conclude(a,{},probes,false)==Conclusion::Verified,"all probes pass");probes[1].state=State::Fail;
    require(conclude(a,{},probes,false)==Conclusion::Partial,"one failure must not reject IPv6");probes[0].state=State::Fail;probes[0].detail=L"DNS";
    require(conclude(a,{},{{"ipv6.microsoft",State::Pass,{}}},false)==Conclusion::Partial,"a skipped probe cannot produce full verification");
    require(conclude(a,{},probes,false)==Conclusion::Unknown,"DNS and probe failures not definitive");a.route=false;
    require(conclude(a,{},probes,false)==Conclusion::NoNetwork,"missing route");a.binding=0;
    require(conclude(a,{},probes,false)==Conclusion::Configuration,"disabled binding");a.binding=1;
    require(conclude(a,{true,0xff},probes,false)==Conclusion::Configuration,"system disabled");
    require(conclude(a,{},probes,true)==Conclusion::Cancelled,"cancelled result");
    Work cancelled;cancelled.cancel=true;require(cancelled.stopped(),"cancel signal");
    Work expired;expired.deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);require(expired.stopped(),"deadline");
    unsigned calls=0;auto skipped=diagnose(a,cancelled,[&](bool,Adapter const&,Work&){++calls;return Check{};});require(!calls && skipped.conclusion==Conclusion::Cancelled,"no probes after cancellation");
    require(enabled({true,0xff}).value==0xef,"only non-tunnel disable bit cleared");require(enabled({true,0x20}).value==0x20,"IPv4 preference preserved");require(!enabled({}).present,"absent registry value preserved");
    Fake s;auto old=s.current,next=old;next.binding=true;next.config=enabled(old.config);
    require(transact(s,old,next,false)==RepairResult::Restart && s.record && s.current.binding && s.current.config.value==0x21,"repair journal and restart");
    require(transact(s,next,old,true)==RepairResult::Restart && !s.record && !s.current.binding && s.current.config==old.config,"restore originals");
    Fake noBackup;noBackup.backup=false;require(transact(noBackup,old,next,false)==RepairResult::BackupFailed && !noBackup.mutations,"backup fails before mutation");
    Fake partial;partial.failConfigOnce=true;require(transact(partial,old,next,false)==RepairResult::Failed && !partial.current.binding && partial.current.config==old.config && !partial.record,"partial failure rolled back");
    Fake broken;broken.failConfigOnce=broken.failRollback=true;require(transact(broken,old,next,false)==RepairResult::RollbackFailed && broken.record,"rollback failure retains record");
    Fake conflict;conflict.current.config.value=0x40;require(transact(conflict,old,next,true)==RepairResult::Conflict && !conflict.mutations,"restore detects external changes");
    Fake interrupted;interrupted.current.binding=true;interrupted.record=true;require(recover(interrupted,old,next)==RepairResult::Success && !interrupted.current.binding && !interrupted.record,"recover interruption between binding and registry");
    Fake interruptedRegistry;interruptedRegistry.current=next;interruptedRegistry.current.binding=false;interruptedRegistry.record=true;require(recover(interruptedRegistry,old,next)==RepairResult::Restart && !interruptedRegistry.record && interruptedRegistry.current.config==old.config,"recover partial rollback");
    require(recover(conflict,old,next)==RepairResult::Conflict && !conflict.mutations,"recovery must not overwrite unrelated configuration");
    Fake bindingFailure;bindingFailure.failBinding=true;require(transact(bindingFailure,old,next,false)==RepairResult::RollbackFailed && bindingFailure.record,"binding failure retained");
    // Real sockets, local-only: prove forced IPv6 connection and IPv4 rejection.
    SOCKET server=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP);sockaddr_in6 local{};local.sin6_family=AF_INET6;local.sin6_addr=in6addr_loopback;
    if(server!=INVALID_SOCKET && bind(server,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0 && listen(server,1)==0) {
        int bytes=sizeof(local);getsockname(server,reinterpret_cast<sockaddr*>(&local),&bytes);DWORD index{};
        require(GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&local),&index)==NO_ERROR,"loopback interface");Adapter loop;loop.index=index;loop.name=L"loopback";
        Work work;auto check=advanced(Operation::Tcp,L"::1",ntohs(local.sin6_port),loop,work);require(check.size()==1 && check[0].state==State::Pass,"native IPv6 TCP loopback");
        Work v4;check=advanced(Operation::Tcp,L"127.0.0.1",ntohs(local.sin6_port),loop,v4);require(check[0].state!=State::Pass,"IPv4 literal cannot pass");
        Work mapped;check=advanced(Operation::Tcp,L"::ffff:127.0.0.1",ntohs(local.sin6_port),loop,mapped);require(check[0].state!=State::Pass,"IPv4 mapped cannot pass");
        Work timeout;timeout.deadline=std::chrono::steady_clock::now();check=advanced(Operation::Tcp,L"::1",ntohs(local.sin6_port),loop,timeout);require(check[0].state!=State::Pass,"expired request cannot pass");
    } else std::cout<<"LIMIT: IPv6 loopback is unavailable\n";
    if(server!=INVALID_SOCKET)closesocket(server);WSACleanup();
    std::cout<<"PASS: IPv6 classification, CIDR, diagnosis, transactions and local IPv6-only sockets\n";
}
