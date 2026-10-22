#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include <cmath>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
fire_and_forget MainWindow::ShowAutoClicker() {
    if(autoClickerDialog_)co_return;
    auto lifetime=get_strong(); autoClickerDialog_=true;
    if(autoClickerTimer_)autoClickerTimer_.Stop();
    autoClicker_.stop(); RestoreWindow();
    using aelura::i18n::tr;
    try {
        ContentDialog dialog; dialog.XamlRoot(Root().XamlRoot()); dialog.RequestedTheme(Root().RequestedTheme());
        dialog.Title(box_value(tr("autoclicker.title"))); dialog.PrimaryButtonText(tr("autoclicker.enable")); dialog.CloseButtonText(tr("autoclicker.keep_disabled"));
        StackPanel panel; panel.Spacing(12);
        TextBlock hint; hint.Text(tr("autoclicker.hint")); hint.TextWrapping(TextWrapping::Wrap); panel.Children().Append(hint);
        const std::vector<UINT> mouse{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
        std::vector<UINT> targets=mouse; for(UINT k='A';k<='Z';++k)targets.push_back(k); for(UINT k='0';k<='9';++k)targets.push_back(k); for(UINT k=VK_F1;k<=VK_F24;++k)targets.push_back(k);
        const char* names[]{"autoclicker.left","autoclicker.right","autoclicker.middle","autoclicker.side1","autoclicker.side2"};
        auto read=[&](wchar_t const* key,int fallback){return GetPrivateProfileIntW(L"AutoClicker",key,fallback,config_.c_str());};
        ComboBox trigger,target; trigger.Header(box_value(tr("autoclicker.trigger"))); target.Header(box_value(tr("autoclicker.target")));
        trigger.HorizontalAlignment(HorizontalAlignment::Stretch); target.HorizontalAlignment(HorizontalAlignment::Stretch);
        for(size_t i=0;i<targets.size();++i) {
            auto k=targets[i]; std::wstring label=i<5 ? tr(names[i]) : k>=VK_F1 ? L"F"+std::to_wstring(k-VK_F1+1) : std::wstring(1,static_cast<wchar_t>(k));
            target.Items().Append(box_value(label)); if(i<5)trigger.Items().Append(box_value(label));
        }
        trigger.SelectedIndex(3); target.SelectedIndex(0);
        for(size_t i=0;i<mouse.size();++i)if(mouse[i]==static_cast<UINT>(read(L"Trigger",VK_XBUTTON1)))trigger.SelectedIndex(static_cast<int>(i));
        for(size_t i=0;i<targets.size();++i)if(targets[i]==static_cast<UINT>(read(L"Target",VK_LBUTTON)))target.SelectedIndex(static_cast<int>(i));
        panel.Children().Append(trigger); panel.Children().Append(target);
        NumberBox rate,jitter; rate.Header(box_value(tr("autoclicker.rate")));rate.Minimum(1);rate.Maximum(100);rate.Value(read(L"Rate",10));
        jitter.Header(box_value(tr("autoclicker.jitter")));jitter.Minimum(0);jitter.Maximum(90);jitter.Value(read(L"Jitter",10));
        panel.Children().Append(rate);panel.Children().Append(jitter);
        TextBlock error;error.TextWrapping(TextWrapping::Wrap);panel.Children().Append(error);dialog.Content(panel);
        dialog.PrimaryButtonClick([&](auto const&,ContentDialogButtonClickEventArgs const& args){
            if(!std::isfinite(rate.Value()) || !std::isfinite(jitter.Value()) || trigger.SelectedIndex()<0 || target.SelectedIndex()<0) {error.Text(tr("autoclicker.invalid"));args.Cancel(true);return;}
            aelura::autoclicker::Options options{mouse.at(trigger.SelectedIndex()),targets.at(target.SelectedIndex()),static_cast<int>(rate.Value()),static_cast<int>(jitter.Value())};
            if(!aelura::autoclicker::valid(options)){error.Text(tr("autoclicker.invalid"));args.Cancel(true);return;}
            // Replace the whole configuration file only after every write succeeds.
            std::error_code ec;std::filesystem::create_directories(config_.parent_path(),ec);auto temporary=config_;temporary+=L".autoclicker.tmp";
            bool ok=!ec;
            if(ok && std::filesystem::exists(config_,ec))ok=CopyFileW(config_.c_str(),temporary.c_str(),FALSE)!=0;
            if(ok) {
                std::wstring section=L"Trigger="+std::to_wstring(options.trigger)+L'\0'+L"Target="+std::to_wstring(options.target)+L'\0'+L"Rate="+std::to_wstring(options.rate)+L'\0'+L"Jitter="+std::to_wstring(options.jitter)+L'\0';section+=L'\0';
                ok=WritePrivateProfileSectionW(L"AutoClicker",section.c_str(),temporary.c_str())!=0;
                WritePrivateProfileStringW(nullptr,nullptr,nullptr,temporary.c_str());
                if(ok)ok=MoveFileExW(temporary.c_str(),config_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
            }
            if(!ok){DeleteFileW(temporary.c_str());error.Text(tr("error.preference_save"));args.Cancel(true);return;}
            if(!autoClicker_.start(options)){error.Text(tr("autoclicker.failed"));args.Cancel(true);}
        });
        co_await dialog.ShowAsync();
        if(autoClicker_.running()) {
            Notice(tr("autoclicker.enabled"));
            if(!autoClickerTimer_) {
                autoClickerTimer_=DispatcherQueue().CreateTimer();autoClickerTimer_.Interval(std::chrono::milliseconds(200));
                autoClickerTimer_.Tick([weak=get_weak()](auto const&,auto const&){if(auto self=weak.get())if(!self->autoClicker_.running()) {
                    self->autoClickerTimer_.Stop();self->Notice(tr(self->autoClicker_.failed()?"autoclicker.input_failed":"autoclicker.stopped"),self->autoClicker_.failed());
                }});
            }
            autoClickerTimer_.Start();
        }
    } catch(hresult_error const& e){Notice(e.message(),true);}
    autoClickerDialog_=false;
}
}
