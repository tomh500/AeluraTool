#include <windows.h>
#include "AccountClient.h"
#include <winhttp.h>
#include <wincred.h>
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#pragma comment(lib,"Winhttp.lib")
#pragma comment(lib,"Advapi32.lib")
namespace aelura::account {
using namespace winrt;
using namespace Windows::Data::Json;
namespace {
struct Handle { HINTERNET value{}; ~Handle(){if(value)WinHttpCloseHandle(value);} operator HINTERNET()const{return value;} };
std::wstring target(std::wstring const& endpoint){return L"AeluraTool/Account/"+endpoint;}
}
bool opaque(std::string const& value){return !value.empty()&&value.size()<=128&&std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});}
std::wstring normalizeEndpoint(std::wstring const& value){
    try{
        if(value.size()>180)throw Error{"account.invalid_endpoint"};
        Windows::Foundation::Uri uri(value);
        std::wstring scheme(uri.SchemeName()),host(uri.Host());
        std::transform(scheme.begin(),scheme.end(),scheme.begin(),::towlower);
        std::transform(host.begin(),host.end(),host.begin(),::towlower);
        const bool local=host==L"localhost"||host==L"127.0.0.1"||host==L"[::1]"||host==L"::1";
        if((scheme!=L"https"&&!(scheme==L"http"&&local))||host.empty()||
            !uri.UserName().empty()||!uri.Password().empty()||!uri.Query().empty()||!uri.Fragment().empty()||
            (uri.Path()!=L"/"&&!uri.Path().empty()))throw Error{"account.invalid_endpoint"};
        auto result=std::wstring(uri.AbsoluteUri());while(!result.empty()&&result.back()==L'/')result.pop_back();return result;
    }catch(Error const&){throw;}catch(...){throw Error{"account.invalid_endpoint"};}
}
bool authorizationUrl(std::wstring const& value,std::string const& provider){
    try{Windows::Foundation::Uri uri(value);return uri.SchemeName()==L"https"&&uri.UserName().empty()&&uri.Password().empty()&&
        uri.Port()==443&&((provider=="qq"&&uri.Host()==L"graph.qq.com"&&uri.Path()==L"/oauth2.0/authorize")||(provider=="google"&&uri.Host()==L"accounts.google.com"&&uri.Path()==L"/o/oauth2/v2/auth")||(provider=="microsoft"&&uri.Host()==L"login.microsoftonline.com"&&uri.Path()==L"/consumers/oauth2/v2.0/authorize"));}catch(...){return false;}
}
void validateProfile(JsonObject const& value){
    try{
        auto id=std::wstring(value.GetNamedString(L"user_id"));
        auto provider=value.GetNamedString(L"session_provider");
        if(id.size()!=32||!std::all_of(id.begin(),id.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');})||(provider!=L"qq"&&provider!=L"google"&&provider!=L"microsoft"))throw Error{"account.network_error"};
        (void)value.GetNamedBoolean(L"qq_linked");(void)value.GetNamedBoolean(L"google_linked");(void)value.GetNamedBoolean(L"service_allowed");
        if(provider==L"microsoft"||value.HasKey(L"microsoft_linked"))(void)value.GetNamedBoolean(L"microsoft_linked");
    }catch(...){throw Error{"account.network_error"};}
}
JsonObject request(std::wstring const& endpoint,wchar_t const* method,std::wstring const& path,std::string const& bearer,std::string const& session){
    if((!bearer.empty()&&!opaque(bearer))||(!session.empty()&&!opaque(session)))throw Error{"account.session_expired"};
    auto base=normalizeEndpoint(endpoint);Windows::Foundation::Uri uri(base);
    Handle agent{WinHttpOpen(L"AeluraTool/Account",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!agent)throw Error{"account.network_error"};
    WinHttpSetTimeouts(agent,8000,8000,8000,8000);
    Handle connection{WinHttpConnect(agent,uri.Host().c_str(),static_cast<INTERNET_PORT>(uri.Port()),0)};
    Handle operation{WinHttpOpenRequest(connection,method,path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,uri.SchemeName()==L"https"?WINHTTP_FLAG_SECURE:0)};
    if(!operation)throw Error{"account.network_error"};
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(operation,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    std::wstring headers=L"Accept: application/json\r\n";
    if(!bearer.empty()){headers+=L"Authorization: Bearer ";headers+=to_hstring(bearer).c_str();headers+=L"\r\n";}
    if(!session.empty()){headers+=L"X-Account-Session: ";headers+=to_hstring(session).c_str();headers+=L"\r\n";}
    if(!WinHttpSendRequest(operation,headers.c_str(),static_cast<DWORD>(headers.size()),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(operation,nullptr))throw Error{"account.network_error"};
    DWORD status{},size=sizeof(status);if(!WinHttpQueryHeaders(operation,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))throw Error{"account.network_error"};
    std::string bytes;DWORD available{},read{};
    for(;;){
        if(!WinHttpQueryDataAvailable(operation,&available))throw Error{"account.network_error"};
        if(!available)break;
        if(available>65536||bytes.size()+available>65536)throw Error{"account.network_error"};
        auto start=bytes.size();bytes.resize(start+available);
        if(!WinHttpReadData(operation,bytes.data()+start,available,&read))throw Error{"account.network_error"};bytes.resize(start+read);
    }
    JsonObject result;
    if(!bytes.empty()&&!JsonObject::TryParse(to_hstring(bytes),result))throw Error{"account.network_error"};
    if(status<200||status>=300){
        auto detail=to_string(result.GetNamedString(L"detail",L""));
        static std::string const allowed[]={"account.provider_unavailable","account.service_unavailable","account.session_expired","account.qq_reauth_required","account.qq_required","account.disabled","account.access_denied","account.flow_expired","account.identity_conflict","account.busy","account.sign_in_required"};
        for(auto const& key:allowed)if(detail==key)throw Error{key};
        throw Error{status==429?"account.busy":"account.network_error"};
    }
    return result;
}
std::string loadCredential(std::wstring const& endpoint){
    PCREDENTIALW credential{};auto name=target(endpoint);
    if(!CredReadW(name.c_str(),CRED_TYPE_GENERIC,0,&credential))return {};
    std::string token;if(credential->CredentialBlobSize<=128)token.assign(reinterpret_cast<char const*>(credential->CredentialBlob),credential->CredentialBlobSize);CredFree(credential);
    return opaque(token)?token:std::string{};
}
void saveCredential(std::wstring const& endpoint,std::string const& token){
    if(!opaque(token))throw Error{"account.login_failed"};auto name=target(endpoint);
    CREDENTIALW credential{};credential.Type=CRED_TYPE_GENERIC;credential.TargetName=name.data();credential.UserName=const_cast<wchar_t*>(L"AeluraTool");
    credential.CredentialBlobSize=static_cast<DWORD>(token.size());credential.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));credential.Persist=CRED_PERSIST_LOCAL_MACHINE;
    if(!CredWriteW(&credential,0))throw Error{"account.credentials_failed"};
}
void forgetCredential(std::wstring const& endpoint){auto name=target(endpoint);if(!CredDeleteW(name.c_str(),CRED_TYPE_GENERIC,0)&&GetLastError()!=ERROR_NOT_FOUND)throw Error{"account.credentials_failed"};}
}
