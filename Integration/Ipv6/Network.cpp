#include <winsock2.h>
#include "Core.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#pragma comment(lib,"iphlpapi.lib")
namespace aelura::ipv6 {
int bindingState(std::wstring const&);
namespace {
struct Winsock { bool ok{}; Winsock(){WSADATA d{};ok=WSAStartup(MAKEWORD(2,2),&d)==0;} ~Winsock(){if(ok)WSACleanup();} };
struct Socket { SOCKET s{INVALID_SOCKET}; ~Socket(){if(s!=INVALID_SOCKET)closesocket(s);} };
std::wstring address(sockaddr const* a) {
    if(!a || a->sa_family!=AF_INET6)return {};
    wchar_t out[96]{};DWORD n=96;
    auto copy=*reinterpret_cast<sockaddr_in6 const*>(a);copy.sin6_port=0;
    return WSAAddressToStringW(reinterpret_cast<sockaddr*>(&copy),sizeof(copy),nullptr,out,&n)==0?out:L"";
}
std::chrono::steady_clock::time_point endFor(Work const& w) { return (std::min)(w.deadline,std::chrono::steady_clock::now()+std::chrono::seconds(5)); }
bool ready(SOCKET s,bool write,Work const& w,std::chrono::steady_clock::time_point end) {
    while(!w.stopped() && std::chrono::steady_clock::now()<end) {
        fd_set set;FD_ZERO(&set);FD_SET(s,&set);timeval t{0,50000};
        auto result=select(0,write?nullptr:&set,write?&set:nullptr,nullptr,&t);
        if(result>0)return true;if(result==SOCKET_ERROR)return false;
    }return false;
}
std::vector<sockaddr_in6> resolve(std::wstring host,Work& w,std::chrono::steady_clock::time_point end) {
    std::vector<sockaddr_in6> result;
    if(host.size()>2 && host.front()==L'[' && host.back()==L']')host=host.substr(1,host.size()-2);
    ADDRINFOEXW hints{};hints.ai_family=AF_INET6;hints.ai_socktype=SOCK_STREAM;
    PADDRINFOEXW list{};OVERLAPPED ov{};ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ov.hEvent)return result;
    HANDLE cancel{};auto status=GetAddrInfoExW(host.c_str(),nullptr,NS_DNS,nullptr,&hints,&list,nullptr,&ov,nullptr,&cancel);
    if(status==WSA_IO_PENDING) {
        while(!w.stopped() && std::chrono::steady_clock::now()<end && WaitForSingleObject(ov.hEvent,30)==WAIT_TIMEOUT){}
        if(WaitForSingleObject(ov.hEvent,0)!=WAIT_OBJECT_0) { GetAddrInfoExCancel(&cancel); WaitForSingleObject(ov.hEvent,INFINITE); }
        status=GetAddrInfoExOverlappedResult(&ov);
    }
    if(status==0 && !w.stopped() && std::chrono::steady_clock::now()<end)
        for(auto p=list;p;p=p->ai_next) if(p->ai_family==AF_INET6 && p->ai_addrlen>=sizeof(sockaddr_in6) && !IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr))result.push_back(*reinterpret_cast<sockaddr_in6*>(p->ai_addr));
    if(list)FreeAddrInfoExW(list);CloseHandle(ov.hEvent);return result;
}
bool connect6(Socket& socket,sockaddr_in6 target,unsigned port,Adapter const& a,Work& w,std::chrono::steady_clock::time_point end) {
    if(w.stopped())return false;
    socket.s=::socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP);if(socket.s==INVALID_SOCKET)return false;
    DWORD only=1;if(setsockopt(socket.s,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<char*>(&only),sizeof(only)))return false;
    DWORD index=a.index;if(!a.index || setsockopt(socket.s,IPPROTO_IPV6,IPV6_UNICAST_IF,reinterpret_cast<char*>(&index),sizeof(index)))return false;
    u_long nonblock=1;ioctlsocket(socket.s,FIONBIO,&nonblock);target.sin6_port=htons(static_cast<u_short>(port));
    auto code=connect(socket.s,reinterpret_cast<sockaddr*>(&target),sizeof(target));
    if(code && WSAGetLastError()!=WSAEWOULDBLOCK)return false;
    if(code && !ready(socket.s,true,w,end))return false;
    int error{};int n=sizeof(error);return getsockopt(socket.s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&n)==0 && !error;
}
}
Config configuration() {
    Config c;DWORD size=sizeof(c.value),type{};
    auto e=RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters",L"DisabledComponents",RRF_RT_REG_DWORD,&type,&c.value,&size);
    if(e==ERROR_SUCCESS)c.present=true;
    else if(e!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("IPv6 configuration read failed");
    return c;
}
std::vector<Adapter> adapters() {
    Winsock winsock;std::vector<Adapter> result;ULONG size=16384;std::vector<unsigned char> buffer(size);
    ULONG status{};for(unsigned i=0;i<3;++i) { status=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_PREFIX|GAA_FLAG_INCLUDE_GATEWAYS,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&size);if(status!=ERROR_BUFFER_OVERFLOW)break;buffer.resize(size); }
    if(status!=NO_ERROR) return result;
    PMIB_IPFORWARD_TABLE2 routes{};GetIpForwardTable2(AF_INET6,&routes);
    for(auto p=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());p;p=p->Next) {
        if(p->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        Adapter a;a.name=p->FriendlyName?p->FriendlyName:L"";a.index=p->Ipv6IfIndex?p->Ipv6IfIndex:p->IfIndex;a.up=p->OperStatus==IfOperStatusUp;
        if(p->AdapterName)for(auto ch= p->AdapterName;*ch;++ch)a.id+=static_cast<wchar_t>(*ch);
        a.binding=bindingState(a.id);
        for(auto u=p->FirstUnicastAddress;u;u=u->Next) {
            auto literal=address(u->Address.lpSockaddr);if(literal.empty())continue;
            a.addresses.push_back(literal+L"/"+std::to_wstring(u->OnLinkPrefixLength));
            bool usable=(u->DadState==IpDadStatePreferred || u->DadState==IpDadStateDeprecated) && u->ValidLifetime;
            auto kind=classify(literal);a.addressInfo.push_back({a.addresses.back(),kind,usable});
            if(kind==Kind::Global && usable)a.global=true;
        }
        for(auto d=p->FirstDnsServerAddress;d;d=d->Next) { auto literal=address(d->Address.lpSockaddr);if(!literal.empty())a.dns.push_back(literal); }
        if(routes) for(ULONG i=0;i<routes->NumEntries;++i) {auto const& r=routes->Table[i];if(r.InterfaceIndex==a.index && r.DestinationPrefix.PrefixLength==0 && r.ValidLifetime) {a.route=true;a.metric=(std::min)(a.metric,static_cast<ULONG>((std::min)(0xfffffffeull,static_cast<unsigned long long>(p->Ipv6Metric)+r.Metric)));}}
        if(!a.route)a.metric=p->Ipv6Metric;
        result.push_back(std::move(a));
    }
    if(routes)FreeMibTable(routes);return result;
}
Check dnsProbe(Work& w) {
    Winsock winsock;auto targets=resolve(L"ipv6.msftconnecttest.com",w,endFor(w));
    Check c{"ipv6.dns",w.cancel?State::Cancelled:targets.empty()?State::Fail:State::Pass,{}};
    for(auto const& t:targets)c.detail+=address(reinterpret_cast<sockaddr const*>(&t))+L" ";return c;
}
Check probe(bool microsoft,Adapter const& a,Work& w) {
    Winsock winsock;auto end=endFor(w);Check c{microsoft?"ipv6.microsoft":"ipv6.cloudflare",State::Fail};
    auto targets=resolve(microsoft?L"ipv6.msftconnecttest.com":L"2606:4700:4700::1111",w,end);
    if(targets.empty()) {c.detail=L"DNS";return c;}
    for(auto target:targets) {
        if(w.stopped())break;Socket socket;
        if(!connect6(socket,target,microsoft?80:53,a,w,end))continue;
        sockaddr_in6 local{};int n=sizeof(local);getsockname(socket.s,reinterpret_cast<sockaddr*>(&local),&n);
        c.detail=a.name+L" (#"+std::to_wstring(a.index)+L")";
        if(!microsoft) {c.state=State::Pass;break;}
        const std::string request="GET /connecttest.txt HTTP/1.1\r\nHost: ipv6.msftconnecttest.com\r\nConnection: close\r\n\r\n";
        size_t sent=0;while(sent<request.size() && ready(socket.s,true,w,end)) {int bytes=send(socket.s,request.data()+sent,static_cast<int>(request.size()-sent),0);if(bytes<=0)break;sent+=bytes;}
        std::string response;char data[1024];while(response.size()<8192 && ready(socket.s,false,w,end)) {int bytes=recv(socket.s,data,sizeof(data),0);if(bytes<=0)break;response.append(data,bytes);}
        auto body=response.find("\r\n\r\n");
        if(response.find("HTTP/1.1 200 ")==0 || response.find("HTTP/1.0 200 ")==0) if(body!=response.npos && response.substr(body+4)=="Microsoft Connect Test") {c.state=State::Pass;break;}
    }
    if(w.cancel)c.state=State::Cancelled;return c;
}
std::vector<Check> advanced(Operation op,std::wstring target,unsigned port,Adapter const& a,Work& w) {
    if(op==Operation::Calculate) {auto value=calculate(target);return {{"ipv6.calculation",value.empty()?State::Fail:State::Pass,value}};}
    Winsock winsock;auto end=endFor(w);auto targets=resolve(target,w,end);std::vector<Check> result;
    if(targets.empty())return {{"ipv6.dns",w.cancel?State::Cancelled:State::Fail,{}}};
    if(op==Operation::Dns) { for(auto const& t:targets)result.push_back({"ipv6.dns",State::Pass,address(reinterpret_cast<sockaddr const*>(&t))});return result; }
    if(op==Operation::Tcp) {
        bool success=false;for(auto t:targets) {Socket socket;if(connect6(socket,t,port,a,w,end)) {success=true;break;}}
        return {{"ipv6.tcp",w.cancel?State::Cancelled:success?State::Pass:State::Fail,a.name+L" (#"+std::to_wstring(a.index)+L")"}};
    }
    auto handle=Icmp6CreateFile();if(handle==INVALID_HANDLE_VALUE)return {{"ipv6.ping",State::Unknown,{}}};
    sockaddr_in6 source{};source.sin6_family=AF_INET6;auto destination=targets.front();
    SOCKADDR_INET dest{},best{};dest.Ipv6=destination;MIB_IPFORWARD_ROW2 route{};
    if(GetBestRoute2(nullptr,a.index,nullptr,&dest,0,&route,&best)!=NO_ERROR) {IcmpCloseHandle(handle);return {{"ipv6.route",State::Fail,{}}};}source=best.Ipv6;
    for(unsigned ttl=1;ttl<=(op==Operation::Trace?16u:1u) && !w.stopped();++ttl) {
        std::array<unsigned char,2048> reply{};IP_OPTION_INFORMATION options{};options.Ttl=static_cast<UCHAR>(op==Operation::Trace?ttl:64);char payload[]="AeluraIPv6";
        auto until=op==Operation::Trace?w.deadline:end;
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now()).count();if(remaining<=0)break;
        auto timeout=static_cast<DWORD>((std::min)(remaining,static_cast<long long>(op==Operation::Trace?1000:3000)));
        auto count=Icmp6SendEcho2(handle,nullptr,nullptr,nullptr,&source,&destination,payload,sizeof(payload),&options,reply.data(),static_cast<DWORD>(reply.size()),timeout);
        auto r=reinterpret_cast<ICMPV6_ECHO_REPLY*>(reply.data());bool success=count && r->Status==IP_SUCCESS;
        sockaddr_in6 hop{};hop.sin6_family=AF_INET6;memcpy(&hop.sin6_addr,r->Address.sin6_addr,16);hop.sin6_scope_id=r->Address.sin6_scope_id;
        result.push_back({op==Operation::Trace?"ipv6.hop":"ipv6.ping",success?State::Pass:State::Unknown,std::to_wstring(ttl)+L" · "+(count?address(reinterpret_cast<sockaddr*>(&hop))+L" · "+std::to_wstring(r->RoundTripTime)+L" ms":L"—")});
        w.progress=ttl;if(success || op==Operation::Ping)break;
    }
    IcmpCloseHandle(handle);return result;
}
}
