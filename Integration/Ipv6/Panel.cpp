#include "pch.h"
#include "Panel.h"
#include "Core.h"
#include "../I18n.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <optional>
#include <sstream>
#include <algorithm>
#include <stdexcept>
namespace aelura::ipv6 {
using namespace winrt;using namespace Microsoft::UI::Xaml;using namespace Controls;using i18n::tr;
namespace {
char const* stateKey(State s) {switch(s){case State::Pass:return "ipv6.pass";case State::Fail:return "ipv6.fail";case State::Cancelled:return "ipv6.cancelled";default:return "ipv6.unknown_short";}}
char const* kindKey(Kind k) {switch(k){case Kind::Global:return "ipv6.global_address";case Kind::LinkLocal:return "ipv6.link_local";case Kind::Local:return "ipv6.local_address";case Kind::Loopback:return "ipv6.loopback";case Kind::Multicast:return "ipv6.multicast";default:return "ipv6.other_address";}}
char const* repairKey(RepairResult r) {switch(r){case RepairResult::Success:return "ipv6.repaired";case RepairResult::Restart:return "ipv6.restart";case RepairResult::Denied:return "ipv6.denied";case RepairResult::BackupFailed:return "ipv6.backup_failed";case RepairResult::Conflict:return "ipv6.conflict";case RepairResult::RollbackFailed:return "ipv6.rollback_failed";case RepairResult::NoRecord:return "ipv6.no_record";default:return "ipv6.repair_failed";}}
std::wstring checksText(std::vector<Check> const& checks) {std::wstring out;for(auto const& c:checks)out+=std::wstring(tr(c.key))+L": "+tr(stateKey(c.state))+(c.detail.empty()?L"":L" · "+c.detail)+L"\n";return out;}
}
struct Panel::Impl:std::enable_shared_from_this<Impl> {
    HWND owner{};FrameworkElement root{nullptr};std::vector<Adapter> list;std::optional<Report> report;
    std::vector<Check> extra;std::shared_ptr<Work> work;bool running{},repairing{},closed{},updating{};
    std::string message="ipv6.ready",repairMessage;Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    template<class T>T get(wchar_t const* name)const{return root.FindName(name).as<T>();}
    explicit Impl(HWND hwnd):owner(hwnd) {
        root=Markup::XamlReader::Load(LR"(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Spacing="14">
<TextBlock x:Name="Hint" TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
<ComboBox x:Name="Adapters" HorizontalAlignment="Stretch"/>
<StackPanel Orientation="Horizontal" Spacing="10"><Button x:Name="Detect" Style="{StaticResource AccentButtonStyle}"/><Button x:Name="Refresh"/><Button x:Name="Cancel"/><ProgressRing x:Name="Progress" Width="24" Height="24"/></StackPanel>
<TextBlock x:Name="Status" TextWrapping="Wrap" FontSize="18" AutomationProperties.LiveSetting="Polite"/>
<TextBlock x:Name="Stage" TextWrapping="Wrap"/>
<TextBlock x:Name="Advice" TextWrapping="Wrap"/>
<TextBlock x:Name="RepairStatus" TextWrapping="Wrap"/>
<StackPanel Orientation="Horizontal" Spacing="10"><Button x:Name="Enable"/><Button x:Name="Restore"/></StackPanel>
<TextBox x:Name="Results" IsReadOnly="True" TextWrapping="Wrap" FontFamily="Consolas" AcceptsReturn="True"/>
<CheckBox x:Name="Details"/><Button x:Name="Copy" HorizontalAlignment="Left"/>
<Expander x:Name="Advanced" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch"><StackPanel Spacing="12">
<TextBlock x:Name="Targets" TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
<TextBlock x:Name="ProbeHint" TextWrapping="Wrap"/>
<ComboBox x:Name="Operation" SelectedIndex="0" HorizontalAlignment="Stretch"><ComboBoxItem/><ComboBoxItem/><ComboBoxItem/><ComboBoxItem/><ComboBoxItem/></ComboBox>
<TextBox x:Name="Target"/>
<NumberBox x:Name="Port" Minimum="1" Maximum="65535" Value="25565" SpinButtonPlacementMode="Inline"/>
<Button x:Name="Run" HorizontalAlignment="Left"/>
<TextBox x:Name="Extra" IsReadOnly="True" TextWrapping="Wrap" AcceptsReturn="True" FontFamily="Consolas"/>
</StackPanel></Expander></StackPanel>)").as<FrameworkElement>();
    }
    Adapter selected()const {int n=get<ComboBox>(L"Adapters").SelectedIndex();return n>=0 && n<static_cast<int>(list.size())?list[n]:Adapter{};}
    void attach() {
        auto weak=weak_from_this();
        get<Button>(L"Detect").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->run(false);});
        get<Button>(L"Run").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->run(true);});
        get<Button>(L"Refresh").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->refresh();});
        get<Button>(L"Cancel").Click([weak](auto const&,auto const&){if(auto s=weak.lock())if(s->work)s->work->cancel=true;});
        get<Button>(L"Enable").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->fix(false);});
        get<Button>(L"Restore").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->fix(true);});
        get<Button>(L"Copy").Click([weak](auto const&,auto const&){if(auto s=weak.lock()) {try {Windows::ApplicationModel::DataTransfer::DataPackage data;data.SetText(s->summary(s->get<CheckBox>(L"Details").IsChecked().Value()));Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);}catch(...) {s->message="ipv6.copy_failed";s->render();}}});
        get<ComboBox>(L"Adapters").SelectionChanged([weak](auto const&,auto const&){if(auto s=weak.lock())if(!s->updating){s->report.reset();s->extra.clear();s->message="ipv6.ready";s->render();}});
        get<ComboBox>(L"Operation").SelectionChanged([weak](auto const&,auto const&){if(auto s=weak.lock())s->render();});
        timer=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();timer.Interval(std::chrono::milliseconds(150));
        timer.Tick([weak](auto const&,auto const&){if(auto s=weak.lock())if(s->work && !s->closed)s->get<TextBlock>(L"Stage").Text(std::wstring(tr("ipv6.steps"))+std::to_wstring(s->work->progress.load()));});
        render();refresh();
    }
    std::wstring summary(bool includeDetails=false)const {
        if(!report)return {};
        auto const& a=report->adapter;std::wstring out=std::wstring(tr(conclusionKey(report->conclusion)))+L"\n"+a.name+L" (#"+std::to_wstring(a.index)+L")\n";
        out+=std::wstring(tr("ipv6.binding"))+L": "+tr(a.binding==1?"ipv6.pass":a.binding==0?"ipv6.fail":"ipv6.unknown_short")+L"\n";
        out+=std::wstring(tr("ipv6.system"))+L": "+tr(report->config.value&0x10?"ipv6.fail":"ipv6.pass")+L"\n";
        out+=std::wstring(tr("ipv6.addresses"))+L": "+tr(a.global?"ipv6.pass":"ipv6.fail")+L"\n";
        out+=std::wstring(tr("ipv6.route"))+L": "+tr(a.route?"ipv6.pass":"ipv6.fail")+L"\n"+checksText(report->checks);
        if(includeDetails) {for(auto const& v:a.addresses)out+=v+L"\n";for(auto const& v:a.dns)out+=v+L"\n";}
        else out+=std::wstring(tr("ipv6.redacted"))+L"\n";
        return out;
    }
    void render() {
        if(closed)return;
        auto label=[&](wchar_t const* n,char const* key){get<Button>(n).Content(box_value(tr(key)));};
        label(L"Detect","ipv6.detect");label(L"Refresh","ipv6.refresh");label(L"Cancel","common.cancel");label(L"Enable","ipv6.enable");label(L"Restore","ipv6.restore");label(L"Copy","ipv6.copy");label(L"Run","ipv6.run");
        get<TextBlock>(L"Hint").Text(tr("ipv6.hint"));get<TextBlock>(L"ProbeHint").Text(tr("ipv6.probe_hint"));
        get<TextBlock>(L"Targets").Text(tr("ipv6.targets"));
        get<ComboBox>(L"Adapters").Header(box_value(tr("ipv6.adapter")));get<CheckBox>(L"Details").Content(box_value(tr("ipv6.include_details")));get<Expander>(L"Advanced").Header(box_value(tr("ipv6.advanced")));
        auto op=get<ComboBox>(L"Operation");int index=op.SelectedIndex();updating=true;
        char const* names[]={"ipv6.dns","ipv6.ping","ipv6.trace","ipv6.tcp","ipv6.calculation"};
        for(int i=0;i<5;++i)op.Items().GetAt(i).as<ComboBoxItem>().Content(box_value(tr(names[i])));
        get<TextBox>(L"Target").Header(box_value(tr(index==4?"ipv6.cidr":"ipv6.target")));get<TextBox>(L"Target").PlaceholderText(tr(index==4?"ipv6.cidr_example":"ipv6.target_example"));
        get<NumberBox>(L"Port").Header(box_value(tr("ipv6.port")));get<NumberBox>(L"Port").Visibility(index==3?Visibility::Visible:Visibility::Collapsed);updating=false;
        bool busy=running||repairing;for(auto n:{L"Detect",L"Refresh",L"Run"})get<Button>(n).IsEnabled(!busy);
        get<ComboBox>(L"Adapters").IsEnabled(!busy);op.IsEnabled(!busy);get<TextBox>(L"Target").IsEnabled(!busy);get<NumberBox>(L"Port").IsEnabled(!busy);
        get<Button>(L"Cancel").Visibility(running&&work?Visibility::Visible:Visibility::Collapsed);
        get<ProgressRing>(L"Progress").IsActive(busy);get<ProgressRing>(L"Progress").Visibility(busy?Visibility::Visible:Visibility::Collapsed);
        get<TextBlock>(L"Status").Text(tr(message));get<TextBlock>(L"Stage").Visibility(running&&work?Visibility::Visible:Visibility::Collapsed);
        get<TextBlock>(L"RepairStatus").Text(repairMessage.empty()?L"":tr(repairMessage));
        auto a=report?report->adapter:selected();bool canEnable=!a.id.empty() && (a.binding==0 || (report && report->config.value&0x10));
        get<Button>(L"Enable").Visibility(canEnable?Visibility::Visible:Visibility::Collapsed);get<Button>(L"Enable").IsEnabled(!busy);
        get<Button>(L"Restore").Visibility(recoveryAvailable()?Visibility::Visible:Visibility::Collapsed);get<Button>(L"Restore").IsEnabled(!busy);
        get<Button>(L"Copy").IsEnabled(report.has_value()&&!busy);
        get<TextBlock>(L"Advice").Text(report?tr(report->conclusion==Conclusion::Configuration?"ipv6.advice_enable":report->conclusion==Conclusion::NoNetwork?"ipv6.advice_network":report->conclusion==Conclusion::Verified?"ipv6.advice_verified":"ipv6.advice_uncertain"):L"");
        std::wstring details;
        if(report) {details=summary();auto cut=details.find(tr("ipv6.redacted"));if(cut!=details.npos)details.resize(cut);details+=std::wstring(tr("ipv6.addresses"))+L"\n";for(auto const& v:report->adapter.addressInfo)details+=v.value+L" · "+tr(kindKey(v.kind))+L" · "+tr(v.usable?"ipv6.address_usable":"ipv6.address_pending")+L"\n";details+=std::wstring(tr("ipv6.dns_servers"))+L"\n";for(auto const& v:report->adapter.dns)details+=v+L"\n";}
        get<TextBox>(L"Results").Text(details);get<TextBox>(L"Results").Visibility(report?Visibility::Visible:Visibility::Collapsed);get<TextBox>(L"Extra").Text(checksText(extra));
    }
    fire_and_forget refresh() {
        auto self=shared_from_this();if(running||repairing)co_return;running=true;message="ipv6.reading";render();apartment_context ui;
        auto previous=selected().id;std::vector<Adapter> found;bool failed=false;
        co_await resume_background();try {found=adapters();}catch(...) {failed=true;}co_await ui;if(closed)co_return;
        list=std::move(found);report.reset();extra.clear();auto combo=get<ComboBox>(L"Adapters");updating=true;combo.Items().Clear();for(auto const& a:list)combo.Items().Append(box_value(a.name+L" (#"+std::to_wstring(a.index)+L")"));
        int n=choose(list);for(size_t i=0;i<list.size();++i)if(list[i].id==previous)n=static_cast<int>(i);combo.SelectedIndex(n);updating=false;
        running=false;message=failed?"ipv6.read_failed":list.empty()?"ipv6.no_adapters":"ipv6.ready";render();
    }
    fire_and_forget run(bool advancedMode) {
        auto self=shared_from_this();if(running||repairing)co_return;auto a=selected();auto op=static_cast<Operation>(get<ComboBox>(L"Operation").SelectedIndex());
        if(a.id.empty() && (!advancedMode || op!=Operation::Calculate)) {message="ipv6.no_adapters";render();co_return;}
        std::wstring target=get<TextBox>(L"Target").Text().c_str();auto port=get<NumberBox>(L"Port").Value();
        if(advancedMode && (target.empty() || target.size()>1024 || (op==Operation::Tcp && (!(port>=1 && port<=65535) || port!=static_cast<unsigned>(port))))) {message="ipv6.invalid_input";render();co_return;}
        running=true;work=std::make_shared<Work>();auto task=work;message="ipv6.running";render();timer.Start();apartment_context ui;
        std::optional<Report> result;std::vector<Check> output;bool failed=false;
        co_await resume_background();try {
            if(advancedMode)output=advanced(op,target,static_cast<unsigned>(port>=1 && port<=65535?port:25565),a,*task);
            else {auto fresh=adapters();auto found=std::find_if(fresh.begin(),fresh.end(),[&](auto const& value){return value.id==a.id;});if(found==fresh.end())throw std::runtime_error("Adapter removed");result=diagnose(*found,*task);}
        }catch(...) {failed=true;}
        co_await ui;timer.Stop();if(closed)co_return;
        running=false;if(result)report=std::move(result);if(advancedMode)extra=std::move(output);
        message=task->cancel?"ipv6.cancelled":failed?"ipv6.read_failed":advancedMode?(task->stopped()?"ipv6.timeout":"ipv6.finished"):conclusionKey(report->conclusion);
        work.reset();render();
    }
    fire_and_forget fix(bool restore) {
        auto self=shared_from_this();if(running||repairing)co_return;auto a=selected();auto id=restore?recoveryAdapter():a.id;if(id.empty())co_return;
        repairing=true;render();
        try {
            ContentDialog dialog;dialog.XamlRoot(root.XamlRoot());dialog.RequestedTheme(root.RequestedTheme());dialog.Title(box_value(tr(restore?"ipv6.restore":"ipv6.enable")));dialog.PrimaryButtonText(tr("common.continue"));dialog.CloseButtonText(tr("common.cancel"));
            TextBlock text;text.Text(std::wstring(tr(restore?"ipv6.restore_confirm":"ipv6.enable_confirm"))+L"\n"+(restore?id:a.name+L"\n"+id));text.TextWrapping(TextWrapping::Wrap);dialog.Content(text);
            if(co_await dialog.ShowAsync()!=ContentDialogResult::Primary){repairing=false;render();co_return;}
        } catch(...) {repairing=false;repairMessage="ipv6.repair_failed";render();co_return;}
        if(closed)co_return;message="ipv6.repairing";render();apartment_context ui;auto hwnd=owner;
        co_await resume_background();auto result=launchRepair(hwnd,id,restore);co_await ui;if(closed)co_return;
        repairing=false;repairMessage=repairKey(result);render();
        if(result==RepairResult::Success || result==RepairResult::Restart)run(false);
    }
};
Panel::Panel(HWND owner):impl_(std::make_shared<Impl>(owner)){impl_->attach();}
Panel::~Panel(){impl_->closed=true;if(impl_->work)impl_->work->cancel=true;if(impl_->timer)impl_->timer.Stop();}
UIElement Panel::content()const{return impl_->root;}
void Panel::localize(){impl_->render();}
bool Panel::busy()const{return impl_->repairing;}
}
