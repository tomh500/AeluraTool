#include "app.h"
#include "../../Integration/CaptureProtocol.h"
#include "ui.h"
#include "Resource.h"
#include "capture.h"
#include "storage.h"
#include "sharing.h"
#include "preview.h"
#include "config.h"
#include "settings.h"
#include "dragdrop.h"
#include "hotkeys.h"
#include "editor.h"
#include "tray.h"
#include "instance.h"
#include "visual_effects.h"
#include "launch_scene.h"
#include <thread>
using namespace std;
using namespace Gdiplus;
namespace {
    struct App {
        enum Phase { Idle, Selecting, Saving, Dragging, Editing } phase = Idle;
        HINSTANCE instance = nullptr;
        HWND panel = nullptr;
        TrayIcon tray;
        HICON smallIcon = nullptr;
        wstring notice = tr("screen.app.001");
        wstring trayWarning;
        POINT mouse{ -1, -1 }, pressed{};
        bool mouseDown = false;
        Config config;
        filesystem::path configFile;
        bool allowPortableConfig = true;
        wstring startupKey;
        Hotkeys hotkeys;
        bool exiting = false;
        UINT taskbarCreated = 0;
        unique_ptr<Bitmap> latest, pending;
        filesystem::path latestFile, shareFile;
        POINT latestPoint{}, pendingPoint{};
        RECT latestOrigin{}, pendingOrigin{};
        bool restorePreview = false;
        bool launching = false, launchStarted = false, initialized = false;
        bool secret = false;
        unsigned logoClicks = 0, secretLetters = 0;
        ULONGLONG logoTime = 0, letterTime = 0, exitTime = 0;
        double exitSeconds = 0;
    } app;
    const array<RectF, 5> buttons{ { { 24, 172, 372, 52 },{ 24, 236, 372, 44 },
        { 24, 292, 372, 44 },{ 24, 348, 372, 44 },{ 24, 404, 372, 44 } } };
    const RectF closeButton{ 372, 16, 28, 28 };
    const RectF retryButton{ 24, 460, 372, 44 };
    constexpr UINT_PTR trayRetryTimer = 4;
    constexpr UINT_PTR exitTimer = 5;
    constexpr UINT exitFinishedMessage = WM_APP + 80;
    const RectF logoButton{ 24, 24, 40, 40 }, launchBadge{ 167, 168, 86, 86 };
    float Ease(float p) { p = clamp(p, 0.0f, 1.0f); return 1 - pow(1 - p, 3.0f); }
    void ConfigureUi() {
        ui::SetMotionOptions(app.config.animationLevel, app.config.animationSpeed); ui::SetAccent(app.config.accentColor);
        ConfigureCapture(app.config.captureShade, app.config.captureGuides, app.config.captureAnimation, app.config.captureWindowSelection);
        ConfigurePreview(app.config);
    }
    void EndLaunch() {
        app.launching = false;
        RemovePropW(app.panel, L"ScreenShotPlus.StartupAnimating");
        RemovePropW(app.panel, L"ScreenShotPlus.StartupPhase");
        RemovePropW(app.panel, L"ScreenShotPlus.SecretScene");
        ui::ContinuousMotion(app.panel, false);
    }
    void RevealSecret() {
        if (app.config.animationLevel < 4 || !ui::DecorativeMotion(4) || app.exiting) return;
        app.secret = true;
        if (!app.launching) {
            app.launching = app.launchStarted = true;
            ui::RestartMotion(app.panel, LaunchSeconds(4));
            SetPropW(app.panel, L"ScreenShotPlus.StartupAnimating", reinterpret_cast<HANDLE>(1));
        }
        SetPropW(app.panel, L"ScreenShotPlus.SecretScene", reinterpret_cast<HANDLE>(1));
        InvalidateRect(app.panel, nullptr, FALSE);
    }
    void BeginExit() {
        if (app.exiting) return;
        if (app.config.animationLevel < 4 || !ui::DecorativeMotion(4) || !IsWindowVisible(app.panel)) {
            DestroyWindow(app.panel); return;
        }
        EndLaunch(); app.exiting = true; app.mouseDown = false;
        if (GetCapture() == app.panel) ReleaseCapture();
        HidePreview(); KillTimer(app.panel, trayRetryTimer);
        ReleaseHotkeys(app.panel, app.hotkeys);
        app.exitTime = GetTickCount64(); app.exitSeconds = ui::MotionDuration(.82);
        SetPropW(app.panel, L"ScreenShotPlus.ExitAnimating", reinterpret_cast<HANDLE>(1));
        ui::ContinuousMotion(app.panel, true);
        // Destruction has its own clock; it must not depend on WM_PAINT delivery.
        if (!SetTimer(app.panel, exitTimer, 16, nullptr)) { DestroyWindow(app.panel); return; }
        InvalidateRect(app.panel, nullptr, FALSE);
    }
    void ShowPanel() {
        if (app.exiting) return;
        if (app.phase == App::Editing) { FocusEditor(); return; }
        if (app.phase != App::Idle) return;
        if (SettingsVisible()) { ShowSettings(app.config); return; }
        ui::Place(app.panel, 420, 570); ShowWindow(app.panel, SW_SHOW);
        if (app.launching && !app.launchStarted) {
            ui::RestartMotion(app.panel, LaunchSeconds(app.config.animationLevel)); app.launchStarted = true;
        }
        SetForegroundWindow(app.panel); InvalidateRect(app.panel, nullptr, FALSE);
    }
    void Notice(const wstring& text) { app.notice = text; ShowPanel(); }
    void LatestPreview(bool captured = false) {
        if (app.latest) ShowPreview(app.latest.get(), app.latestFile, app.latestPoint, app.config.previewSeconds, captured ? app.latestOrigin : RECT{});
        else Notice(tr("screen.app.002"));
    }
    void CopyAndPreview(bool captured = false) {
        const bool copied = app.latest && CopyImageToClipboard(*app.latest, app.panel);
        LatestPreview(captured); PreviewCopyResult(copied);
    }
    void SavePending() {
        if (!app.pending || app.phase != App::Idle) return;
        if (app.config.manualSaveOnly) {
            HidePreview(); app.latest = move(app.pending); app.latestFile.clear(); app.shareFile.clear();
            app.latestPoint = app.pendingPoint; app.latestOrigin = app.pendingOrigin;
            app.notice = tr("screen.app.003");
            ShowWindow(app.panel, SW_HIDE); CopyAndPreview(true); return;
        }
        app.phase = App::Saving;
        filesystem::path file; wstring error;
        const bool ok = SavePng(*app.pending, app.config.folder, file, error);
        app.phase = App::Idle;
        if (!ok) { Notice(error); return; }
        HidePreview(); app.latest = move(app.pending); app.latestFile = move(file); app.shareFile.clear();
        app.latestPoint = app.pendingPoint;
        app.latestOrigin = app.pendingOrigin;
        app.notice = tr("screen.app.004");
        ShowWindow(app.panel, SW_HIDE); CopyAndPreview(true);
    }
    void StartCapture() {
        if (app.phase != App::Idle || SettingsVisible() || app.exiting) return;
        EndLaunch();
        app.restorePreview = PreviewVisible();
        ShowWindow(app.panel, SW_HIDE); HidePreview(); DwmFlush();
        app.phase = App::Selecting;
        wstring error;
        if (!BeginSelection(app.instance, [](unique_ptr<Bitmap> image, POINT point, wstring failure) {
            if (app.exiting) return;
            app.phase = App::Idle;
            if (!image) {
                if (app.restorePreview && app.latest) LatestPreview();
                if (!failure.empty()) Notice(failure);
                return;
            }
            app.pending = move(image); app.pendingPoint = point; app.pendingOrigin = LastCaptureRect(); SavePending();
            }, error)) {
            app.phase = App::Idle;
            if (app.restorePreview && app.latest) LatestPreview();
            Notice(error);
        }
    }
    void DragLatest() {
        if (app.phase != App::Idle || !app.latest || SettingsVisible() || app.exiting) return;
        auto file = app.latestFile;
        if (file.empty()) {
            error_code ec;
            if (app.shareFile.empty() || !filesystem::is_regular_file(app.shareFile, ec)) {
                wstring error;
                if (!SaveTemporaryImage(*app.latest, ImageShareCache(), app.shareFile, error)) { Notice(error); return; }
            }
            file = app.shareFile;
        }
        app.phase = App::Dragging;
        if (app.config.dragAnimation && ui::DecorativeMotion()) BeginDragVisual(*app.latest, PreviewImageScreenRect());
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT result = DragFile(file, effect, DragVisualFeedback);
        const bool accepted = result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY;
        EndDragVisual(accepted); app.phase = App::Idle; PreviewDragResult(accepted);
        if (FAILED(result)) Notice(tr("screen.app.005"));
        else {
            app.notice = result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY ? tr("screen.app.006") : tr("screen.app.007");
            InvalidateRect(app.panel, nullptr, FALSE);
        }
    }
    void EditLatest() {
        if (app.phase != App::Idle || !app.latest || SettingsVisible() || app.exiting) return;
        app.phase = App::Editing;
        ShowWindow(app.panel, SW_HIDE); HidePreview();
        wstring error;
        if (!ShowEditor(*app.latest, error)) { app.phase = App::Idle; LatestPreview(); Notice(error); }
    }
    void LocateLatest() {
        if (app.phase != App::Idle || !app.latest || app.latestFile.empty() || SettingsVisible() || app.exiting) return;
        wstring error;
        if (!ui::RevealFile(app.latestFile, error)) Notice(error);
    }
    bool SaveEdited(unique_ptr<Bitmap>& image, wstring& error) {
        if (app.exiting || app.phase != App::Editing || !image) return false;
        filesystem::path file;
        if (!SavePng(*image, app.config.folder, file, error)) return false;
        HidePreview(); app.latest = move(image); app.latestFile = move(file); app.shareFile.clear();
        app.phase = App::Idle; app.notice = tr("screen.app.008");
        CopyAndPreview(); return true;
    }
    void CancelEdit() {
        if (app.exiting) return;
        app.phase = App::Idle; LatestPreview();
    }
    void DismissLatest() {
        if (app.phase == App::Idle && app.latestFile.empty()) {
            app.latest.reset(); app.shareFile.clear(); app.latestOrigin = {}; app.restorePreview = false;
            app.notice = tr("screen.app.009");
            InvalidateRect(app.panel, nullptr, FALSE);
        }
    }
    bool ApplyConfig(const Config& next, wstring& error) {
        filesystem::path destination;
        if (!ApplyConfiguredSettings(app.panel, app.hotkeys, app.config, next, app.configFile, error, app.startupKey.c_str(),
            app.allowPortableConfig ? PortableConfigFile() : filesystem::path{}, &destination)) return false;
        app.configFile = move(destination); SetSettingsConfigFile(app.configFile);
        ConfigureUi();
        if (PreviewVisible()) LatestPreview();
        return true;
    }
    void AddTray() {
        if (RegisterTrayIcon(app.tray)) {
            SetPropW(app.panel, L"ScreenShotPlus.TrayRegistered", reinterpret_cast<HANDLE>(1));
            KillTimer(app.panel, trayRetryTimer);
            if (!app.trayWarning.empty() && app.notice == app.trayWarning) app.notice = tr("screen.app.010");
            app.trayWarning.clear();
        }
        else {
            RemovePropW(app.panel, L"ScreenShotPlus.TrayRegistered");
            SetTimer(app.panel, trayRetryTimer, 2000, nullptr);
            app.trayWarning = TrayFailureMessage(app.tray); app.notice = app.trayWarning;
            if (!app.exiting && app.phase == App::Idle && !SettingsVisible()) ShowPanel();
        }
        InvalidateRect(app.panel, nullptr, FALSE);
    }
    void HidePanel() {
        AddTray(); if (!app.tray.registered) return;
        ShowWindow(app.panel, SW_HIDE);
        EndLaunch();
    }
    void DrawPanel(HWND hwnd, HDC target = nullptr) {
        if (app.exiting && GetTickCount64() - app.exitTime >= app.exitSeconds * 1000) {
            // Continuous frames can starve low-priority WM_TIMER at high refresh
            // rates. Post destruction after painting, never destroy inside a paint.
            ui::ContinuousMotion(hwnd, false);
            PostMessageW(hwnd, exitFinishedMessage, 0, 0);
        }
        const bool launch = !app.exiting && app.launching && ui::DecorativeMotion(3);
        ui::Paint paint(hwnd, target, !launch && !app.exiting); Graphics g(paint.buffer); ui::Prepare(g, hwnd);
        const float progress = ui::EntranceProgress(hwnd);
        auto scene = LaunchScene(progress, app.initialized); scene.secret = app.secret;
        ui::Round(g, { 24, 24, 40, 40 }, 12, ui::accent);
        ui::Text(g, L"S", { 35, 29, 30, 34 }, 24, ui::background, true);
        ui::Text(g, L"ScreenShotPlus", { 78, 27, 278, 36 }, 23, ui::foreground, true);
        ui::Button(g, hwnd, closeButton, L"×", false, ui::Inside(app.mouse, closeButton));
        ui::Text(g, tr("screen.app.011"), { 24, 92, 372, 28 }, 17);
        ui::Text(g, L"v1.7.2", { 328, 98, 68, 22 }, 12, ui::accent);
        ui::Text(g, app.notice, { 24, 128, 372, 40 }, 12, ui::muted);
        const array<wstring, 5> labels{ tr("screen.app.012"), tr("screen.app.013"),
            tr("screen.app.014"), tr("screen.app.015"), tr("screen.app.016") };
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto state = g.Save();
            if (launch) g.TranslateTransform(0, 20 * (1 - Ease((progress - 0.82f - static_cast<float>(i) * 0.018f) / 0.10f)));
            ui::Button(g, hwnd, buttons[i], labels[i], i == 0, ui::Inside(app.mouse, buttons[i]));
            g.Restore(state);
        }
        if (app.pending) ui::Button(g, hwnd, retryButton, tr("screen.app.017"), false, ui::Inside(app.mouse, retryButton));
        ui::Text(g, app.tray.registered ? tr("screen.app.018") : tr("screen.app.019"),
            { 24, 530, 372, 24 }, 12, ui::muted);
        if (launch) {
            DrawLaunchScene(g, scene, app.config.animationLevel);
            if (!scene.finished) SetPropW(hwnd, L"ScreenShotPlus.StartupPhase", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(scene.phase)));
            ui::ContinuousMotion(hwnd, !scene.finished);
        }
        if (app.launching && (scene.finished || !ui::DecorativeMotion(3))) EndLaunch();
        if (app.exiting)
            DrawExitScene(g, static_cast<float>((GetTickCount64() - app.exitTime) / (app.exitSeconds * 1000)), app.secret);
    }
    LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == Aelura::Capture::Message) {
            using namespace Aelura::Capture;
            if (wparam == SetLanguage) {
                if (lparam != 1 && lparam != 2) return 0;
                aelura::i18n::select(lparam == 1 ? 1 : 2);
                app.notice = tr("screen.app.001");
                EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM)->BOOL { InvalidateRect(window, nullptr, FALSE); return TRUE; }, 0);
                return Accepted;
            }
            if (wparam == Shutdown) { DestroyWindow(hwnd); return Accepted; }
            if (app.exiting) return Busy;
            if (wparam == CanCapture || wparam == Start) {
                if (app.phase != App::Idle || SettingsVisible()) return Busy;
                if (wparam == Start) StartCapture();
                return Accepted;
            }
            if (wparam == Settings) {
                if (app.phase != App::Idle) return Busy;
                ShowWindow(hwnd, SW_HIDE); ShowSettings(app.config); return Accepted;
            }
            if (wparam == Preview) {
                if (app.phase != App::Idle || SettingsVisible()) return Busy;
                LatestPreview(); return Accepted;
            }
            return 0;
        }
        if (message == exitFinishedMessage && app.exiting) { DestroyWindow(hwnd); return 0; }
        if (ui::MotionMessage(hwnd, message, wparam, lparam)) return 0;
        if (app.exiting) {
            if (message == WM_TIMER && wparam == exitTimer) {
                if (!ui::DecorativeMotion(4) || GetTickCount64() - app.exitTime >= app.exitSeconds * 1000) DestroyWindow(hwnd);
                else InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (message == WM_CLOSE || message == WM_LBUTTONDOWN || (message == WM_KEYDOWN &&
                (wparam == VK_ESCAPE || wparam == VK_RETURN || wparam == VK_SPACE))) {
                DestroyWindow(hwnd); return 0;
            }
            if (message == WM_HOTKEY || message == WM_SHOW_PANEL || message == WM_TRAY || message == WM_LBUTTONUP) return 0;
        }
        if (app.taskbarCreated && message == app.taskbarCreated) { app.tray.registered = false; AddTray(); return 0; }
        switch (message) {
        case WM_PAINT: DrawPanel(hwnd); return 0;
        case WM_PRINTCLIENT: DrawPanel(hwnd, reinterpret_cast<HDC>(wparam)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SHOW_PANEL: AddTray(); ShowPanel(); return 0;
        case WM_CLOSE: HidePanel(); return 0;
        case WM_TIMER: if (wparam == trayRetryTimer) AddTray(); return 0;
        case WM_TRAY:
            if (TrayEventOpensPanel(lparam)) ShowPanel();
            return 0;
        case WM_HOTKEY: return 0; // Global shortcuts belong exclusively to the toolbox.
        case WM_SETTINGS_SAVED:
            if (app.pending) SavePending();
            else Notice(tr("screen.app.020") + app.configFile.wstring());
            return 0;
        case WM_MOUSEMOVE: {
            app.mouse = ui::Mouse(hwnd, lparam);
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        case WM_MOUSELEAVE: app.mouse = { -1, -1 }; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN:
            if (app.launching) {
                if (ui::Inside(ui::Mouse(hwnd, lparam), launchBadge) && app.config.animationLevel >= 4) {
                    RevealSecret(); app.mouseDown = false; return 0;
                }
                EndLaunch(); app.mouseDown = false; ui::RestartMotion(hwnd, 0); return 0;
            }
            app.pressed = ui::Mouse(hwnd, lparam); app.mouseDown = true; SetCapture(hwnd); return 0;
        case WM_CAPTURECHANGED: app.mouseDown = false; return 0;
        case WM_NCHITTEST: {
            if (app.launching || app.exiting) return HTCLIENT;
            POINT p{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; ScreenToClient(hwnd, &p);
            const auto s = ui::Scale(hwnd);
            p = { static_cast<LONG>(p.x / s), static_cast<LONG>(p.y / s) };
            return p.y < 76 && !ui::Inside(p, closeButton) &&
                !(app.config.animationLevel >= 4 && ui::Inside(p, logoButton)) ? HTCAPTION : HTCLIENT;
        }
        case WM_LBUTTONUP: {
            const bool clicked = app.mouseDown; const POINT down = app.pressed;
            app.mouseDown = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (!clicked) return 0;
            const POINT p = ui::Mouse(hwnd, lparam);
            if (app.config.animationLevel >= 4 && ui::Inside(p, logoButton) && ui::Inside(down, logoButton)) {
                const auto now = GetTickCount64();
                if (now - app.logoTime > 1600) app.logoClicks = 0;
                app.logoTime = now;
                if (++app.logoClicks >= 5) { app.logoClicks = 0; RevealSecret(); }
                return 0;
            }
            if (ui::Inside(p, closeButton) && ui::Inside(down, closeButton)) { HidePanel(); return 0; }
            if (app.pending && ui::Inside(p, retryButton) && ui::Inside(down, retryButton)) { SavePending(); return 0; }
            for (size_t i = 0; i < buttons.size(); ++i) if (ui::Inside(p, buttons[i]) && ui::Inside(down, buttons[i])) {
                if (i == 0) StartCapture();
                else if (i == 1) LatestPreview();
                else if (i == 2) {
                    if (!ui::OpenFolder(app.config.folder)) Notice(tr("screen.app.021"));
                }
                else if (i == 3) {
                    ShowWindow(hwnd, SW_HIDE); ShowSettings(app.config);
                }
                else if (i == 4) BeginExit();
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (app.config.animationLevel >= 4 && wparam >= 'A' && wparam <= 'Z') {
                constexpr wstring_view signal = L"XUANRIKKA";
                const auto now = GetTickCount64();
                if (now - app.letterTime > 2000) app.secretLetters = 0;
                app.letterTime = now;
                app.secretLetters = wparam == signal[app.secretLetters] ? app.secretLetters + 1 : wparam == signal[0] ? 1 : 0;
                if (app.secretLetters == signal.size()) { app.secretLetters = 0; RevealSecret(); }
            }
            if (app.launching && (wparam == VK_ESCAPE || wparam == VK_RETURN || wparam == VK_SPACE)) {
                EndLaunch(); ui::RestartMotion(hwnd, 0);
            }
            else if (wparam == VK_ESCAPE) HidePanel();
            return 0;
        case WM_DPICHANGED: {
            ui::DpiChanged(hwnd, lparam, 420, 570);
            return 0;
        }
        case WM_DESTROY:
            CancelVisualEffects();
            app.exiting = true; CancelSelection();
            KillTimer(hwnd, exitTimer); RemovePropW(hwnd, L"ScreenShotPlus.ExitAnimating");
            KillTimer(hwnd, trayRetryTimer); RemoveTrayIcon(app.tray);
            ReleaseHotkeys(hwnd, app.hotkeys);
            DestroyEditor(); DestroySettings(); DestroyPreview(); app.latest.reset(); app.pending.reset();
            if (app.smallIcon) { DestroyIcon(app.smallIcon); app.smallIcon = nullptr; }
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
int RunApp(HINSTANCE instance, [[maybe_unused]] const filesystem::path& configFile, bool trayStartup, const wchar_t* startupKey) {
    app.instance = instance;
    jthread shareCleanup([] { CleanupTemporaryImages(ImageShareCache()); });
    app.startupKey = startupKey;
    app.configFile = ui::KnownFolder(FOLDERID_LocalAppData) / L"AeluraTool" / L"ScreenShotPlus" / L"config.json";
    app.allowPortableConfig = false;
    wstring warning; app.config = LoadConfig(app.configFile, warning);
    app.config.startup = false; app.config.printScreen = false;
    if (app.config.manualSaveOnly)app.notice = tr("screen.app.022");
    ConfigureUi();
    app.launching = !trayStartup && app.config.startupAnimation && ui::DecorativeMotion(3);
    if (!warning.empty()) app.notice = warning;
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW cls{ sizeof(cls) };
    cls.hInstance = instance; cls.lpfnWndProc = PanelProc; cls.lpszClassName = PANEL_CLASS;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SCREENSHOTPLUS));
    if (!RegisterClassExW(&cls)) return 4;
    app.panel = CreateWindowExW(WS_EX_TOOLWINDOW, PANEL_CLASS, L"ScreenShotPlus", WS_POPUP,
        0, 0, 420, 520, nullptr, nullptr, instance, nullptr);
    if (!app.panel) return 5;
    if (app.launching) SetPropW(app.panel, L"ScreenShotPlus.StartupAnimating", reinterpret_cast<HANDLE>(1));
    // Managed engine has a separate window class and does not claim the standalone instance marker.
    if (!InitPreview(instance, DragLatest, EditLatest, LocateLatest, DismissLatest) || !InitSettings(instance, app.panel, ApplyConfig) ||
        !InitEditor(instance, app.panel, SaveEdited, CancelEdit)) {
        DestroyWindow(app.panel); return 7;
    }
    SetSettingsConfigFile(app.configFile);
    ui::Corners(app.panel);
    app.smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SMALL), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()), GetSystemMetricsForDpi(SM_CYSMICON, GetDpiForSystem()), LR_DEFAULTCOLOR));
    app.tray.data.hWnd = app.panel; app.tray.data.uID = 1;
    app.tray.data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    app.tray.data.uCallbackMessage = WM_TRAY; app.tray.data.hIcon = app.smallIcon ? app.smallIcon : cls.hIcon;
    wcscpy_s(app.tray.data.szTip, tr("screen.app.023")); AddTray();
    const auto hotkeyWarning = RegisterConfiguredHotkeys(app.panel, app.hotkeys, app.config);
    if (!hotkeyWarning.empty()) app.notice += L" " + hotkeyWarning;
    app.initialized = true;
    if (!trayStartup) ShowPanel();
    SetPropW(app.panel, L"ScreenShotPlus.Ready", reinterpret_cast<HANDLE>(1));
    MSG message{}; BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(app.panel)) DestroyWindow(app.panel);
    return result == -1 ? 6 : static_cast<int>(message.wParam);
}
