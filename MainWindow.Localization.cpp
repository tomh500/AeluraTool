#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/I18n.h"
#include "Integration/CaptureProtocol.h"
#include "Integration/ToolLanguageProtocol.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
void MainWindow::ApplyLanguage() {
    localizing_ = true;
    auto themeIndex = ThemeChoice().SelectedIndex(); auto languageIndex = LanguageChoice().SelectedIndex();
    Title(tr("app.title"));
    Localized1().Text(tr("app.brand"));
    HomeNav().Content(box_value(tr("nav.all")));
    Localized2().Text(tr("nav.categories"));
    ImageNav().Content(box_value(tr("nav.images")));
    FilesNav().Content(box_value(tr("nav.files")));
    GamesNav().Content(box_value(tr("nav.games")));
    KeysNav().Content(box_value(tr("nav.keys")));
    PreferencesNav().Content(box_value(tr("nav.settings")));
    PageTitle().Text(tr("nav.all"));
    PageDescription().Text(tr("home.description"));
    Search().PlaceholderText(tr("search.tools"));
    Automation::AutomationProperties::SetName(Search(), tr("search.accessible"));
    NoResults().Text(tr("search.none"));
    KeySearch().PlaceholderText(tr("search.keys"));
    Automation::AutomationProperties::SetName(KeySearch(), tr("search.keys.accessible"));
    NoKeys().Text(tr("search.no_keys"));
    Localized3().Text(tr("keys.hint"));
    Localized4().Text(tr("settings.appearance"));
    ThemeChoice().Header(box_value(tr("settings.theme")));
    Localized5().Content(box_value(tr("settings.system")));
    Localized6().Content(box_value(tr("settings.light")));
    Localized7().Content(box_value(tr("settings.dark")));
    LanguageChoice().Header(box_value(tr("settings.language")));
    Localized8().Content(box_value(tr("settings.auto_language")));
    Localized9().Content(box_value(tr("settings.chinese")));
    Localized10().Content(box_value(tr("settings.english")));
    StartupMotionSwitch().Header(box_value(tr("settings.motion")));
    Localized11().Text(tr("settings.motion_hint"));
    PreviewMotionButton().Content(box_value(tr("settings.preview_motion")));
    Localized12().Text(tr("app.brand"));
    LicensesButton().Content(box_value(tr("settings.licenses")));
    Localized13().Text(tr("move.hint"));
    SourcePath().Header(box_value(tr("move.source")));
    SourcePath().PlaceholderText(tr("move.source_placeholder"));
    SourceFileButton().Content(box_value(tr("move.file")));
    SourceFolderButton().Content(box_value(tr("move.folder")));
    DestinationPath().Header(box_value(tr("move.destination")));
    DestinationPath().PlaceholderText(tr("move.destination_placeholder"));
    DestinationButton().Content(box_value(tr("move.choose_destination")));
    CheckMoveButton().Content(box_value(tr("move.inspect")));
    MoveButton().Content(box_value(tr("move.start")));
    CancelMoveButton().Content(box_value(tr("common.cancel")));
    StartupName().Text(tr("app.brand"));
    LocalizeHash();
    // Refresh the selected-item presenter; changing an item's content alone leaves its cached text behind.
    ThemeChoice().SelectedIndex(-1); ThemeChoice().SelectedIndex(themeIndex);
    LanguageChoice().SelectedIndex(-1); LanguageChoice().SelectedIndex(languageIndex);
    StartupMotionSwitch().OnContent(box_value(tr("common.enabled")));
    StartupMotionSwitch().OffContent(box_value(tr("common.disabled")));
    Root().Language(aelura::i18n::chinese() ? L"zh-CN" : L"en-US");
    hotkeys_.localize();
    if (auto engine = EngineWindow()) { DWORD_PTR result{}; SendMessageTimeoutW(engine, Aelura::Capture::Message, Aelura::Capture::SetLanguage, aelura::i18n::chinese() ? 1 : 2, SMTO_ABORTIFHUNG, 500, &result); }
    if (strike_ && WaitForSingleObject(strike_, 0) == WAIT_TIMEOUT) {
        struct LanguageState { DWORD pid; WPARAM language; } state{GetProcessId(strike_), aelura::i18n::chinese() ? 1u : 2u};
        EnumWindows([](HWND w, LPARAM data)->BOOL { auto const& s = *reinterpret_cast<LanguageState*>(data); DWORD pid{}; GetWindowThreadProcessId(w, &pid); if (pid == s.pid && GetPropW(w, L"Aelura.Strike.Main")) { DWORD_PTR result{}; SendMessageTimeoutW(w, aelura::ToolLanguageMessage, s.language, 0, SMTO_ABORTIFHUNG, 500, &result); } return TRUE; }, reinterpret_cast<LPARAM>(&state));
    }
    if (!moveBusy_) { MoveSummary().Text(L""); Status().IsOpen(false); }
    localizing_ = false;
    Page(page_);
}
}
