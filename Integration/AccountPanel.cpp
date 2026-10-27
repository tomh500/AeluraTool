#include "pch.h"
#include "AccountPanel.h"
#include "AccountClient.h"
#include "I18n.h"
#include <winrt/Windows.System.h>
#include <chrono>
#include <winrt/Windows.UI.Text.h>
namespace aelura::account {
using namespace winrt;using namespace Microsoft::UI::Xaml;using namespace Controls;
using namespace Windows::Data::Json;using aelura::i18n::tr;
struct Panel::Impl:std::enable_shared_from_this<Impl>{
    std::filesystem::path settings;std::wstring endpoint;std::string token,ticket,secret;
    bool busy{},closed{},signedIn{},qqReady{},microsoftReady{};unsigned generation{};
    JsonObject profile;std::string message="account.signed_out";
    StackPanel root;TextBlock title,hint,state,identity,bindings,permission;TextBox address;
    Button save,qq,microsoft,link,logout,refreshButton,cancel;ProgressRing progress;
    explicit Impl(std::filesystem::path const& path):settings(path){
        wchar_t value[256]{};GetPrivateProfileStringW(L"Account",L"Service",L"",value,256,path.c_str());
        if(*value){try{endpoint=normalizeEndpoint(value);}catch(...){message="account.invalid_endpoint";}}
        else message="account.configure_service";
        if(!endpoint.empty())token=loadCredential(endpoint);root.Spacing(12);title.FontSize(18);title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        for(auto text:{hint,state,identity,bindings,permission})text.TextWrapping(TextWrapping::Wrap);
        root.Children().Append(title);root.Children().Append(hint);root.Children().Append(state);
        root.Children().Append(identity);root.Children().Append(bindings);root.Children().Append(permission);
        auto row=[&](std::initializer_list<UIElement> list){StackPanel line;line.Orientation(Orientation::Horizontal);line.Spacing(10);for(auto const& item:list)line.Children().Append(item);root.Children().Append(line);};
        progress.Width(22);progress.Height(22);row({qq,microsoft,link});row({logout,refreshButton,cancel,progress});
        address.Text(value);address.HorizontalAlignment(HorizontalAlignment::Stretch);root.Children().Append(address);root.Children().Append(save);
    }
    void attach(){
        auto weak=weak_from_this();qq.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->login("qq",false);});
        microsoft.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->login("microsoft",false);});
        link.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->login("microsoft",true);});
        logout.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->signout();});
        refreshButton.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->refresh();});
        cancel.Click([weak](auto const&,auto const&){if(auto self=weak.lock()){++self->generation;self->busy=false;self->message="account.cancelled";self->cancelFlow();self->render();}});
        save.Click([weak](auto const&,auto const&){if(auto self=weak.lock())self->saveEndpoint();});render();
    }
    void render(){
        if(closed)return;
        title.Text(tr("account.title"));hint.Text(tr(endpoint.empty()?"account.configure_hint":"account.hint"));state.Text(tr(message.c_str()));
        for(auto text:{identity,bindings,permission})text.Visibility(signedIn?Visibility::Visible:Visibility::Collapsed);
        link.Visibility(signedIn?Visibility::Visible:Visibility::Collapsed);
        logout.Visibility(token.empty()?Visibility::Collapsed:Visibility::Visible);
        qq.Content(box_value(tr(signedIn?"account.qq_reauth":"account.qq_login")));microsoft.Content(box_value(tr("account.microsoft_login")));
        link.Content(box_value(tr("account.microsoft_link")));logout.Content(box_value(tr("account.logout")));refreshButton.Content(box_value(tr("account.refresh")));
        cancel.Content(box_value(tr("common.cancel")));save.Content(box_value(tr("account.save_service")));address.Header(box_value(tr("account.service_address")));address.PlaceholderText(tr("account.service_placeholder"));
        qq.IsEnabled(!busy&&qqReady);microsoft.IsEnabled(!busy&&!signedIn&&microsoftReady);link.IsEnabled(!busy&&signedIn&&microsoftReady&&!profile.GetNamedBoolean(L"microsoft_linked",false));
        logout.IsEnabled(!busy&&!token.empty());refreshButton.IsEnabled(!busy&&!endpoint.empty());address.IsEnabled(!busy);save.IsEnabled(!busy);
        cancel.Visibility(busy?Visibility::Visible:Visibility::Collapsed);progress.IsActive(busy);progress.Visibility(busy?Visibility::Visible:Visibility::Collapsed);
        identity.Text(signedIn?hstring(tr("account.user_id"))+profile.GetNamedString(L"user_id",L""):L"");
        bindings.Text(signedIn?tr(profile.GetNamedBoolean(L"microsoft_linked",false)?"account.both_bound":"account.qq_bound"):L"");
        permission.Text(signedIn?tr(profile.GetNamedBoolean(L"service_allowed",false)?"account.service_allowed":"account.service_denied"):L"");
    }
    fire_and_forget cancelFlow(){
        auto self=shared_from_this();auto base=endpoint;auto t=std::exchange(ticket,{});auto s=std::exchange(secret,{});
        if(t.empty()||s.empty())co_return;co_await resume_background();try{request(base,L"DELETE",std::wstring(L"/v2/accounts/flow/")+to_hstring(t).c_str(),s);}catch(...){}
    }
    fire_and_forget refresh()try{
        auto self=shared_from_this();if(busy||closed)co_return;if(endpoint.empty()){message="account.configure_service";render();co_return;}busy=true;auto version=++generation;auto base=endpoint;auto saved=token;apartment_context ui;
        message="account.checking";render();JsonObject config,me;std::string issue;
        bool qqAvailable=false,microsoftAvailable=false;
        co_await resume_background();try{config=request(base,L"GET",L"/v2/accounts/config");qqAvailable=config.GetNamedBoolean(L"qq");microsoftAvailable=config.GetNamedBoolean(L"microsoft",false);if(!saved.empty()){me=request(base,L"GET",L"/v2/accounts/me",saved);validateProfile(me);}}catch(Error const& e){issue=e.key;}catch(...){issue="account.network_error";}
        co_await ui;if(closed||generation!=version)co_return;
        qqReady=qqAvailable;microsoftReady=microsoftAvailable;
        if(issue.empty()){profile=me;signedIn=!saved.empty();message=signedIn?"account.signed_in":qqReady||microsoftReady?"account.signed_out":"account.provider_unavailable";}
        else{message=issue;if(issue=="account.session_expired"){signedIn=false;profile=JsonObject();}}
        busy=false;render();
    }catch(...){}
    fire_and_forget login(std::string provider,bool binding)try{
        auto self=shared_from_this();if(busy||closed)co_return;busy=true;auto version=++generation;auto base=endpoint;auto existing=token;apartment_context ui;
        message="account.opening_browser";render();std::string issue;JsonObject started;bool finished=false;
        co_await resume_background();try{started=request(base,L"POST",binding?std::wstring(L"/v2/accounts/link/microsoft"):std::wstring(L"/v2/accounts/login/")+to_hstring(provider).c_str(),binding?existing:std::string{});}catch(Error const&e){issue=e.key;}catch(...){issue="account.network_error";}
        co_await ui;if(closed||generation!=version){
            try{auto t=to_string(started.GetNamedString(L"ticket",L""));auto s=to_string(started.GetNamedString(L"poll_secret",L""));if(opaque(t)&&opaque(s)){co_await resume_background();try{request(base,L"DELETE",std::wstring(L"/v2/accounts/flow/")+to_hstring(t).c_str(),s);}catch(...){}}}catch(...){}
            co_return;
        }
        if(issue.empty()){
            std::wstring url;
            try{ticket=to_string(started.GetNamedString(L"ticket"));secret=to_string(started.GetNamedString(L"poll_secret"));url=std::wstring(started.GetNamedString(L"authorization_url"));}catch(...){issue="account.login_failed";}
            if(!opaque(ticket)||!opaque(secret)||!authorizationUrl(url,provider))issue="account.login_failed";
            else{
                try{if(!co_await Windows::System::Launcher::LaunchUriAsync(Windows::Foundation::Uri(url)))issue="account.browser_failed";}catch(...){issue="account.browser_failed";}
            }
        }
        auto t=ticket,s=secret;auto until=std::chrono::steady_clock::now()+std::chrono::minutes(10);
        if(issue.empty()){message="account.waiting_browser";render();}
        while(issue.empty()&&!finished&&std::chrono::steady_clock::now()<until){
            co_await resume_after(std::chrono::seconds(3));co_await ui;if(closed||generation!=version)co_return;
            JsonObject result,me;std::string newToken;hstring resultStatus;co_await resume_background();
            try{
                result=request(base,L"POST",std::wstring(L"/v2/accounts/poll/")+to_hstring(t).c_str(),s,binding?existing:std::string{});
                auto status=resultStatus=result.GetNamedString(L"status",L"");
                if(status==L"approved"){
                    newToken=to_string(result.GetNamedString(L"access_token",L""));me=request(base,L"GET",L"/v2/accounts/me",newToken);validateProfile(me);
                }else if(status==L"linked"){me=request(base,L"GET",L"/v2/accounts/me",existing);validateProfile(me);}
                else if(status==L"failed"||status==L"cancelled"||status==L"consumed"){
                    auto code=to_string(result.GetNamedString(L"error",L"account.login_failed"));
                    issue=(code=="account.qq_required"||code=="account.qq_reauth_required"||code=="account.identity_conflict"||code=="account.disabled")?code:"account.login_failed";
                }
            }catch(Error const&e){issue=e.key;}catch(...){issue="account.network_error";}
            co_await ui;
            if(closed||generation!=version){
                if(!newToken.empty()){co_await resume_background();try{request(base,L"POST",L"/v2/accounts/logout",newToken);}catch(...){}}
                co_return;
            }
            auto status=resultStatus;
            if(issue.empty()&&(status==L"approved"||status==L"linked")){
                try{if(!newToken.empty()){saveCredential(base,newToken);token=newToken;}profile=me;signedIn=true;message=binding?"account.linked":"account.signed_in";finished=true;}
                catch(Error const&e){issue=e.key;}
            }
            if(!issue.empty()&&!newToken.empty()){co_await resume_background();try{request(base,L"POST",L"/v2/accounts/logout",newToken);}catch(...){}co_await ui;}
        }
        if(closed||generation!=version)co_return;
        if(!finished){message=issue.empty()?"account.flow_expired":issue;cancelFlow();}else{ticket.clear();secret.clear();}
        busy=false;render();
    }catch(...){}
    fire_and_forget signout()try{
        auto self=shared_from_this();if(busy||closed)co_return;auto version=++generation;auto base=endpoint;auto saved=token;apartment_context ui;
        try{forgetCredential(base);}catch(Error const&e){message=e.key;render();co_return;}
        token.clear();signedIn=false;profile=JsonObject();busy=true;message="account.signing_out";render();std::string issue;
        co_await resume_background();try{request(base,L"POST",L"/v2/accounts/logout",saved);}catch(...){issue="account.local_signed_out";}
        co_await ui;if(closed||generation!=version)co_return;busy=false;message=issue.empty()?"account.signed_out":issue;render();
    }catch(...){}
    void saveEndpoint(){
        try{
            auto entered=std::wstring(address.Text());auto value=entered.empty()?std::wstring{}:normalizeEndpoint(entered);
            if(!WritePrivateProfileStringW(L"Account",L"Service",value.c_str(),settings.c_str()))throw Error{"account.settings_failed"};
            endpoint=value;address.Text(value);token=value.empty()?std::string{}:loadCredential(endpoint);signedIn=false;profile=JsonObject();qqReady=microsoftReady=false;refresh();
        }catch(Error const&e){message=e.key;render();}
    }
};
Panel::Panel(std::filesystem::path const& settings):impl_(std::make_shared<Impl>(settings)){impl_->attach();}
Panel::~Panel(){impl_->closed=true;++impl_->generation;impl_->cancelFlow();}
UIElement Panel::content()const{return impl_->root;}
void Panel::localize(){impl_->render();}
void Panel::refresh(){impl_->refresh();}
}
