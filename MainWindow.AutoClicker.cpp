#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include <cmath>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
namespace {
const std::vector<UINT> mouseKeys{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
std::vector<UINT> outputKeys() {auto keys=mouseKeys;for(UINT k='A';k<='Z';++k)keys.push_back(k);for(UINT k='0';k<='9';++k)keys.push_back(k);for(UINT k=VK_F1;k<=VK_F24;++k)keys.push_back(k);return keys;}
}
void MainWindow::SetupAutoClicker() {
    auto read=[&](wchar_t const* key,int fallback){return GetPrivateProfileIntW(L"AutoClicker",key,fallback,config_.c_str());};
    autoClickerOptions_={static_cast<UINT>(read(L"Trigger",VK_XBUTTON1)),static_cast<UINT>(read(L"Target",'K')),static_cast<int>(read(L"Rate",10)),static_cast<int>(read(L"Jitter",10))};
    if(!aelura::autoclicker::valid(autoClickerOptions_))autoClickerOptions_=aelura::autoclicker::Options{};
    toggleNotification_=std::make_unique<aelura::notifications::ToggleNotification>(hwnd_);
    hotkeyNotifications_=GetPrivateProfileIntW(L"Behavior",L"HotkeyNotifications",1,config_.c_str())!=0;
    HotkeyNotificationsSwitch().IsOn(hotkeyNotifications_);
    HotkeyNotificationsSwitch().Toggled([this](auto const&,auto const&){if(localizing_)return;auto requested=HotkeyNotificationsSwitch().IsOn();if(SaveBehavior(L"HotkeyNotifications",requested)){hotkeyNotifications_=requested;if(!requested&&toggleNotification_)toggleNotification_->close();}else{localizing_=true;HotkeyNotificationsSwitch().IsOn(hotkeyNotifications_);localizing_=false;}});
    AutoClickerBackButton().Click([this](auto const&,auto const&){GoBack();});
    AutoClickerSwitch().Toggled([this](auto const&,auto const&){if(!autoClickerUpdating_)ToggleAutoClicker();});
    AutoClickerSave().Click([this](auto const&,auto const&){if(SaveAutoClicker())Notice(tr("autoclicker.saved"));});
    autoClickerTimer_=DispatcherQueue().CreateTimer();autoClickerTimer_.Interval(std::chrono::milliseconds(100));
    autoClickerTimer_.Tick([this](auto const&,auto const&){if(!autoClicker_.running()){autoClickerTimer_.Stop();RefreshAutoClicker();if(autoClicker_.failed())Notice(tr("autoclicker.input_failed"),true);else Notice(tr("autoclicker.stopped"));}});
}
void MainWindow::LocalizeAutoClicker() {
    autoClickerUpdating_=true;
    UINT selectedTrigger=autoClickerOptions_.trigger,selectedTarget=autoClickerOptions_.target;
    auto targets=outputKeys();
    if(AutoClickerTrigger().SelectedIndex()>=0)selectedTrigger=mouseKeys.at(AutoClickerTrigger().SelectedIndex());
    if(AutoClickerTarget().SelectedIndex()>=0)selectedTarget=targets.at(AutoClickerTarget().SelectedIndex());
    AutoClickerTrigger().Items().Clear();AutoClickerTarget().Items().Clear();
    const char* names[]{"autoclicker.left","autoclicker.right","autoclicker.middle","autoclicker.side1","autoclicker.side2"};
    for(size_t i=0;i<targets.size();++i){auto k=targets[i];std::wstring label=i<5?tr(names[i]):k>=VK_F1?L"F"+std::to_wstring(k-VK_F1+1):std::wstring(1,static_cast<wchar_t>(k));AutoClickerTarget().Items().Append(box_value(label));if(i<5)AutoClickerTrigger().Items().Append(box_value(label));}
    for(size_t i=0;i<mouseKeys.size();++i)if(mouseKeys[i]==selectedTrigger)AutoClickerTrigger().SelectedIndex(static_cast<int>(i));
    for(size_t i=0;i<targets.size();++i)if(targets[i]==selectedTarget)AutoClickerTarget().SelectedIndex(static_cast<int>(i));
    if(!std::isfinite(AutoClickerRate().Value()))AutoClickerRate().Value(autoClickerOptions_.rate);
    if(!std::isfinite(AutoClickerJitter().Value()))AutoClickerJitter().Value(autoClickerOptions_.jitter);
    AutoClickerBackLabel().Text(tr("nav.back"));AutoClickerHint().Text(tr("autoclicker.page_hint"));
    AutoClickerParametersTitle().Text(tr("autoclicker.parameters"));AutoClickerTrigger().Header(box_value(tr("autoclicker.trigger")));AutoClickerTarget().Header(box_value(tr("autoclicker.target")));
    AutoClickerRate().Header(box_value(tr("autoclicker.rate")));AutoClickerJitter().Header(box_value(tr("autoclicker.jitter")));AutoClickerSave().Content(box_value(tr("common.save")));
    AutoClickerSwitch().OnContent(box_value(tr("common.enabled")));AutoClickerSwitch().OffContent(box_value(tr("common.disabled")));
    HotkeyNotificationsSwitch().Header(box_value(tr("settings.hotkey_notifications")));HotkeyNotificationsHint().Text(tr("settings.hotkey_notifications_hint"));
    HotkeyNotificationsSwitch().OnContent(box_value(tr("common.enabled")));HotkeyNotificationsSwitch().OffContent(box_value(tr("common.disabled")));
    autoClickerUpdating_=false;RefreshAutoClicker();
}
void MainWindow::RefreshAutoClicker() {
    autoClickerUpdating_=true;bool enabled=autoClicker_.running();AutoClickerSwitch().IsOn(enabled);
    AutoClickerStateTitle().Text(tr(enabled?"autoclicker.state_on":"autoclicker.state_off"));AutoClickerStateHint().Text(tr(enabled?"autoclicker.state_on_hint":"autoclicker.state_off_hint"));
    AutoClickerTrigger().IsEnabled(!enabled);AutoClickerTarget().IsEnabled(!enabled);AutoClickerRate().IsEnabled(!enabled);AutoClickerJitter().IsEnabled(!enabled);AutoClickerSave().IsEnabled(!enabled);
    autoClickerUpdating_=false;
}
bool MainWindow::SaveAutoClicker() {
    if(autoClicker_.running())return false;
    if(!std::isfinite(AutoClickerRate().Value())||!std::isfinite(AutoClickerJitter().Value())||AutoClickerTrigger().SelectedIndex()<0||AutoClickerTarget().SelectedIndex()<0){Notice(tr("autoclicker.invalid"),true);return false;}
    aelura::autoclicker::Options options{mouseKeys.at(AutoClickerTrigger().SelectedIndex()),outputKeys().at(AutoClickerTarget().SelectedIndex()),static_cast<int>(AutoClickerRate().Value()),static_cast<int>(AutoClickerJitter().Value())};
    if(!aelura::autoclicker::valid(options)){Notice(tr("autoclicker.invalid"),true);return false;}
    std::error_code ec;std::filesystem::create_directories(config_.parent_path(),ec);auto temp=config_;temp+=L".autoclicker.tmp";bool ok=!ec;
    if(ok&&std::filesystem::exists(config_,ec))ok=CopyFileW(config_.c_str(),temp.c_str(),FALSE)!=0;else if(ok)DeleteFileW(temp.c_str());
    if(ok){std::wstring section=L"Trigger="+std::to_wstring(options.trigger)+L'\0'+L"Target="+std::to_wstring(options.target)+L'\0'+L"Rate="+std::to_wstring(options.rate)+L'\0'+L"Jitter="+std::to_wstring(options.jitter)+L'\0';section+=L'\0';ok=WritePrivateProfileSectionW(L"AutoClicker",section.c_str(),temp.c_str())!=0;WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());if(ok)ok=MoveFileExW(temp.c_str(),config_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;}
    if(!ok){DeleteFileW(temp.c_str());Notice(tr("error.preference_save"),true);return false;}
    autoClickerOptions_=options;return true;
}
void MainWindow::ToggleAutoClicker(bool hotkey) {
    bool requested=!autoClicker_.running();
    if(requested){if(!SaveAutoClicker()){RefreshAutoClicker();return;}if(!autoClicker_.start(autoClickerOptions_,hwnd_)){Notice(tr("autoclicker.failed"),true);RefreshAutoClicker();return;}autoClickerTimer_.Start();}
    else{autoClicker_.stop();autoClickerTimer_.Stop();}
    RefreshAutoClicker();
    if(!hotkey)Notice(tr(requested?"autoclicker.enabled":"autoclicker.stopped"));
    if(!hotkey&&toggleNotification_)toggleNotification_->show(tr("autoclicker.title"),requested,Root().RequestedTheme());
}
}
