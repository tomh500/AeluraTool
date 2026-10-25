#include <winsock2.h>
#include <ws2tcpip.h>
#include "../Integration/Network/Core.h"
#include <thread>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <cmath>
namespace {
using namespace aelura::network;
void check(bool b,char const* message){if(!b)throw std::runtime_error(message);}
struct Server {
    SOCKET listener=INVALID_SOCKET;unsigned short port{};std::thread worker;std::atomic_bool stop{};
    std::atomic_uint64_t received{};bool ok{};
    Server(int family,int code=200,bool shortBody=false,bool stall=false) {
        auto af=family==4?AF_INET:AF_INET6;listener=socket(af,SOCK_STREAM,IPPROTO_TCP);check(listener!=INVALID_SOCKET,"fixture socket");
        DWORD only=1;if(family==6)setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<char*>(&only),sizeof(only));
        sockaddr_storage address{};int length=family==4?sizeof(sockaddr_in):sizeof(sockaddr_in6);
        if(family==4){auto& a=reinterpret_cast<sockaddr_in&>(address);a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);}else{auto& a=reinterpret_cast<sockaddr_in6&>(address);a.sin6_family=AF_INET6;a.sin6_addr=in6addr_loopback;}
        check(bind(listener,reinterpret_cast<sockaddr*>(&address),length)==0,"fixture loopback bind");check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0,"fixture port");port=ntohs(family==4?reinterpret_cast<sockaddr_in&>(address).sin_port:reinterpret_cast<sockaddr_in6&>(address).sin6_port);check(listen(listener,2)==0,"fixture listen");
        worker=std::thread([this,code,shortBody,stall]{
            while(!stop){fd_set set;FD_ZERO(&set);FD_SET(listener,&set);timeval wait{0,30000};if(select(0,&set,nullptr,nullptr,&wait)<=0)continue;SOCKET s=accept(listener,nullptr,nullptr);if(s==INVALID_SOCKET)continue;
                DWORD timeout=2000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
                std::string input;char buffer[65536]{};size_t split=std::string::npos;
                while(input.size()<65536 && (split=input.find("\r\n\r\n"))==std::string::npos){int n=recv(s,buffer,sizeof(buffer),0);if(n<=0)break;input.append(buffer,n);}
                if(split==std::string::npos){closesocket(s);continue;}
                bool upload=input.rfind("POST ",0)==0;size_t amount=1000000;
                if(upload){auto p=input.find("Content-Length: ");if(p!=std::string::npos)amount=std::stoull(input.substr(p+16));received=input.size()-split-4;
                    while(received<amount){int n=recv(s,buffer,static_cast<int>((std::min)(amount-received.load(),static_cast<size_t>(sizeof(buffer)))),0);if(n<=0)break;received+=n;}}
                if(stall){while(!stop)std::this_thread::sleep_for(std::chrono::milliseconds(20));closesocket(s);continue;}
                size_t body=upload?0:amount;auto header="HTTP/1.1 "+std::to_string(code)+" Test\r\nContent-Length: "+std::to_string(body)+"\r\nConnection: close\r\n\r\n";
                send(s,header.data(),static_cast<int>(header.size()),0);
                size_t sent=0;if(shortBody)body/=2;while(sent<body&&!stop){int n=send(s,buffer,static_cast<int>((std::min)(body-sent,sizeof(buffer))),0);if(n<=0)break;sent+=n;}
                ok=upload?received==amount:sent==body;closesocket(s);
            }
        });
    }
    ~Server(){stop=true;if(worker.joinable())worker.join();closesocket(listener);}
};
Endpoint local(Server const& s,int family){Endpoint e;e.host=family==4?L"127.0.0.1":L"::1";e.port=s.port;e.secure=false;return e;}
}
void run_network_tests() {
    using namespace aelura::network;WSADATA data{};check(!WSAStartup(MAKEWORD(2,2),&data),"network winsock startup");
    struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
    check(validHost(L"example.com")&&validHost(L"::1")&&!validHost(L"https://example.com")&&!validHost(L"a b")&&!validHost(L"x\r\ny"),"host validation");
    check(std::abs(megabits(1000000,1)-8)<0.001&&megabits(1,0)==0,"Mbps units");
    for(int family:{4,6}) {
        Work w;auto literal=family==4?L"127.0.0.1":L"::1";
        auto d=dns(literal,family,w);check(d.result.state==State::Success&&d.records.size()==1,"literal DNS");
        check(dns(literal,family==4?6:4,w).result.state==State::NoRecords,"no cross-family DNS fallback");
        Server server(family);auto e=local(server,family);
        auto connection=tcp(literal,server.port,family,w);check(connection.state==State::Success&&connection.family==family,"loopback TCP connect");
        auto down=transfer(family,false,1000000,w,e);std::cout<<"download IPv"<<family<<" state="<<static_cast<int>(down.state)<<" error="<<down.error<<" bytes="<<down.bytes<<"\n";
        check(down.state==State::Success&&down.bytes==1000000&&down.mbps>0,"actual download and family verification");
        auto up=transfer(family,true,1000000,w,e);check(up.state==State::Success&&up.bytes==1000000&&server.received==1000000,"actual server-acknowledged upload");
        Work cancelled;cancelled.cancel=true;check(transfer(family,false,1000000,cancelled,e).state==State::Cancelled,"cancel before HTTP");check(tcp(literal,server.port,family,cancelled).state==State::Cancelled,"cancel before TCP");
        Work expired;expired.deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);check(dns(literal,family,expired).result.state==State::Timeout,"expired DNS deadline");
    }
    {Server s(4,302);Work w;check(transfer(4,false,1000000,w,local(s,4)).state==State::Failed,"redirect must not be followed");}
    {Server s(4,500);Work w;check(transfer(4,true,1000000,w,local(s,4)).state==State::Failed,"failed server response cannot yield upload speed");}
    {Server s(4,200,true);Work w;check(transfer(4,false,1000000,w,local(s,4)).state==State::Failed,"truncated download rejected");}
    {Server s(4,200,false,true);Work w;std::thread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(150));w.cancel=true;});auto start=std::chrono::steady_clock::now();auto r=transfer(4,false,1000000,w,local(s,4));cancel.join();check(r.state==State::Cancelled&&std::chrono::steady_clock::now()-start<std::chrono::seconds(5),"cancel blocked receive bounded");}
    {Server s(6,200,false,true);Work w;w.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);auto start=std::chrono::steady_clock::now();auto r=transfer(6,false,1000000,w,local(s,6));check(r.state==State::Timeout&&std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"HTTP overall deadline interrupts pending receive");}
    {unsigned short port{};{Server s(4);port=s.port;}Work w;check(tcp(L"127.0.0.1",port,4,w).state==State::Refused,"closed port refused");}
    {Work w;auto d=dns(L"localhost",4,w);check(d.result.state==State::Success&&!d.records.empty(),"DnsQueryEx synchronous or asynchronous path");}
    std::cout<<"PASS: dual-stack DNS/TCP, actual HTTP download/upload, redirects, HTTP failures, short body, timeout and cancellation\n";
}
void run_network_live_probe() {
    using namespace aelura::network;
    for(int f:{4,6})for(bool upload:{false,true}) {
        Work work;auto result=transfer(f,upload,1000000,work);
        std::wcout<<L"IPv"<<f<<(upload?L" upload":L" download")<<L" state="<<static_cast<int>(result.state)<<L" error="<<result.error<<L" address="<<result.address<<L" bytes="<<result.bytes<<L" ms="<<result.milliseconds<<L" Mbps="<<result.mbps<<std::endl;
    }
}
