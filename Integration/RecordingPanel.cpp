#include "pch.h"
#include "RecordingPanel.h"
#include "RecordingSelector.h"
#include "RecordingNotification.h"
#include "Diagnostics.h"
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
#include <dwmapi.h>
namespace aelura::recording {
using namespace winrt;using namespace Microsoft::UI::Xaml;using namespace Controls;
using aelura::i18n::tr;
namespace {
void label(ContentControl const& c,char const* key){c.Content(box_value(tr(key)));}
std::wstring elapsed(double seconds){auto n=unsigned(seconds);std::wostringstream s;s<<std::setfill(L'0')<<std::setw(2)<<n/3600<<L":"<<std::setw(2)<<n/60%60<<L":"<<std::setw(2)<<n%60;return s.str();}
}
struct Panel::Impl : std::enable_shared_from_this<Impl> {
    HWND main{};std::filesystem::path config;std::function<void()> restore;std::function<HWND()> captureHost;
    Session session,replaySession;Notification notification;Options options;ElementTheme requested{};unsigned seen{},seenReplay{};bool localizing{},closing{},syncReplay{},previewLoading{},quitting{},autoReplay{},pickingWindow{};char const* issueKey{};
    std::filesystem::path latestOutput;
    ContentDialog windowDialog{nullptr};
    std::unique_ptr<Selector> selector;bool selectPending{};std::chrono::steady_clock::time_point selectAt;
    StackPanel root{nullptr};StackPanel settings{nullptr};ComboBox fps,codec,encoder,scaling,scale,graphics,replayStorage;
    NumberBox bitrate,seconds,memoryLimit;ToggleSwitch microphone,cursor,beta,replay;bool syncStorage{};
    TextBox directory;Button browse,start,windowStart,stop,pause,saveReplay,preview,folder,recovery,recover,cancelSave;
    TextBlock hint,state,region,details,betaHint,replayState,graphicsHint,memoryHint;ProgressRing progress;
    Window floating{nullptr},previewWindow{nullptr};HWND floatingHwnd{};Grid floatingRoot{nullptr};
    TextBlock floatState;Button floatPause,floatStop,floatSave,floatOpen;
    MediaPlayerElement player{nullptr};Windows::Media::Playback::MediaPlayer mediaPlayer{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    Impl(HWND h,std::filesystem::path const& path,std::function<void()> callback,std::function<HWND()> host):main(h),config(path),restore(std::move(callback)),captureHost(std::move(host)),notification(h) {
        options=load(config);
        if(options.directory.empty()) {PWSTR p{};check_hresult(SHGetKnownFolderPath(FOLDERID_Videos,0,nullptr,&p));options.directory=std::filesystem::path(p)/L"AeluraTool";CoTaskMemFree(p);}
        // Older development deployments accidentally persisted an automated UI
        // fixture folder. Do not move or delete any videos in that directory.
        if(options.directory.filename().wstring().starts_with(L"AeluraRecordingUI-")&&options.directory.parent_path()==std::filesystem::temp_directory_path()){
            PWSTR p{};check_hresult(SHGetKnownFolderPath(FOLDERID_Videos,0,nullptr,&p));options.directory=std::filesystem::path(p)/L"AeluraTool";CoTaskMemFree(p);
            try{save(config,options);}catch(...){issueKey="record.settings_failed";}
        }
        autoReplay=options.replayEnabled;
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
        graphics.Width(350);for(int i=0;i<2;++i)graphics.Items().Append(box_value(L""));graphics.SelectedIndex(int(options.graphics));settings.Children().Append(graphics);
        graphicsHint=TextBlock();graphicsHint.TextWrapping(TextWrapping::Wrap);settings.Children().Append(graphicsHint);
        replayStorage.Width(240);for(int i=0;i<2;++i)replayStorage.Items().Append(box_value(L""));replayStorage.SelectedIndex(int(options.replayStorage));
        memoryLimit.Minimum(128);memoryLimit.Maximum(8192);memoryLimit.SmallChange(128);memoryLimit.LargeChange(512);memoryLimit.Value(options.memoryLimitMB);memoryLimit.Width(180);memoryLimit.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);row({replayStorage,memoryLimit});
        memoryHint=TextBlock();memoryHint.TextWrapping(TextWrapping::Wrap);settings.Children().Append(memoryHint);
        microphone.IsOn(options.microphone);cursor.IsOn(options.cursor);row({microphone,cursor});
        beta.IsOn(options.performanceBeta);settings.Children().Append(beta);betaHint=TextBlock();betaHint.TextWrapping(TextWrapping::Wrap);settings.Children().Append(betaHint);
        encoder.SelectionChanged([this](auto const&,auto const&){if(localizing)return;beta.IsEnabled(encoder.SelectedIndex()==1);if(encoder.SelectedIndex()!=1){beta.IsOn(false);graphics.SelectedIndex(0);}});beta.IsEnabled(encoder.SelectedIndex()==1);
        graphics.SelectionChanged([this](auto const&,auto const&){if(!localizing&&graphics.SelectedIndex()==1){encoder.SelectedIndex(1);beta.IsOn(true);}});
        beta.Toggled([this](auto const&,auto const&){if(!localizing&&!beta.IsOn())graphics.SelectedIndex(0);});
        directory.Text(options.directory.wstring());directory.IsReadOnly(true);settings.Children().Append(directory);settings.Children().Append(browse);
        replayStorage.SelectionChanged([this](auto const&,auto const&){saveStorage();});memoryLimit.ValueChanged([this](auto const&,auto const&){saveStorage();});
        root.Children().Append(settings);
        root.Children().Append(replay);
        replayState=TextBlock();replayState.TextWrapping(TextWrapping::Wrap);root.Children().Append(replayState);
        StackPanel actions;actions.Orientation(Orientation::Horizontal);actions.Spacing(8);for(auto b:{start,windowStart,pause,stop,saveReplay})actions.Children().Append(b);root.Children().Append(actions);
        progress=ProgressRing();progress.Width(24);progress.Height(24);progress.HorizontalAlignment(HorizontalAlignment::Left);root.Children().Append(progress);
        state=TextBlock();state.TextWrapping(TextWrapping::Wrap);root.Children().Append(state);root.Children().Append(cancelSave);details=TextBlock();details.TextWrapping(TextWrapping::Wrap);root.Children().Append(details);
        StackPanel outputs;outputs.Orientation(Orientation::Horizontal);outputs.Spacing(8);for(auto b:{preview,folder,recovery,recover})outputs.Children().Append(b);root.Children().Append(outputs);
        browse.Click([this](auto const&,auto const&){pick();});start.Click([this](auto const&,auto const&){select();});replay.Toggled([this](auto const&,auto const&){if(!syncReplay)toggleReplay();});
        pause.Click([this](auto const&,auto const&){session.pause();});stop.Click([this](auto const&,auto const&){session.stop();});saveReplay.Click([this](auto const&,auto const&){saveCurrentReplay();});
        windowStart.Click([this](auto const&,auto const&){chooseWindow();});
        preview.Click([this](auto const&,auto const&){showPreview();});folder.Click([this](auto const&,auto const&){auto path=latestOutput.parent_path();if(path.empty())path=options.directory;ShellExecuteW(main,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);});
        recovery.Click([this](auto const&,auto const&){auto path=replaySession.snapshot().recovery;if(path.empty())path=session.snapshot().recovery;ShellExecuteW(main,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);});
        cancelSave.Click([this](auto const&,auto const&){replaySession.cancelSave();});
        recover.Click([this](auto const&,auto const&){
            try{auto o=read();com_ptr<IFileOpenDialog> d;check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(d.put())));DWORD flags{};check_hresult(d->GetOptions(&flags));check_hresult(d->SetOptions(flags|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM));COMDLG_FILTERSPEC filters[]={{tr("record.recovery"),L"*.json"}};check_hresult(d->SetFileTypes(1,filters));check_hresult(d->SetTitle(tr("record.recover_hint")));auto hr=d->Show(main);if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;check_hresult(hr);com_ptr<IShellItem> item;check_hresult(d->GetResult(item.put()));PWSTR p{};check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH,&p));std::filesystem::path path(p);CoTaskMemFree(p);replaySession.recover(path,o.directory);seenReplay=0;issueKey=nullptr;}catch(...){issueKey="record.recover_failed";}
        });
        timer=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();timer.Interval(std::chrono::milliseconds(100));
        localize();tick();
    }
    void beginTick(){
        auto weak=weak_from_this();
        timer.Tick([weak](auto const&,auto const&){
            auto self=weak.lock();if(!self||self->closing)return;
            try{self->tick();}
            catch(hresult_error const& e){if(!self->issueKey||std::string_view(self->issueKey)!="record.ui_failed")diagnostics::failure("recording.tick",e.code(),e.message());self->issueKey="record.ui_failed";}
            catch(std::exception const&){diagnostics::failure("recording.tick",E_FAIL);self->issueKey="record.ui_failed";}
        });timer.Start();
    }
    void dispose() noexcept {
        if(closing)return;closing=true;
        auto attempt=[](auto&& action){try{action();}catch(hresult_error const& e){diagnostics::failure("recording.close",e.code(),e.message());}catch(...){diagnostics::failure("recording.close",E_FAIL);}};
        attempt([&]{if(timer)timer.Stop();});selector.reset();attempt([&]{notification.close();});
        attempt([&]{if(player)player.SetMediaPlayer(nullptr);});attempt([&]{if(mediaPlayer)mediaPlayer.Close();});mediaPlayer=nullptr;
        attempt([&]{if(floating)floating.Close();});floating=nullptr;attempt([&]{if(previewWindow)previewWindow.Close();});previewWindow=nullptr;
    }
    ~Impl(){dispose();}
    void saveCurrentReplay(){if(quitting)return;try{if(replaySession.saveReplay())notification.update(replaySession.snapshot(),requested);else issueKey="record.replay_not_ready";}catch(hresult_error const& e){diagnostics::failure("replay.save.ui",e.code(),e.message());issueKey="record.ui_failed";}catch(...){diagnostics::failure("replay.save.ui",E_FAIL);issueKey="record.ui_failed";}}
    void pick(){
        try{com_ptr<IFileOpenDialog> d;check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(d.put())));DWORD flags{};check_hresult(d->GetOptions(&flags));check_hresult(d->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM));auto hr=d->Show(main);if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;check_hresult(hr);com_ptr<IShellItem> i;check_hresult(d->GetResult(i.put()));PWSTR p{};check_hresult(i->GetDisplayName(SIGDN_FILESYSPATH,&p));directory.Text(p);CoTaskMemFree(p);}catch(hresult_error const& e){state.Text(e.message());}
    }
    Options read(){
        if(fps.SelectedIndex()<0||fps.SelectedIndex()>=5||scale.SelectedIndex()<0||scale.SelectedIndex()>=4||codec.SelectedIndex()<0||encoder.SelectedIndex()<0||scaling.SelectedIndex()<0||graphics.SelectedIndex()<0||graphics.SelectedIndex()>1)throw hresult_invalid_argument();
        Options o=options;const unsigned rates[]{15,24,30,60,120},scales[]{100,75,50,25};o.fps=rates[fps.SelectedIndex()];
        if(!std::isfinite(bitrate.Value())||!std::isfinite(seconds.Value()))throw hresult_invalid_argument();
        o.bitrate=unsigned(bitrate.Value()*1000000);o.replaySeconds=unsigned(seconds.Value());o.scale=scales[scale.SelectedIndex()];
        if(replayStorage.SelectedIndex()<0||replayStorage.SelectedIndex()>1||!std::isfinite(memoryLimit.Value())||memoryLimit.Value()!=std::floor(memoryLimit.Value())||memoryLimit.Value()<128||memoryLimit.Value()>8192)throw hresult_invalid_argument();
        o.replayStorage=ReplayStorage(replayStorage.SelectedIndex());o.memoryLimitMB=unsigned(memoryLimit.Value());
        o.codec=Codec(codec.SelectedIndex());o.encoder=Encoder(encoder.SelectedIndex());o.scaling=Scaling(scaling.SelectedIndex());o.graphics=Graphics(graphics.SelectedIndex());o.microphone=microphone.IsOn();o.cursor=cursor.IsOn();o.performanceBeta=beta.IsOn();o.directory=directory.Text().c_str();validate(o,{0,0,1920,1080});return o;
    }
    void saveStorage(){
        if(localizing||syncStorage||quitting)return;
        try{auto next=read();save(config,next);options=next;issueKey=nullptr;}
        catch(...){issueKey="record.settings_failed";syncStorage=true;replayStorage.SelectedIndex(int(options.replayStorage));memoryLimit.Value(options.memoryLimitMB);syncStorage=false;}
    }
    void select(){
        if(quitting||session.busy()||selector||selectPending||pickingWindow)return;
        try{auto next=read();save(config,next);options=next;issueKey=nullptr;selectPending=true;selectAt=std::chrono::steady_clock::now()+std::chrono::milliseconds(220);ShowWindow(main,SW_MINIMIZE);}
        catch(...){issueKey="record.settings_failed";restore();}
    }
    void toggleReplay(){
        try{
            if(quitting)return;
            auto next=read();next.replayEnabled=replay.IsOn();save(config,next);options=next;issueKey=nullptr;
            if(!options.replayEnabled)replaySession.stop(true);else startReplay();
        }catch(...){issueKey="record.settings_failed";syncReplay=true;replay.IsOn(options.replayEnabled);syncReplay=false;}
    }
    void startReplay(){
        if(quitting||replaySession.busy())return;
        try{MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST),&mi))throw_last_error();replaySession.start(options,mi.rcMonitor,true);seenReplay=0;}
        catch(hresult_error const& e){issueKey="record.replay_start_failed";diagnostics::failure("replay.start",e.code(),e.message());}
        catch(...){issueKey="record.replay_start_failed";}
    }
    void showControls(){
        floatingWindow();ShowWindow(floatingHwnd,SW_SHOWNOACTIVATE);SetWindowPos(floatingHwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    fire_and_forget chooseWindow(){
        auto lifetime=shared_from_this();if(quitting||session.busy()||selector||selectPending||pickingWindow)co_return;
        pickingWindow=true;struct Guard{bool& b;~Guard(){b=false;}} guard{pickingWindow};
        try{
            struct Candidate{HWND hwnd;std::wstring label;};std::vector<Candidate> windows;
            EnumWindows([](HWND hwnd,LPARAM data)->BOOL{
                auto& list=*reinterpret_cast<std::vector<Candidate>*>(data);DWORD pid{};GetWindowThreadProcessId(hwnd,&pid);
                if(pid==GetCurrentProcessId()||!IsWindowVisible(hwnd)||(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)||GetWindowTextLengthW(hwnd)==0)return TRUE;
                DWORD cloaked{};if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return TRUE;
                wchar_t title[1024]{},path[32768]{};GetWindowTextW(hwnd,title,1024);std::wstring executable;
                HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(process){DWORD length=32768;if(QueryFullProcessImageNameW(process,0,path,&length))executable=std::filesystem::path(path).filename().wstring();CloseHandle(process);}
                list.push_back({hwnd,executable+L" · "+std::to_wstring(pid)+L" · "+title});return TRUE;
            },reinterpret_cast<LPARAM>(&windows));
            windowDialog=ContentDialog();windowDialog.XamlRoot(root.XamlRoot());windowDialog.RequestedTheme(requested);windowDialog.Title(box_value(tr("record.window_title")));windowDialog.PrimaryButtonText(tr("record.start"));windowDialog.CloseButtonText(tr("common.cancel"));windowDialog.IsPrimaryButtonEnabled(false);
            StackPanel body;body.Spacing(12);TextBlock description;description.Text(tr("record.window_hint"));description.TextWrapping(TextWrapping::Wrap);body.Children().Append(description);
            auto selected=std::make_shared<int>(-1);auto weakDialog=make_weak(windowDialog);StackPanel choices;choices.Spacing(8);
            for(size_t index=0;index<windows.size();++index){
                RadioButton choice;choice.GroupName(L"record-target");TextBlock text;text.Text(windows[index].label);text.TextWrapping(TextWrapping::Wrap);text.MaxWidth(430);choice.Content(text);
                choice.Checked([selected,weakDialog,index](auto const&,auto const&){*selected=int(index);if(auto dialog=weakDialog.get())dialog.IsPrimaryButtonEnabled(true);});choices.Children().Append(choice);
            }
            ScrollViewer scroll;scroll.MaxHeight(340);scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.Content(choices);body.Children().Append(scroll);windowDialog.Content(body);
            auto result=co_await windowDialog.ShowAsync();windowDialog=nullptr;
            if(quitting||result!=ContentDialogResult::Primary||*selected<0)co_return;
            auto target=windows.at(*selected).hwnd;RECT bounds{};if(!IsWindow(target)||!GetWindowRect(target,&bounds))throw hresult_invalid_argument();
            auto next=read();save(config,next);options=next;if(session.start(options,bounds,false,target)){seen=0;issueKey=nullptr;showControls();}
        }catch(hresult_error const& e){diagnostics::failure("recording.window",e.code(),e.message());issueKey="record.window_failed";}
        catch(...){issueKey="record.window_failed";}
    }
    void localize(){
        localizing=true;
        hint.Text(tr("record.hint"));fps.Header(box_value(tr("record.fps")));bitrate.Header(box_value(tr("record.bitrate")));seconds.Header(box_value(tr("record.seconds")));
        codec.Header(box_value(tr("record.codec")));encoder.Header(box_value(tr("record.encoder")));scaling.Header(box_value(tr("record.scaling")));scale.Header(box_value(tr("record.scale")));
        auto combo=[](ComboBox const& c,std::initializer_list<char const*> keys){auto index=c.SelectedIndex();c.Items().Clear();for(auto key:keys)c.Items().Append(box_value(tr(key)));c.SelectedIndex(index);};
        combo(codec,{"record.h264","record.h265"});combo(encoder,{"record.cpu","record.gpu"});combo(scaling,{"record.nearest","record.linear","record.cubic","record.fant"});
        combo(graphics,{"record.graphics11","record.graphics12"});graphics.Header(box_value(tr("record.graphics")));graphicsHint.Text(tr("record.dx12_hint"));
        combo(replayStorage,{"record.cache_disk","record.cache_memory"});replayStorage.Header(box_value(tr("record.cache_storage")));memoryLimit.Header(box_value(tr("record.memory_limit")));memoryHint.Text(tr("record.memory_hint"));
        microphone.Header(box_value(tr("record.microphone")));cursor.Header(box_value(tr("record.cursor")));beta.Header(box_value(tr("record.beta")));betaHint.Text(tr("record.beta_hint"));
        for(auto t:{microphone,cursor,beta,replay}){t.OnContent(box_value(tr("common.enabled")));t.OffContent(box_value(tr("common.disabled")));}
        replay.Header(box_value(tr("record.replay_toggle")));
        label(windowStart,"record.window_select");
        directory.Header(box_value(tr("record.directory")));label(browse,"record.browse");label(start,"record.select");label(stop,"record.stop");label(saveReplay,"record.save_replay");label(preview,"record.preview");label(folder,"record.open_folder");label(recovery,"record.recovery");
        label(recover,"record.recover");label(cancelSave,"record.cancel_save");
        if(floating){floating.Title(tr("record.title"));label(floatStop,"record.stop");label(floatSave,"record.save_replay");label(floatOpen,"record.open");}
        if(previewWindow)previewWindow.Title(tr("record.preview"));
        localizing=false;
    }
    void floatingWindow(){
        if(floating)return;
        floating=Window();floating.Title(tr("record.title"));check_hresult(floating.as<::IWindowNative>()->get_WindowHandle(&floatingHwnd));
        auto p=Microsoft::UI::Windowing::OverlappedPresenter::Create();p.SetBorderAndTitleBar(false,false);p.IsAlwaysOnTop(true);p.IsResizable(false);p.IsMaximizable(false);p.IsMinimizable(false);floating.AppWindow().SetPresenter(p);
        floatingRoot=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="12" Padding="12"/>)").as<Grid>();floatingRoot.RequestedTheme(requested);
        StackPanel stack;stack.Spacing(10);floatState.TextWrapping(TextWrapping::Wrap);stack.Children().Append(floatState);StackPanel row;row.Orientation(Orientation::Horizontal);row.Spacing(8);for(auto b:{floatPause,floatStop,floatSave,floatOpen})row.Children().Append(b);stack.Children().Append(row);floatingRoot.Children().Append(stack);floating.Content(floatingRoot);
        floatPause.Click([this](auto const&,auto const&){session.pause();});floatStop.Click([this](auto const&,auto const&){session.stop();});floatSave.Click([this](auto const&,auto const&){saveCurrentReplay();});floatOpen.Click([this](auto const&,auto const&){restore();});
        auto weak=weak_from_this();floating.Closed([weak](auto const&,WindowEventArgs const& e){if(auto self=weak.lock();self&&!self->closing){e.Handled(true);if(self->session.busy())self->restore();else self->floating.AppWindow().Hide();}});
        SetWindowDisplayAffinity(floatingHwnd,WDA_EXCLUDEFROMCAPTURE);
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST),&mi);auto dpi=GetDpiForWindow(main);const auto width=MulDiv(340,dpi,96),height=MulDiv(96,dpi,96),margin=MulDiv(20,dpi,96);floating.AppWindow().MoveAndResize({mi.rcWork.right-width-margin,mi.rcWork.top+margin,width,height});localize();
    }
    fire_and_forget showPreview(){
        auto lifetime=shared_from_this();if(closing||previewLoading)co_return;
        previewLoading=true;struct LoadingGuard{bool& value;~LoadingGuard(){value=false;}} loadingGuard{previewLoading};
        auto output=latestOutput;if(output.empty())co_return;
        try{
            if(!previewWindow){previewWindow=Window();previewWindow.Title(tr("record.preview"));Grid g=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>)").as<Grid>();g.RequestedTheme(requested);player=MediaPlayerElement();player.AreTransportControlsEnabled(true);g.Children().Append(player);previewWindow.Content(g);previewWindow.AppWindow().Resize({960,600});auto weak=weak_from_this();previewWindow.Closed([weak](auto const&,WindowEventArgs const& e){if(auto self=weak.lock();self&&!self->closing){e.Handled(true);if(self->mediaPlayer)self->mediaPlayer.Pause();self->previewWindow.AppWindow().Hide();}});}
            auto file=co_await Windows::Storage::StorageFile::GetFileFromPathAsync(output.wstring());
            if(closing)co_return;
            if(player)player.SetMediaPlayer(nullptr);if(mediaPlayer)mediaPlayer.Close();mediaPlayer=Windows::Media::Playback::MediaPlayer();
            auto weak=weak_from_this();auto queue=root.DispatcherQueue();mediaPlayer.MediaFailed([weak,queue](auto const&,auto const&){queue.TryEnqueue([weak]{if(auto self=weak.lock();self&&!self->closing)self->issueKey="record.preview_failed";});});
            mediaPlayer.Source(Windows::Media::Core::MediaSource::CreateFromStorageFile(file));player.SetMediaPlayer(mediaPlayer);previewWindow.Activate();mediaPlayer.Play();
        }catch(...){if(!closing)issueKey="record.preview_failed";}
    }
    void tick(){
        // A modal picker owns focus and its automation tree until it closes.
        // Repeatedly disabling/relabeling its underlying page can reenter XAML
        // focus/automation processing. Recording workers continue independently.
        if(pickingWindow&&!quitting)return;
        if(autoReplay&&!quitting){autoReplay=false;startReplay();}
        if(selectPending&&std::chrono::steady_clock::now()>=selectAt){selectPending=false;try{selector=std::make_unique<Selector>(captureHost);}catch(...){issueKey="record.select_failed";restore();}}
        if(selector)selector->poll();
        if(selector&&selector->done){auto selected=selector->result;auto error=selector->error;selector.reset();if(FAILED(error))issueKey="record.select_failed";if(selected){try{session.start(options,*selected,false);seen=0;region.Text(std::to_wstring(selected->left)+L", "+std::to_wstring(selected->top)+L" · "+std::to_wstring(selected->right-selected->left)+L" × "+std::to_wstring(selected->bottom-selected->top));showControls();}catch(...){issueKey="record.start_failed";restore();}}else if(!quitting)restore();}
        auto s=session.snapshot();const bool running=s.state==State::Recording||s.state==State::Paused;const bool busy=session.busy();
        auto rs=replaySession.snapshot();if(!quitting)notification.update(rs,requested);
        auto replayText=std::wstring(tr(rs.state==State::Failed?"record.replay_start_failed":rs.state==State::Starting?"record.starting":rs.state==State::Stopping?"record.stopping":rs.state==State::Recording?"record.buffering":"record.replay_off"));
        if(rs.state==State::Recording)replayText+=L" · "+elapsed(rs.bufferedSeconds)+L" · "+std::to_wstring(rs.width)+L" × "+std::to_wstring(rs.height);
        if(rs.captureWaiting)replayText+=L"\n"+std::wstring(tr("record.capture_waiting"));
        if(rs.memoryReplay){replayText+=L"\n"+std::wstring(tr("record.memory_usage"))+std::to_wstring((rs.cacheBytes+1048575)/1048576);if(rs.memoryLimited)replayText+=L"\n"+std::wstring(tr("record.memory_limited"));if(rs.error==E_OUTOFMEMORY||rs.error==STG_E_MEDIUMFULL)replayText+=L"\n"+std::wstring(tr("record.memory_failed"));}
        if(rs.saving)replayText+=L" · "+std::wstring(tr("record.saving"));if(FAILED(rs.error)){std::wostringstream code;code<<L" · 0x"<<std::hex<<unsigned(rs.error);replayText+=code.str();}replayState.Text(replayText);
        for(auto const& child:settings.Children()) {
            if(auto control=child.try_as<Control>())control.IsEnabled(!quitting&&!busy&&!selector&&!selectPending&&!pickingWindow);
            if(auto row=child.try_as<StackPanel>())for(auto const& item:row.Children())if(auto control=item.try_as<Control>())control.IsEnabled(!quitting&&!busy&&!selector&&!selectPending&&!pickingWindow);
        }
        beta.IsEnabled(!quitting&&!busy&&encoder.SelectedIndex()==1);
        replayStorage.IsEnabled(!quitting&&!busy&&!replaySession.busy());memoryLimit.IsEnabled(replayStorage.IsEnabled()&&replayStorage.SelectedIndex()==1);memoryHint.Visibility(replayStorage.SelectedIndex()==1?Visibility::Visible:Visibility::Collapsed);
        start.IsEnabled(!quitting&&!busy&&!selector&&!selectPending&&!pickingWindow);windowStart.IsEnabled(start.IsEnabled()&&graphics.SelectedIndex()==0);replay.IsEnabled(!quitting&&rs.state!=State::Stopping);syncReplay=true;replay.IsOn(options.replayEnabled);syncReplay=false;pause.IsEnabled(!quitting&&running);stop.IsEnabled(!quitting&&(running||s.state==State::Starting));saveReplay.IsEnabled(!quitting&&rs.state==State::Recording&&!rs.saving&&rs.frames>0);
        label(pause,s.state==State::Paused?"record.resume":"record.pause");progress.IsActive(s.state==State::Starting||s.state==State::Stopping||s.saving);progress.Visibility(progress.IsActive()?Visibility::Visible:Visibility::Collapsed);
        const char* key=s.state==State::Starting?"record.starting":s.state==State::Paused?"record.paused":s.state==State::Stopping?"record.stopping":s.state==State::Failed?"record.failed":s.state==State::Completed?"record.completed":s.state==State::Recording?(s.replay?"record.buffering":"record.recording"):"record.idle";
        auto text=std::wstring(tr(key))+L" · "+elapsed(s.seconds);if(s.replay)text+=L" · "+std::wstring(tr("record.buffer"))+elapsed(s.bufferedSeconds);if(s.saving)text+=L" · "+std::wstring(tr("record.saving"))+L" "+std::to_wstring(unsigned(s.saveProgress*100))+L"%";
        cancelSave.Visibility(rs.saving?Visibility::Visible:Visibility::Collapsed);recover.IsEnabled(!quitting&&!replaySession.busy());
        if(s.error==E_ABORT)text+=L"\n"+std::wstring(tr("record.save_canceled"));
        else if(FAILED(s.error)){std::wostringstream error;error<<L" · 0x"<<std::hex<<unsigned(s.error);text+=error.str();text+=L"\n"+std::wstring(tr(running?"record.save_failed":"record.codec_hint"));}
        if(issueKey)text+=L"\n"+std::wstring(tr(issueKey));state.Text(text);
        if(s.captureWaiting){text+=L"\n"+std::wstring(tr("record.capture_waiting"));state.Text(text);}
        if(quitting)state.Text(tr("record.exiting"));
        std::wstring detail=s.output.wstring();if(s.width)detail+=L"\n"+std::to_wstring(s.width)+L" × "+std::to_wstring(s.height)+L" · "+tr("record.dropped")+std::to_wstring(s.dropped);details.Text(detail);
        if(s.width){detail+=L"\n"+std::wstring(tr(s.optimizedVendor==0x10de?"record.optimized_nvenc":s.optimizedVendor==0x1002?"record.optimized_amf":FAILED(s.optimizationError)?"record.fallback":"record.standard"));if(!s.adapter.empty())detail+=L" · "+s.adapter;if(s.optimizedVendor==0x10de)detail+=L"\n"+std::wstring(tr(s.cuda?"record.cuda_available":"record.cuda_unavailable"));if(FAILED(s.optimizationError)){std::wostringstream code;code<<L" · 0x"<<std::hex<<unsigned(s.optimizationError);detail+=code.str();}details.Text(detail);}
        if(s.nativeDx12){detail+=L"\n"+std::wstring(tr("record.graphics12"));details.Text(detail);}
        if(s.windowCapture){detail+=L"\n"+s.windowTitle+L" · "+std::to_wstring(s.processId);if(s.windowMinimized)detail+=L"\n"+std::wstring(tr("record.window_minimized"));details.Text(detail);}
        preview.IsEnabled(!quitting&&!latestOutput.empty());folder.IsEnabled(!latestOutput.empty());recovery.IsEnabled(!s.recovery.empty()||!rs.recovery.empty());
        if(floating){floatState.Text(std::wstring(tr(key))+L" · "+elapsed(s.seconds));floatPause.IsEnabled(pause.IsEnabled());floatStop.IsEnabled(stop.IsEnabled());floatSave.IsEnabled(saveReplay.IsEnabled());floatSave.Visibility(s.replay?Visibility::Visible:Visibility::Collapsed);label(floatPause,s.state==State::Paused?"record.resume":"record.pause");if(!busy||s.replay)ShowWindow(floatingHwnd,SW_HIDE);}
        if(s.savedGeneration!=seen){seen=s.savedGeneration;latestOutput=s.output;if(!quitting){restore();showPreview();}}
        if(rs.savedGeneration!=seenReplay){seenReplay=rs.savedGeneration;latestOutput=rs.output;}
        if(!quitting&&s.state==State::Failed&&lastState!=s.state)restore();lastState=s.state;
    }
    State lastState{State::Idle};
};
Panel::Panel(HWND h,std::filesystem::path const& p,std::function<void()> r,std::function<HWND()> host):impl_(std::make_shared<Impl>(h,p,std::move(r),std::move(host))){impl_->beginTick();}
Panel::~Panel(){impl_->dispose();}
UIElement Panel::content()const{return impl_->root;}
void Panel::localize(){impl_->localize();}
void Panel::theme(ElementTheme t){impl_->requested=t;if(impl_->floatingRoot)impl_->floatingRoot.RequestedTheme(t);if(impl_->previewWindow)impl_->previewWindow.Content().as<FrameworkElement>().RequestedTheme(t);}
void Panel::toggle(){if(impl_->quitting)return;if(impl_->session.busy())impl_->session.stop();else impl_->select();}
void Panel::replaySave(){impl_->saveCurrentReplay();}
bool Panel::busy()const{return impl_->session.busy()||impl_->replaySession.busy()||impl_->selectPending||bool(impl_->selector)||impl_->pickingWindow;}
Windows::Foundation::IAsyncAction Panel::shutdown(){
    auto self=impl_;self->quitting=true;self->autoReplay=false;self->selectPending=false;self->selector.reset();if(self->windowDialog)self->windowDialog.Hide();
    self->session.stop();self->replaySession.stop(true);self->tick();auto ui=apartment_context();
    co_await resume_background();self->session.wait();self->replaySession.wait();co_await ui;self->dispose();
}
}
