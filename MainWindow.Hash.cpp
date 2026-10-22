#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include <shobjidl.h>
#include <wil/resource.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
namespace {
using aelura::hash::Mode;
using aelura::hash::State;
using aelura::hash::Status;
std::wstring progressText(uint64_t done, uint64_t total) {
    std::wstring value = tr("hash.progress");
    value.replace(value.find(L"{0}"), 3, std::to_wstring(done));
    value.replace(value.find(L"{1}"), 3, std::to_wstring(total));
    return value;
}
}
bool MainWindow::AcquireFileTask(FileTask task) {
    if(recordingCloseBusy_)return false;
    // All entry points (including hotkeys) acquire this gate on the UI thread.
    if (fileTask_ != FileTask::None) { Notice(tr("files.busy"), true); fileTaskNotice_ = true; return false; }
    fileTask_ = task; RenderHash(); return true;
}
void MainWindow::ReleaseFileTask() {
    fileTask_ = FileTask::None;
    if (fileTaskNotice_) { Status().IsOpen(false); fileTaskNotice_ = false; }
    RenderHash();
}
void MainWindow::SetupHash() {
    HashMode().SelectionChanged([this](auto const&, auto const&) { InvalidateHash(); });
    HashFirstPath().TextChanged([this](auto const&, auto const&) { InvalidateHash(); });
    HashSecondPath().TextChanged([this](auto const&, auto const&) { InvalidateHash(); });
    HashExpected().TextChanged([this](auto const&, auto const&) { InvalidateHash(); });
    HashFirstPick().Click([this](auto const&, auto const&) { PickHash(false); });
    HashSecondPick().Click([this](auto const&, auto const&) { PickHash(true); });
    HashStart().Click([this](auto const&, auto const&) { RunHash(); });
    HashCancel().Click([this](auto const&, auto const&) { CancelHash(); });
    HashTaskCancel().Click([this](auto const&, auto const&) { CancelHash(); });
    HashTaskOpen().Click([this](auto const&, auto const&) { Page(4); });
    HashFirstCopy().Click([this](auto const&, auto const&) { CopyHash(false); });
    HashSecondCopy().Click([this](auto const&, auto const&) { CopyHash(true); });
    hashTimer_ = DispatcherQueue().CreateTimer(); hashTimer_.Interval(std::chrono::milliseconds(120));
    hashTimer_.Tick([this](auto const&, auto const&) { RenderHash(); });
}
void MainWindow::LocalizeHash() {
    auto mode = HashMode().SelectedIndex();
    HashHint().Text(tr("hash.hint"));
    HashMode().Header(box_value(tr("hash.mode")));
    HashCalculateMode().Content(box_value(tr("hash.mode_calculate")));
    HashExpectedMode().Content(box_value(tr("hash.mode_expected")));
    HashCompareMode().Content(box_value(tr("hash.mode_compare")));
    HashMode().SelectedIndex(-1); HashMode().SelectedIndex(mode);
    HashFirstPath().Header(box_value(tr("hash.first_file")));
    HashFirstPath().PlaceholderText(tr("hash.path_placeholder"));
    HashSecondPath().Header(box_value(tr("hash.second_file")));
    HashSecondPath().PlaceholderText(tr("hash.path_placeholder"));
    HashFirstPick().Content(box_value(tr("hash.choose_first")));
    HashSecondPick().Content(box_value(tr("hash.choose_second")));
    HashExpected().Header(box_value(tr("hash.expected")));
    HashExpected().PlaceholderText(tr("hash.expected_placeholder"));
    HashStart().Content(box_value(tr("hash.start")));
    HashCancel().Content(box_value(tr("common.cancel")));
    HashTaskOpen().Content(box_value(tr("hash.view_task")));
    HashTaskCancel().Content(box_value(tr("common.cancel")));
    HashFirstResult().Header(box_value(tr("hash.first_digest")));
    HashSecondResult().Header(box_value(tr("hash.second_digest")));
    HashFirstCopy().Content(box_value(tr("hash.copy_first")));
    HashSecondCopy().Content(box_value(tr("hash.copy_second")));
    RenderHash();
}
void MainWindow::InvalidateHash() {
    if (localizing_) return;
    hashSession_.invalidate();
    Status().IsOpen(false);
    RenderHash();
}
void MainWindow::CancelHash() { hashSession_.cancel(); RenderHash(); }
void MainWindow::RenderHash() {
    const bool busy = hashSession_.busy(), cancelling = hashSession_.state() == State::Cancelling;
    const bool compare = HashMode().SelectedIndex() == 2;
    HashExpectedPanel().Visibility(HashMode().SelectedIndex() == 1 ? Visibility::Visible : Visibility::Collapsed);
    HashSecondPanel().Visibility(compare ? Visibility::Visible : Visibility::Collapsed);
    HashMode().IsEnabled(!busy); HashFirstPath().IsEnabled(!busy); HashSecondPath().IsEnabled(!busy); HashExpected().IsEnabled(!busy);
    HashFirstPick().IsEnabled(!busy); HashSecondPick().IsEnabled(!busy);
    HashStart().IsEnabled(fileTask_ == FileTask::None && !busy);
    HashCancel().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    HashCancel().IsEnabled(!cancelling); HashTaskCancel().IsEnabled(!cancelling);
    HashTaskStrip().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    HashTaskText().Text(tr(cancelling ? "hash.cancelling" : "hash.running"));
    HashProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    auto work = hashSession_.work();
    const auto total = work ? work->total.load() : 0, done = work ? work->done.load() : 0;
    HashProgress().IsIndeterminate(busy && !total);
    HashProgress().Value(total ? (std::min)(99.0, 100.0 * static_cast<double>(done) / static_cast<double>(total)) : 0);
    auto const& result = hashSession_.result();
    const bool success = hashSession_.state() == State::Finished && result.status == Status::Success;
    HashResults().Visibility(success ? Visibility::Visible : Visibility::Collapsed);
    HashFirstCopy().IsEnabled(success); HashSecondCopy().IsEnabled(success && compare);
    HashFirstResult().Text(success ? result.first : L""); HashSecondResult().Text(success ? result.second : L"");
    HashSecondResult().Visibility(success && compare ? Visibility::Visible : Visibility::Collapsed);
    HashSecondCopy().Visibility(success && compare ? Visibility::Visible : Visibility::Collapsed);
    std::wstring state = tr("hash.idle");
    if (busy) state = std::wstring(tr(cancelling ? "hash.cancelling" : "hash.running")) + L"\n" + progressText(done, total);
    else if (hashSession_.state() == State::Finished) {
        switch (result.status) {
        case Status::Success:
            state = tr(HashMode().SelectedIndex() == 0 ? "hash.complete" : result.matches ? "hash.match" : "hash.mismatch"); break;
        case Status::Cancelled: state = tr("hash.cancelled"); break;
        case Status::InvalidExpected: state = tr("hash.invalid_expected"); break;
        case Status::InvalidPath: state = tr("hash.invalid_path"); break;
        case Status::UnsupportedFile: state = tr("hash.unsupported"); break;
        case Status::OpenFailed:
            state = tr(result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND ? "hash.missing" :
                result.error == ERROR_ACCESS_DENIED ? "hash.denied" : result.error == ERROR_SHARING_VIOLATION || result.error == ERROR_LOCK_VIOLATION ? "hash.locked" : "hash.open_failed"); break;
        case Status::ReadFailed: state = tr("hash.read_failed"); break;
        case Status::Changed: state = tr("hash.changed"); break;
        case Status::CryptoFailed: state = tr("hash.crypto_failed"); break;
        }
    }
    if (HashState().Text() != state) HashState().Text(state);
}
void MainWindow::PickHash(bool second) {
    if (hashSession_.busy()) return;
    try {
        com_ptr<IFileOpenDialog> dialog; check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{}; check_hresult(dialog->GetOptions(&options));
        check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST));
        const auto choice = dialog->Show(hwnd_); if (choice == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return; check_hresult(choice);
        com_ptr<IShellItem> item; check_hresult(dialog->GetResult(item.put()));
        wil::unique_cotaskmem_string path; check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()));
        (second ? HashSecondPath() : HashFirstPath()).Text(path.get());
    } catch (hresult_error const&) { Notice(tr("hash.pick_failed"), true); }
}
void MainWindow::CopyHash(bool second) {
    auto const& result = hashSession_.result();
    if (hashSession_.state() != State::Finished || result.status != Status::Success || (second && result.second.empty())) return;
    try {
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(second ? result.second : result.first);
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        Notice(tr("hash.copied"));
    } catch (hresult_error const&) { Notice(tr("hash.copy_failed"), true); }
}
fire_and_forget MainWindow::RunHash() {
    auto lifetime = get_strong();
    if(recordingCloseBusy_)co_return;
    if (fileTask_ != FileTask::None) { Notice(tr("files.busy"), true); fileTaskNotice_ = true; co_return; }
    aelura::hash::Request request;
    request.mode = static_cast<Mode>(HashMode().SelectedIndex());
    request.first = HashFirstPath().Text().c_str(); request.second = HashSecondPath().Text().c_str(); request.expected = HashExpected().Text();
    auto work = hashSession_.begin(std::move(request)); if (!work) co_return;
    AcquireFileTask(FileTask::Hash); Status().IsOpen(false); hashTimer_.Start(); RenderHash();
    auto queue = DispatcherQueue();
    co_await resume_background();
    auto result = aelura::hash::compute(work->request, [work](uint64_t done, uint64_t total) {
        work->total = total; work->done = done; return !work->cancel.load();
    });
    co_await wil::resume_foreground(queue);
    hashSession_.finish(work, std::move(result)); hashTimer_.Stop(); ReleaseFileTask();
}
}
