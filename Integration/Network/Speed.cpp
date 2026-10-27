#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "Core.h"
#include <vector>
#include <algorithm>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"bcrypt.lib")
namespace aelura::network {
namespace {
struct Http {HINTERNET h{};~Http(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};
// Operations are issued and closed by one worker. Native callbacks only signal
// completion; the worker can close an asynchronous request when cancelled.
// HANDLE_CLOSING is the final callback, so all buffers/context stay alive until it.
struct Request {
    HINTERNET h{};HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr),closed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic_ulong error{},count{};bool bound{};
    explicit Request(HINTERNET handle):h(handle){}
    Request(Request const&)=delete;
    ~Request(){if(h){WinHttpCloseHandle(h);if(bound)WaitForSingleObject(closed,INFINITE);}if(done)CloseHandle(done);if(closed)CloseHandle(closed);}
    operator HINTERNET()const{return h;}
    static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD status,void* data,DWORD length) {
        if(!context)return;auto& self=*reinterpret_cast<Request*>(context);
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){SetEvent(self.closed);return;}
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){self.error=static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;SetEvent(self.done);}
        else if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE){self.count=length;SetEvent(self.done);}
        else if(status==WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE){self.count=*static_cast<DWORD*>(data);SetEvent(self.done);}
        else if(status==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE||status==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)SetEvent(self.done);
    }
    bool attach() {
        if(!h||!done||!closed)return false;
        if(WinHttpSetStatusCallback(h,callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)return false;
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(this);
        bound=WinHttpSetOption(h,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))!=FALSE;return bound;
    }
    void prepare(){ResetEvent(done);error=0;count=0;}
    bool wait(Work& work) {
        while(!work.stopped()) {
            auto status=WaitForSingleObject(done,30);
            if(status==WAIT_OBJECT_0){if(error){SetLastError(error);return false;}return true;}
            if(status==WAIT_FAILED)return false;
        }
        SetLastError(work.cancel?ERROR_WINHTTP_OPERATION_CANCELLED:ERROR_WINHTTP_TIMEOUT);return false;
    }
};
}
Result transfer(int family,bool upload,uint64_t amount,Work& work,Endpoint const& endpoint) {
    Result out;out.family=family;
    if((family!=4&&family!=6)||amount<1||amount>100000000)return out;
    auto lookup=dns(endpoint.host,family,work);if(lookup.result.state!=State::Success)return lookup.result;
    out.address=lookup.records.front().address;
    Http session{WinHttpOpen(L"AeluraTool/1.0",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,WINHTTP_FLAG_ASYNC)};if(!session.h){out.error=GetLastError();return out;}
    if(!WinHttpSetTimeouts(session,3000,3000,3000,3000)){out.error=GetLastError();return out;}
    auto host=endpoint.host.find(L':')!=std::wstring::npos?L"["+endpoint.host+L"]":endpoint.host;
    Http connection{WinHttpConnect(session,host.c_str(),endpoint.port,0)};if(!connection.h){out.error=GetLastError();return out;}
    auto path=(upload?endpoint.up:endpoint.down)+L"?bytes="+std::to_wstring(amount)+L"&aelura="+std::to_wstring(GetTickCount64());
    // Buffer must outlive request teardown, including a pending ReadData/WriteData.
    std::vector<unsigned char> buffer(65536);
    Request request{WinHttpOpenRequest(connection,upload?L"POST":L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,endpoint.secure?WINHTTP_FLAG_SECURE:0)};
    if(!request.attach()){out.error=GetLastError();return out;}
    // Override DNS resolution only: Host, SNI and certificate validation retain endpoint.host.
    // A numeric override prevents fallback to another address family.
    auto resolution=family==6?L"["+out.address+L"]":out.address;
    if(!WinHttpSetOption(request,WINHTTP_OPTION_RESOLUTION_HOSTNAME,resolution.data(),static_cast<DWORD>((resolution.size()+1)*sizeof(wchar_t)))){out.error=GetLastError();out.state=State::Unsupported;return out;}
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    if(!WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled))){out.error=GetLastError();return out;}
    if(upload && BCryptGenRandom(nullptr,buffer.data(),static_cast<ULONG>(buffer.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return out;
    auto failed=[&](){out.error=GetLastError();out.state=work.stopped()?(work.cancel?State::Cancelled:State::Timeout):out.error==ERROR_WINHTTP_TIMEOUT?State::Timeout:State::Failed;return out;};
    auto start=std::chrono::steady_clock::now();
    if(work.stopped())return failed();
    request.prepare();
    if(!WinHttpSendRequest(request,L"Cache-Control: no-cache\r\nAccept-Encoding: identity\r\nContent-Type: application/octet-stream\r\n",static_cast<DWORD>(-1),nullptr,0,upload?static_cast<DWORD>(amount):0,reinterpret_cast<DWORD_PTR>(&request))||!request.wait(work))return failed();
    if(upload) {
        while(out.bytes<amount && !work.stopped()) {
            auto n=static_cast<DWORD>((std::min)(amount-out.bytes,static_cast<uint64_t>(buffer.size())));
            request.prepare();if(!WinHttpWriteData(request,buffer.data(),n,nullptr)||!request.wait(work)||!request.count)return failed();auto sent=request.count.load();out.bytes+=sent;work.bytes+=sent;
        }
        if(work.stopped())return failed();
    }
    request.prepare();if(!WinHttpReceiveResponse(request,nullptr)||!request.wait(work))return failed();
    DWORD code{},size=sizeof(code);
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX))return failed();
    if(code!=200){out.error=code;return out;}
    WINHTTP_CONNECTION_INFO info{};info.cbSize=sizeof(info);size=sizeof(info);
    if(!WinHttpQueryOption(request,WINHTTP_OPTION_CONNECTION_INFO,&info,&size))return failed();
    if(info.RemoteAddress.ss_family!=(family==4?AF_INET:AF_INET6)){out.state=State::Unsupported;return out;}
    if(family==6 && IN6_IS_ADDR_V4MAPPED(&reinterpret_cast<sockaddr_in6*>(&info.RemoteAddress)->sin6_addr)){out.state=State::Unsupported;return out;}
    // Both directions use total HTTP transaction time (DNS excluded), including
    // TCP/TLS and server acknowledgement. Never report socket-buffer speed.
    uint64_t responseBytes{};
    while(!work.stopped()) {
        request.prepare();if(!WinHttpReadData(request,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr)||!request.wait(work))return failed();auto read=request.count.load();if(!read)break;
        responseBytes+=read;
        if(!upload){out.bytes+=read;work.bytes+=read;}
        if(responseBytes>(upload?1048576:amount)){out.state=State::Failed;return out;}
    }
    if(work.stopped())return failed();
    if(!upload && out.bytes!=amount)return out;
    out.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    out.mbps=megabits(out.bytes,out.milliseconds/1000);out.state=State::Success;return out;
}
}
