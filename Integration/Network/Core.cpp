#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#include "Core.h"
#include <algorithm>
#include <memory>
#include <cwctype>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"dnsapi.lib")
namespace aelura::network {
bool validHost(std::wstring const& s) {
    if(s.empty() || s.size()>253)return false;
    for(auto c:s)if(!(iswalnum(c)||c==L'.'||c==L'-'||c==L':'||c==L'%'||c==L'_'))return false;
    return true;
}
char const* stateKey(State s) { switch(s) {
case State::Success:return "net.success";case State::NoRecords:return "net.no_records";case State::DnsFailure:return "net.dns_failed";
case State::Refused:return "net.refused";case State::Unreachable:return "net.unreachable";case State::Timeout:return "net.timeout";
case State::Cancelled:return "net.cancelled";case State::Unsupported:return "net.unsupported";default:return "net.failed";}}
double megabits(uint64_t bytes,double seconds) {return seconds>0?bytes*8.0/seconds/1000000.0:0;}
Lookup dns(std::wstring const& host,int family,Work& work) {
    Lookup out;out.result.family=family;
    if(!validHost(host) || (family!=4 && family!=6))return out;
    if(work.stopped()){out.result.state=work.cancel?State::Cancelled:State::Timeout;return out;}
    IN_ADDR v4{};IN6_ADDR v6{};
    if(InetPtonW(AF_INET,host.c_str(),&v4)==1 || InetPtonW(AF_INET6,host.c_str(),&v6)==1) {
        bool is4=InetPtonW(AF_INET,host.c_str(),&v4)==1;
        if((family==4)==is4 && (is4 || !IN6_IS_ADDR_V4MAPPED(&v6))){out.records.push_back({host,0,family});out.result.state=State::Success;}
        else out.result.state=State::NoRecords;
        return out;
    }
    auto start=std::chrono::steady_clock::now();
    DNS_QUERY_REQUEST request{};request.Version=DNS_QUERY_REQUEST_VERSION1;request.QueryName=host.c_str();
    request.QueryType=family==4?DNS_TYPE_A:DNS_TYPE_AAAA;request.QueryOptions=DNS_QUERY_STANDARD;
    DNS_QUERY_RESULT result{};result.Version=DNS_QUERY_RESULTS_VERSION1;
    struct Context { HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Context(){if(done)CloseHandle(done);} } context;
    if(!context.done)return out;
    request.pQueryContext=&context;
    request.pQueryCompletionCallback=[](void* p,DNS_QUERY_RESULT*) {SetEvent(static_cast<Context*>(p)->done);};
    DNS_QUERY_CANCEL cancel{};
    auto status=DnsQueryEx(&request,&result,&cancel);
    if(status==DNS_REQUEST_PENDING) {
        while(WaitForSingleObject(context.done,30)==WAIT_TIMEOUT && !work.stopped()){}
        if(WaitForSingleObject(context.done,0)!=WAIT_OBJECT_0){DnsCancelQuery(&cancel);WaitForSingleObject(context.done,INFINITE);}
        status=result.QueryStatus;
    }
    out.result.error=status;out.result.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    for(auto p=result.pQueryRecords;p;p=p->pNext) {
        wchar_t value[INET6_ADDRSTRLEN]{};
        if(p->wType==DNS_TYPE_A && family==4 && InetNtopW(AF_INET,&p->Data.A.IpAddress,value,INET6_ADDRSTRLEN))out.records.push_back({value,p->dwTtl,4});
        if(p->wType==DNS_TYPE_AAAA && family==6 && InetNtopW(AF_INET6,&p->Data.AAAA.Ip6Address,value,INET6_ADDRSTRLEN))out.records.push_back({value,p->dwTtl,6});
    }
    if(result.pQueryRecords)DnsRecordListFree(result.pQueryRecords,DnsFreeRecordList);
    out.result.state=work.stopped()?(work.cancel?State::Cancelled:State::Timeout):status==DNS_ERROR_RCODE_NAME_ERROR||status==DNS_INFO_NO_RECORDS?State::NoRecords:status?State::DnsFailure:out.records.empty()?State::NoRecords:State::Success;
    return out;
}
Result tcp(std::wstring const& host,unsigned port,int family,Work& work) {
    Result out;out.family=family;if(!validHost(host)||!port||port>65535||(family!=4&&family!=6))return out;
    auto lookup=dns(host,family,work);if(lookup.result.state!=State::Success)return lookup.result;
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return out;
    struct Cleanup {~Cleanup(){WSACleanup();}} cleanup;
    auto end=(std::min)(work.deadline,std::chrono::steady_clock::now()+std::chrono::seconds(5));
    for(auto const& record:lookup.records) {
        if(work.stopped()||std::chrono::steady_clock::now()>=end)break;
        out.address=record.address;auto af=family==4?AF_INET:AF_INET6;
        SOCKET socket=::socket(af,SOCK_STREAM,IPPROTO_TCP);if(socket==INVALID_SOCKET){out.error=WSAGetLastError();continue;}
        struct Close {SOCKET s;~Close(){closesocket(s);}} close{socket};
        u_long mode=1;ioctlsocket(socket,FIONBIO,&mode);
        sockaddr_storage storage{};int size{};
        if(family==4){auto& a=reinterpret_cast<sockaddr_in&>(storage);a.sin_family=AF_INET;a.sin_port=htons(static_cast<u_short>(port));InetPtonW(AF_INET,record.address.c_str(),&a.sin_addr);size=sizeof(a);}
        else {auto& a=reinterpret_cast<sockaddr_in6&>(storage);a.sin6_family=AF_INET6;a.sin6_port=htons(static_cast<u_short>(port));InetPtonW(AF_INET6,record.address.c_str(),&a.sin6_addr);size=sizeof(a);DWORD only=1;setsockopt(socket,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<char*>(&only),sizeof(only));}
        auto start=std::chrono::steady_clock::now();int status=connect(socket,reinterpret_cast<sockaddr*>(&storage),size);int error=status?WSAGetLastError():0;
        if(status && error==WSAEWOULDBLOCK) {
            error=WSAETIMEDOUT;
            while(!work.stopped() && std::chrono::steady_clock::now()<end) {
                fd_set write,except;FD_ZERO(&write);FD_ZERO(&except);FD_SET(socket,&write);FD_SET(socket,&except);timeval timeout{0,30000};
                int ready=select(0,nullptr,&write,&except,&timeout);
                if(ready>0){int len=sizeof(error);if(getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len))error=WSAGetLastError();break;}
                if(ready<0){error=WSAGetLastError();break;}
            }
        }
        out.error=error;out.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        out.state=!error?State::Success:error==WSAECONNREFUSED?State::Refused:error==WSAETIMEDOUT?State::Timeout:State::Unreachable;
        if(!error)return out;
    }
    if(work.stopped())out.state=work.cancel?State::Cancelled:State::Timeout;
    return out;
}
}
