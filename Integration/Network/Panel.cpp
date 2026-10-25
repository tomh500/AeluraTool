#include "pch.h"
#include "Panel.h"
#include "Core.h"
#include "../I18n.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <sstream>
#include <iomanip>
#include <cmath>
namespace aelura::network {
using namespace winrt;using namespace Microsoft::UI::Xaml;using namespace Controls;using i18n::tr;
struct Panel::Impl:std::enable_shared_from_this<Impl> {
    int mode{};bool running{},closed{};std::string status="net.ready";
    FrameworkElement root{nullptr};std::shared_ptr<Work> work;
    std::vector<Result> results;std::vector<Lookup> lookups;
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    template<class T>T get(wchar_t const* n){return root.FindName(n).as<T>();}
    explicit Impl(int m):mode(m) {
        root=Markup::XamlReader::Load(LR"(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Spacing="14">
<TextBlock x:Name="Hint" TextWrapping="Wrap" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
<TextBox x:Name="Host" Text="example.com"/>
<NumberBox x:Name="Port" Value="443" Minimum="1" Maximum="65535" SpinButtonPlacementMode="Inline"/>
<ComboBox x:Name="Family" SelectedIndex="0" HorizontalAlignment="Stretch"><ComboBoxItem Content="IPv4 + IPv6"/><ComboBoxItem Content="IPv4"/><ComboBoxItem Content="IPv6"/></ComboBox>
<ComboBox x:Name="Size" SelectedIndex="1" HorizontalAlignment="Stretch"><ComboBoxItem/><ComboBoxItem/><ComboBoxItem/></ComboBox>
<StackPanel Orientation="Horizontal" Spacing="10"><Button x:Name="Start" Style="{StaticResource AccentButtonStyle}"/><Button x:Name="Cancel"/><Button x:Name="Copy"/></StackPanel>
<ProgressBar x:Name="Progress" IsIndeterminate="True"/>
<TextBlock x:Name="Status" TextWrapping="Wrap" AutomationProperties.LiveSetting="Polite"/>
<TextBlock x:Name="Stage" TextWrapping="Wrap"/>
<TextBox x:Name="Results" IsReadOnly="True" TextWrapping="Wrap" AcceptsReturn="True" FontFamily="Consolas"/>
</StackPanel>)").as<FrameworkElement>();
    }
    std::wstring summary() {
        std::wostringstream s;s<<std::fixed<<std::setprecision(2);
        for(size_t i=0;i<results.size();++i){auto const& r=results[i];s<<L"IPv"<<r.family<<L" · ";if(mode==0)s<<tr(i%2?"net.upload":"net.download")<<L" · ";s<<tr(stateKey(r.state));if(!r.address.empty())s<<L" · "<<r.address;if(r.error)s<<L" · "<<tr("net.error_code")<<L" "<<r.error;
            if(r.state==State::Success){s<<L" · "<<r.milliseconds<<L" ms";if(mode==0){s<<L" · "<<r.mbps<<L" Mbps · "<<r.bytes<<L" "<<tr("net.bytes");if(r.milliseconds<500)s<<L" · "<<tr("net.short_sample");}}s<<L"\n";}
        for(auto const& l:lookups){s<<L"IPv"<<l.result.family<<L" · "<<tr(stateKey(l.result.state))<<L" · "<<l.result.milliseconds<<L" ms";if(l.result.error)s<<L" · "<<tr("net.error_code")<<L" "<<l.result.error;s<<L"\n";for(auto const& r:l.records)s<<r.address<<L" · TTL "<<r.ttl<<L" s\n";}
        return s.str();
    }
    void render() {
        if(closed)return;
        get<TextBlock>(L"Hint").Text(tr(mode==0?"net.speed_hint":mode==1?"net.dns_hint":"net.tcp_hint"));
        get<TextBox>(L"Host").Header(box_value(tr("net.host")));get<TextBox>(L"Host").Visibility(mode?Visibility::Visible:Visibility::Collapsed);
        get<NumberBox>(L"Port").Header(box_value(tr("ipv6.port")));get<NumberBox>(L"Port").Visibility(mode==2?Visibility::Visible:Visibility::Collapsed);
        auto family=get<ComboBox>(L"Family");family.Header(box_value(tr("net.family")));family.Items().GetAt(0).as<ComboBoxItem>().Content(box_value(tr("net.both")));
        auto size=get<ComboBox>(L"Size");size.Header(box_value(tr("net.size")));size.Visibility(mode==0?Visibility::Visible:Visibility::Collapsed);
        char const* sizes[]={"net.small","net.medium","net.large"};for(int i=0;i<3;++i)size.Items().GetAt(i).as<ComboBoxItem>().Content(box_value(tr(sizes[i])));
        get<Button>(L"Start").Content(box_value(tr("net.start")));get<Button>(L"Start").IsEnabled(!running);
        get<Button>(L"Cancel").Content(box_value(tr("common.cancel")));get<Button>(L"Cancel").Visibility(running?Visibility::Visible:Visibility::Collapsed);
        get<Button>(L"Copy").Content(box_value(tr("ipv6.copy")));get<Button>(L"Copy").IsEnabled(!running&&(!results.empty()||!lookups.empty()));
        family.IsEnabled(!running);size.IsEnabled(!running);get<TextBox>(L"Host").IsEnabled(!running);get<NumberBox>(L"Port").IsEnabled(!running);
        get<ProgressBar>(L"Progress").Visibility(running?Visibility::Visible:Visibility::Collapsed);
        get<TextBlock>(L"Status").Text(tr(status));get<TextBox>(L"Results").Text(summary());
        if(!running)get<TextBlock>(L"Stage").Text(L"");
    }
    void attach() {
        auto weak=weak_from_this();get<Button>(L"Start").Click([weak](auto const&,auto const&){if(auto s=weak.lock())s->run();});
        get<Button>(L"Cancel").Click([weak](auto const&,auto const&){if(auto s=weak.lock())if(s->work){s->work->cancel=true;s->status="net.cancelling";s->render();}});
        get<Button>(L"Copy").Click([weak](auto const&,auto const&){if(auto s=weak.lock())try {Windows::ApplicationModel::DataTransfer::DataPackage p;p.SetText(s->summary());Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(p);}catch(...){s->status="ipv6.copy_failed";s->render();}});
        timer=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();timer.Interval(std::chrono::milliseconds(150));
        timer.Tick([weak](auto const&,auto const&){if(auto s=weak.lock())if(s->work){auto n=s->work->stage.load();s->get<TextBlock>(L"Stage").Text(std::wstring(L"IPv")+(n>=2?L"6":L"4")+L" · "+tr(n%2?"net.upload":"net.download")+L" · "+std::to_wstring(s->work->bytes.load())+L" "+tr("net.bytes"));}});render();
    }
    fire_and_forget run() {
        auto self=shared_from_this();if(running)co_return;
        std::wstring host=get<TextBox>(L"Host").Text().c_str();auto port=get<NumberBox>(L"Port").Value();
        if(mode && (!validHost(host)||(mode==2&&(!std::isfinite(port)||port<1||port>65535||port!=std::floor(port))))){status="net.invalid";render();co_return;}
        int family=get<ComboBox>(L"Family").SelectedIndex(),size=get<ComboBox>(L"Size").SelectedIndex();
        uint64_t amounts[]={1000000,10000000,50000000};auto amount=amounts[size>=0&&size<3?size:1];
        work=std::make_shared<Work>();auto task=work;running=true;status="net.running";results.clear();lookups.clear();render();if(mode==0)timer.Start();
        std::vector<Result> found;std::vector<Lookup> names;apartment_context ui;
        co_await resume_background();bool failed=false;
        try {for(int f:{4,6}){if((family==1&&f!=4)||(family==2&&f!=6))continue;if(task->cancel)break;
            if(mode==0){for(bool up:{false,true}){if(task->cancel)break;task->deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);task->stage=(f==6?2:0)+(up?1:0);task->bytes=0;found.push_back(transfer(f,up,amount,*task));}}
            else {task->deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);if(mode==1)names.push_back(dns(host,f,*task));else found.push_back(tcp(host,static_cast<unsigned>(port),f,*task));}
        }}catch(...){failed=true;}
        co_await ui;timer.Stop();if(closed)co_return;results=std::move(found);lookups=std::move(names);running=false;status=task->cancel?"net.cancelled":failed?"net.failed":"net.finished";work.reset();render();
    }
};
Panel::Panel(int mode):impl_(std::make_shared<Impl>(mode)){impl_->attach();}
Panel::~Panel(){impl_->closed=true;if(impl_->work)impl_->work->cancel=true;if(impl_->timer)impl_->timer.Stop();}
UIElement Panel::content()const{return impl_->root;}
void Panel::localize(){impl_->render();}
}
