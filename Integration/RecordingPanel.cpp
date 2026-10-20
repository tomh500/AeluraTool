#include "pch.h"
#include "RecordingPanel.h"
#include "RecordingSelector.h"
#include "I18n.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
#include <iomanip>
#include <cmath>
namespace aelura::recording {
using namespace winrt;using namespace Microsoft::UI::Xaml;using namespace Controls;
using aelura::i18n::tr;
namespace {
void label(ContentControl const& c,char const* key){c.Content(box_value(tr(key)));}
std::wstring elapsed(double seconds){auto n=unsigned(seconds);std::wostringstream s;s<<std::setfill(L'0')<<std::setw(2)<<n/3600<<L":"<<std::setw(2)<<n/60%60<<L":"<<std::setw(2)<<n%60;return s.str();}
}
struct Panel::Impl : std::enable_shared_from_this<Impl> {
    HWND main{};std::filesystem::path config;std::function<void()> restore;
    Session session;Options options;ElementTheme requested{};unsigned seen{};bool localizing{},selectReplay{},closing{};char const* issueKey{};
    std::unique_ptr<Selector> selector;bool selectPending{};std::chrono::steady_clock::time_point selectAt;
    StackPanel root{nullptr};StackPanel settings{nullptr};ComboBox fps,codec,encoder,scaling,scale;
    NumberBox bitrate,seconds;ToggleSwitch microphone,cursor,beta;
    TextBox directory;Button browse,start,replay,stop,pause,saveReplay,preview,folder,recovery,recover,cancelSave;
    TextBlock hint,state,region,details,betaHint;ProgressRing progress;
    Window floating{nullptr},previewWindow{nullptr};HWND floatingHwnd{};Grid floatingRoot{nullptr};
    TextBlock floatState;Button floatPause,floatStop,floatSave,floatOpen;
    MediaPlayerElement player{nullptr};Windows::Media::Playback::MediaPlayer mediaPlayer{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    Impl(HWND h,std::filesystem::path const& path,std::function<void()> callback):main(h),config(path),restore(std::move(callback)) {
        options=load(config);
        if(options.directory.empty()) {PWSTR p{};check_hresult(SHGetKnownFolderPath(FOLDERID_Videos,0,nullptr,&p));options.directory=std::filesystem::path(p)/L"AeluraTool";CoTaskMemFree(p);}
        root=StackPanel();root.Spacing(14);
        hint=TextBlock();hint.TextWrapping(TextWrapping::Wrap);root.Children().Append(hint);
        region=TextBlock();region.TextWrapping(TextWrapping::Wrap);root.Children().Append(region);
        settings=StackPanel();settings.Spacing(12);
        auto row=[&](std::initializer_list<UIElement> controls){StackPanel p;p.Orientation(Orientation::Horizontal);p.Spacing(16);for(auto const& c:controls)p.Children().Append(c);settings.Children().Append(p);};
        for(auto n:{15,24,30,60,120}){fps.Items().Append(box_value(std::to_wstring(n)));if(n==int(options.fps))fps.SelectedIndex(int(fps.Items().Size())-1);}if(fps.SelectedIndex()<0)fps.SelectedIndex(2);fps.Width(150);
        bitrate.Minimum(.1);bitrate.Maximum(200);bitrate.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);bitrate.Value(options.bitrate/1000000.0);bitrate.Width(180);
        seconds.Minimum(10);seconds.Maximum(600);seconds.Value(options.replaySeconds);seconds.Width(180);seconds.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
        for(int i=0;i<2;++i){codec.Items().Append(box_value(L""));encoder.Items().Append(box_value(L""));}codec.SelectedIndex(int(options.codec));encoder.SelectedIndex(int(options.encoder));codec.Width(150);encoder.Width(180);
        for(int i=0;i<4;++i)scaling.Items().Append(box_value(L""));scaling.SelectedIndex(int(options.scaling));scaling.Width(180);
        for(auto n:{100,75,50,25}){scale.Items().Append(box_value(std::to_wstring(n)+L"%"));if(n==int(options.scale))scale.SelectedIndex(int(scale.Items().Size())-1);}if(scale.SelectedIndex()<0)scale.SelectedIndex(0);scale.Width(150);
        row({fps,bitrate,seconds});row({codec,encoder});row({scale,scaling});
        microphone.IsOn(options.microphone);cursor.IsOn(options.cursor);row({microphone,cursor});
        beta.IsOn(options.performanceBeta);settings.Children().Append(beta);betaHint=TextBlock();betaHint.TextWrapping(TextWrapping::Wrap);settings.Children().Append(betaHint);
        encoder.SelectionChanged([this](auto const&,auto const&){if(localizing)return;beta.IsEnabled(encoder.SelectedIndex()==1);if(encoder.SelectedIndex()!=1)beta.IsOn(false);});beta.IsEnabled(encoder.SelectedIndex()==1);
        directory.Text(options.directory.wstring());directory.IsReadOnly(true);settings.Children().Append(directory);settings.Children().Append(browse);
        root.Children().Append(settings);
        StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.Spacing(8);for(auto b:{start,replay,pause,stop,saveReplay})actions.Children().Append(b);root.Children().Append(actions);
        progress=ProgressRing();progress.Width(24);progress.Height(24);progress.HorizontalAlignment(HorizontalAlignment::Left);root.Children().Append(progress);
        state=TextBlock();state.TextWrapping(TextWrapping::Wrap);root.Children().Append(state);root.Children().Append(cancelSave);details=TextBlock();details.TextWrapping(TextWrapping::Wrap);root.Children().Append(details);
        StackPanel outputs;outputs.Orientation(Orientation::Horizontal);outputs.Spacing(8);for(auto b:{preview,folder,recovery,recover})outputs.Children().Append(b);root.Children().Append(outputs);
        browse.Click([this](auto const&,auto const&){pick();});start.Click([this](auto const&,auto const&){select(false);});replay.Click([this](auto const&,auto const&){select(true);});
        pause.Click([this](auto const&,auto const&){session.pause();});stop.Click([this](auto const&,auto const&){session.stop();});saveReplay.Click([this](auto const&,auto const&){saveCurrentReplay();});
        preview.Click([this](auto const&,auto const&){showPreview();});folder.Click([this](auto const&,auto const&){auto path=session.snapshot().output.parent_path();if(path.empty())path=options.directory;ShellExecuteW(main,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);});
        recovery.Click([this](auto const&,auto const&){auto path=session.snapshot().recovery;ShellExecuteW(main,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);});
        cancelSave.Click([this](auto const&,auto const&){session.cancelSave();});
        recover.Click([this](auto const&,auto const&){
            try{auto o=read();com_ptr<IFileOpenDialog> d;check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(d.put())));DWORD flags{};check_hresult(d->GetOptions(&flags));check_hresult(d->SetOptions(flags|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM));COMDLG_FILTERSPEC filters[]={{tr("record.recovery"),L"*.json"}};check_hresult(d->SetFileTypes(1,filters));check_hresult(d->SetTitle(tr("record.recover_hint")));auto hr=d->Show(main);if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;check_hresult(hr);com_ptr<IShellItem> item;check_hresult(d->GetResult(item.put()));PWSTR p{};check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH,&p));std::filesystem::path path(p);CoTaskMemFree(p);session.recover(path,o.directory);seen=0;issueKey=nullptr;}catch(...){issueKey="record.recover_failed";}
        });
        timer=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();timer.Interval(std::chrono::milliseconds(100));timer.Tick([this](auto const&,auto const&){tick();});timer.Start();
        localize();tick();
    }
    void dispose(){if(closing)return;closing=true;if(timer)timer.Stop();selector.reset();if(mediaPlayer){mediaPlayer.Close();mediaPlayer=nullptr;}if(floating){floating.Close();floating=nullptr;}if(previewWindow){previewWindow.Close();previewWindow=nullptr;}}
    ~Impl(){dispose();}
    void saveCurrentReplay(){auto s=session.snapshot();if(s.replay&&s.state==State::Completed){if(session.recover(s.recovery,options.directory))seen=0;}else session.saveReplay();}
    void pick(){
        try{com_ptr<IFileOpenDialog> d;check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(d.put())));DWORD flags{};check_hresult(d->GetOptions(&flags));check_hresult(d->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM));auto hr=d->Show(main);if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;check_hresult(hr);com_ptr<IShellItem> i;check_hresult(d->GetResult(i.put()));PWSTR p{};check_hresult(i->GetDisplayName(SIGDN_FILESYSPATH,&p));directory.Text(p);CoTaskMemFree(p);}catch(hresult_error const& e){state.Text(e.message());}
    }
    Options read(){
        Options o=options;const unsigned rates[]{15,24,30,60,120},scales[]{100,75,50,25};o.fps=rates[fps.SelectedIndex()];
        if(!std::isfinite(bitrate.Value())||!std::isfinite(seconds.Value()))throw hresult_invalid_argument();
        o.bitrate=unsigned(bitrate.Value()*1000000);o.replaySeconds=unsigned(seconds.Value());o.scale=scales[scale.SelectedIndex()];
        o.codec=Codec(codec.SelectedIndex());o.encoder=Encoder(encoder.SelectedIndex());o.scaling=Scaling(scaling.SelectedIndex());o.microphone=microphone.IsOn();o.cursor=cursor.IsOn();o.performanceBeta=beta.IsOn();o.directory=directory.Text().c_str();validate(o,{0,0,1920,1080});return o;
    }
    void select(bool replayMode){
        if(session.busy()||selector||selectPending)return;
        try{auto next=read();save(config,next);options=next;issueKey=nullptr;selectReplay=replayMode;selectPending=true;selectAt=std::chrono::steady_clock::now()+std::chrono::milliseconds(220);ShowWindow(main,SW_MINIMIZE);}
        catch(...){issueKey="record.settings_failed";restore();}
    }
    void localize(){
        localizing=true;
        hint.Text(tr("record.hint"));fps.Header(box_value(tr("record.fps")));bitrate.Header(box_value(tr("record.bitrate")));seconds.Header(box_value(tr("record.seconds")));
        codec.Header(box_value(tr("record.codec")));encoder.Header(box_value(tr("record.encoder")));scaling.Header(box_value(tr("record.scaling")));scale.Header(box_value(tr("record.scale")));
        auto combo=[](ComboBox const& c,std::initializer_list<char const*> keys){auto index=c.SelectedIndex();c.Items().Clear();for(auto key:keys)c.Items().Append(box_value(tr(key)));c.SelectedIndex(index);};
        combo(codec,{"record.h264","record.h265"});combo(encoder,{"record.cpu","record.gpu"});combo(scaling,{"record.nearest","record.linear","record.cubic","record.fant"});
        microphone.Header(box_value(tr("record.microphone")));cursor.Header(box_value(tr("record.cursor")));beta.Header(box_value(tr("record.beta")));betaHint.Text(tr("record.beta_hint"));
        for(auto t:{microphone,cursor,beta}){t.OnContent(box_value(tr("common.enabled")));t.OffContent(box_value(tr("common.disabled")));}
        directory.Header(box_value(tr("record.directory")));label(browse,"record.browse");label(start,"record.select");label(replay,"record.replay_select");label(stop,"record.stop");label(saveReplay,"record.save_replay");label(preview,"record.preview");label(folder,"record.open_folder");label(recovery,"record.recovery");
        label(recover,"record.recover");label(cancelSave,"record.cancel_save");
        if(floating){floating.Title(tr("record.title"));label(floatStop,"record.stop");label(floatSave,"record.save_replay");label(floatOpen,"record.open");}
        if(previewWindow)previewWindow.Title(tr("record.preview"));
        localizing=false;
    }
    void floatingWindow(){
        if(floating)return;
        floating=Window();floating.Title(tr("record.title"));check_hresult(floating.as<::IWindowNative>()->get_WindowHandle(&floatingHwnd));
        auto p=Microsoft::UI::Windowing::OverlappedPresenter::Create();p.IsAlwaysOnTop(true);p.IsResizable(false);p.IsMaximizable(false);p.IsMinimizable(false);floating.AppWindow().SetPresenter(p);
        floatingRoot=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="12"/>)").as<Grid>();floatingRoot.RequestedTheme(requested);
        StackPanel stack;stack.Spacing(10);floatState.TextWrapping(TextWrapping::Wrap);stack.Children().Append(floatState);StackPanel row;row.Orientation(Orientation::Horizontal);row.Spacing(8);for(auto b:{floatPause,floatStop,floatSave,floatOpen})row.Children().Append(b);stack.Children().Append(row);floatingRoot.Children().Append(stack);floating.Content(floatingRoot);
        floatPause.Click([this](auto const&,auto const&){session.pause();});floatStop.Click([this](auto const&,auto const&){session.stop();});floatSave.Click([this](auto const&,auto const&){saveCurrentReplay();});floatOpen.Click([this](auto const&,auto const&){restore();});
        floating.Closed([this](auto const&,WindowEventArgs const& e){if(!closing){e.Handled(true);if(session.busy())restore();else floating.AppWindow().Hide();}});
        SetWindowDisplayAffinity(floatingHwnd,WDA_EXCLUDEFROMCAPTURE);
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST),&mi);floating.AppWindow().MoveAndResize({mi.rcWork.right-640,mi.rcWork.top+24,620,160});localize();
    }
    fire_and_forget showPreview(){
        auto lifetime=shared_from_this();if(closing)co_return;
        auto output=session.snapshot().output;if(output.empty())co_return;
        try{
            if(!previewWindow){previewWindow=Window();previewWindow.Title(tr("record.preview"));Grid g=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>)").as<Grid>();g.RequestedTheme(requested);player=MediaPlayerElement();player.AreTransportControlsEnabled(true);g.Children().Append(player);previewWindow.Content(g);previewWindow.AppWindow().Resize({960,600});previewWindow.Closed([this](auto const&,WindowEventArgs const& e){if(!closing){e.Handled(true);if(mediaPlayer)mediaPlayer.Pause();previewWindow.AppWindow().Hide();}});}
            auto file=co_await Windows::Storage::StorageFile::GetFileFromPathAsync(output.wstring());
            if(closing)co_return;
            if(mediaPlayer)mediaPlayer.Close();mediaPlayer=Windows::Media::Playback::MediaPlayer();
            auto weak=weak_from_this();auto queue=root.DispatcherQueue();mediaPlayer.MediaFailed([weak,queue](auto const&,auto const&){queue.TryEnqueue([weak]{if(auto self=weak.lock();self&&!self->closing)self->issueKey="record.preview_failed";});});
            mediaPlayer.Source(Windows::Media::Core::MediaSource::CreateFromStorageFile(file));player.SetMediaPlayer(mediaPlayer);previewWindow.Activate();mediaPlayer.Play();
        }catch(...){if(!closing)issueKey="record.preview_failed";}
    }
    void tick(){
        if(selectPending&&std::chrono::steady_clock::now()>=selectAt){selectPending=false;try{selector=std::make_unique<Selector>(requested,selectReplay);}catch(...){issueKey="record.select_failed";restore();}}
        if(selector&&selector->done){auto selected=selector->result;selector.reset();if(selected){try{session.start(options,*selected,selectReplay);seen=0;region.Text(std::to_wstring(selected->left)+L", "+std::to_wstring(selected->top)+L" · "+std::to_wstring(selected->right-selected->left)+L" × "+std::to_wstring(selected->bottom-selected->top));floatingWindow();floating.Activate();}catch(...){issueKey="record.start_failed";restore();}}else restore();}
        auto s=session.snapshot();const bool running=s.state==State::Recording||s.state==State::Paused;const bool busy=session.busy();
        for(auto const& child:settings.Children()) {
            if(auto control=child.try_as<Control>())control.IsEnabled(!busy&&!selector&&!selectPending);
            if(auto row=child.try_as<StackPanel>())for(auto const& item:row.Children())if(auto control=item.try_as<Control>())control.IsEnabled(!busy&&!selector&&!selectPending);
        }
        beta.IsEnabled(!busy&&encoder.SelectedIndex()==1);
        start.IsEnabled(!busy&&!selector&&!selectPending);replay.IsEnabled(start.IsEnabled());pause.IsEnabled(running);stop.IsEnabled(running||s.state==State::Starting);saveReplay.IsEnabled(s.replay&&(s.state==State::Recording||s.state==State::Completed)&&!s.saving&&s.frames>0);
        label(pause,s.state==State::Paused?"record.resume":"record.pause");progress.IsActive(s.state==State::Starting||s.state==State::Stopping||s.saving);progress.Visibility(progress.IsActive()?Visibility::Visible:Visibility::Collapsed);
        const char* key=s.state==State::Starting?"record.starting":s.state==State::Paused?"record.paused":s.state==State::Stopping?"record.stopping":s.state==State::Failed?"record.failed":s.state==State::Completed?"record.completed":s.state==State::Recording?(s.replay?"record.buffering":"record.recording"):"record.idle";
        auto text=std::wstring(tr(key))+L" · "+elapsed(s.seconds);if(s.replay)text+=L" · "+std::wstring(tr("record.buffer"))+elapsed(s.bufferedSeconds);if(s.saving)text+=L" · "+std::wstring(tr("record.saving"))+L" "+std::to_wstring(unsigned(s.saveProgress*100))+L"%";
        cancelSave.Visibility(s.saving?Visibility::Visible:Visibility::Collapsed);recover.IsEnabled(!busy);
        if(s.error==E_ABORT)text+=L"\n"+std::wstring(tr("record.save_canceled"));
        else if(FAILED(s.error)){std::wostringstream error;error<<L" · 0x"<<std::hex<<unsigned(s.error);text+=error.str();text+=L"\n"+std::wstring(tr(running?"record.save_failed":"record.codec_hint"));}
        if(issueKey)text+=L"\n"+std::wstring(tr(issueKey));state.Text(text);
        std::wstring detail=s.output.wstring();if(s.width)detail+=L"\n"+std::to_wstring(s.width)+L" × "+std::to_wstring(s.height)+L" · "+tr("record.dropped")+std::to_wstring(s.dropped);details.Text(detail);
        if(s.width){detail+=L"\n"+std::wstring(tr(s.optimizedVendor==0x10de?"record.optimized_nvenc":s.optimizedVendor==0x1002?"record.optimized_amf":FAILED(s.optimizationError)?"record.fallback":"record.standard"));if(!s.adapter.empty())detail+=L" · "+s.adapter;if(s.optimizedVendor==0x10de)detail+=L"\n"+std::wstring(tr(s.cuda?"record.cuda_available":"record.cuda_unavailable"));if(FAILED(s.optimizationError)){std::wostringstream code;code<<L" · 0x"<<std::hex<<unsigned(s.optimizationError);detail+=code.str();}details.Text(detail);}
        preview.IsEnabled(!s.output.empty());folder.IsEnabled(!s.output.empty());recovery.IsEnabled(!s.recovery.empty());
        if(floating){floatState.Text(text);floatPause.IsEnabled(pause.IsEnabled());floatStop.IsEnabled(stop.IsEnabled());floatSave.IsEnabled(saveReplay.IsEnabled());floatSave.Visibility(s.replay?Visibility::Visible:Visibility::Collapsed);label(floatPause,s.state==State::Paused?"record.resume":"record.pause");if(!busy)ShowWindow(floatingHwnd,SW_HIDE);}
        if(s.savedGeneration!=seen){seen=s.savedGeneration;if(!s.replay){restore();showPreview();}}
        if(s.state==State::Failed&&lastState!=s.state)restore();lastState=s.state;
    }
    State lastState{State::Idle};
};
Panel::Panel(HWND h,std::filesystem::path const& p,std::function<void()> r):impl_(std::make_shared<Impl>(h,p,std::move(r))){}
Panel::~Panel(){impl_->dispose();}
UIElement Panel::content()const{return impl_->root;}
void Panel::localize(){impl_->localize();}
void Panel::theme(ElementTheme t){impl_->requested=t;if(impl_->floatingRoot)impl_->floatingRoot.RequestedTheme(t);if(impl_->previewWindow)impl_->previewWindow.Content().as<FrameworkElement>().RequestedTheme(t);}
void Panel::toggle(){if(impl_->session.busy())impl_->session.stop();else impl_->select(false);}
void Panel::replaySave(){impl_->saveCurrentReplay();}
bool Panel::busy()const{return impl_->session.busy()||impl_->selectPending||bool(impl_->selector);}
}
