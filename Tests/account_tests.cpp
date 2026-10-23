#include <windows.h>
#include "../Integration/AccountClient.h"
#include <iostream>
#include <stdexcept>
using namespace aelura::account;
void check(bool condition,char const* what){if(!condition)throw std::runtime_error(what);}
template<class F>void rejects(F fn){try{fn();}catch(Error const&){return;}throw std::runtime_error("unsafe input accepted");}
int main(int argc,char** argv){
    winrt::init_apartment();
    try{
        check(normalizeEndpoint(L"https://example.com/")==L"https://example.com","endpoint normalization");
        check(normalizeEndpoint(L"http://localhost:18741")==L"http://localhost:18741","localhost endpoint");
        for(auto const* value:{L"http://example.com",L"https://name:password@example.com",L"https://example.com/path",L"https://example.com/?token=bad",L"https://example.com/#fragment",L"file:///tmp/account"})rejects([&]{normalizeEndpoint(value);});
        check(opaque("Ab12_-"),"valid opaque token");
        check(!opaque("header\r\nInjected: bad")&&!opaque(std::string(129,'a'))&&!opaque(""),"token injection protection");
        check(authorizationUrl(L"https://accounts.google.com/o/oauth2/v2/auth?state=abc","google"),"Google authorization URL");
        check(authorizationUrl(L"https://graph.qq.com/oauth2.0/authorize?state=abc","qq"),"QQ authorization URL");
        check(!authorizationUrl(L"https://accounts.google.com.evil.example/o/oauth2/v2/auth","google"),"authorization host spoof");
        check(!authorizationUrl(L"http://graph.qq.com/oauth2.0/authorize","qq"),"authorization HTTPS");
        check(!authorizationUrl(L"https://accounts.google.com/other","google"),"authorization path");
        rejects([&]{validateProfile(winrt::Windows::Data::Json::JsonObject::Parse(L"{\"user_id\":123}"));});
        auto profile=winrt::Windows::Data::Json::JsonObject::Parse(L"{\"user_id\":\"0123456789abcdef0123456789abcdef\",\"session_provider\":\"qq\",\"qq_linked\":true,\"google_linked\":false,\"service_allowed\":false}");
        validateProfile(profile);profile.SetNamedValue(L"google_linked",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"true"));rejects([&]{validateProfile(profile);});
        GUID guid{};winrt::check_hresult(CoCreateGuid(&guid));wchar_t text[64]{};StringFromGUID2(guid,text,64);
        // Unique synthetic credential targets only; never read or change the user's real session.
        auto endpoint=std::wstring(L"https://aelura-credential-test.invalid/")+text;
        saveCredential(endpoint,"test_session_one");
        check(loadCredential(endpoint)=="test_session_one","credential round trip");
        rejects([&]{saveCredential(endpoint,"bad\nsecret");});
        check(loadCredential(endpoint)=="test_session_one","failed save preserves previous credential");
        check(loadCredential(endpoint+L"-other").empty(),"credential service isolation");
        saveCredential(endpoint,"test_session_two");check(loadCredential(endpoint)=="test_session_two","credential replacement");
        forgetCredential(endpoint);check(loadCredential(endpoint).empty(),"credential deletion");
        forgetCredential(endpoint);
        if(argc==2){
            auto service=std::wstring(winrt::to_hstring(argv[1]));
            auto config=request(service,L"GET",L"/v2/accounts/config");
            (void)config.GetNamedBoolean(L"qq");(void)config.GetNamedBoolean(L"google");
            try{request(service,L"GET",L"/v2/accounts/me");throw std::runtime_error("anonymous session accepted");}
            catch(Error const& error){check(error.key=="account.sign_in_required","server error mapping");}
            std::cout<<"PASS: real WinHTTP request over configured account service and anonymous request rejection\n";
        }
        std::cout<<"PASS: account endpoint/authorization validation, token injection rejection, isolated Windows credentials and failed-save preservation\n";
        return 0;
    }catch(std::exception const& e){std::cerr<<e.what()<<'\n';}catch(Error const& e){std::cerr<<e.key<<'\n';}catch(winrt::hresult_error const& e){std::cerr<<winrt::to_string(e.message())<<'\n';}
    return 1;
}
