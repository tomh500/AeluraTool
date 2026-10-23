#pragma once
#include "MainWindow.g.h"
#include <filesystem>
#include <atomic>
#include <optional>
#include <vector>
#include "Integration/DesktopLifecycle.h"
#include "Integration/HotkeyManager.h"
#include "Integration/AutoClicker.h"
#include "Integration/ToggleNotification.h"
#include "Integration/FileHash.h"
#include "Integration/RecordingPanel.h"
#include "Integration/AccountPanel.h"
#include "Integration/FileSearchWindow.h"
#include <memory>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
namespace winrt::AeluraTool::implementation {
    struct MainWindow : MainWindowT<MainWindow> {
        MainWindow() = default;
        ~MainWindow();
        void Setup(bool startupActivation = false);
        void ShowInitial();
        bool Ready() const { return ready_; }
    private:
        HWND hwnd_{};
        HANDLE process_{}, job_{}, singleton_{};
        DWORD pid_{};
        aelura::hotkeys::Manager hotkeys_;
        aelura::autoclicker::Engine autoClicker_;
        void SetupAutoClicker();
        void LocalizeAutoClicker();
        void RefreshAutoClicker();
        bool SaveAutoClicker();
        void ToggleAutoClicker(bool hotkey = false);
        bool autoClickerUpdating_{}, hotkeyNotifications_{true};
        aelura::autoclicker::Options autoClickerOptions_;
        std::unique_ptr<aelura::notifications::ToggleNotification> toggleNotification_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer autoClickerTimer_{nullptr};
        std::unique_ptr<aelura::filesearch::SearchWindow> fileSearch_;
        std::unique_ptr<aelura::filesearch::SearchWindow> fileSearchPage_;
        std::shared_ptr<aelura::filesearch::Index> fileSearchIndex_;
        void ShowFileSearch();
        void EnsureFileSearchIndex();
        HANDLE strike_{};
        int category_{1}, page_{};
        struct SearchTarget { std::wstring label, id; int page, category; };
        std::vector<SearchTarget> searchTargets_;
        struct NavigationEntry { int page, category; winrt::hstring query; };
        struct NavigationSnapshot { NavigationEntry location; std::optional<NavigationEntry> toolReturn; };
        std::vector<NavigationSnapshot> navigationHistory_;
        std::optional<NavigationEntry> toolReturn_;
        bool restoringNavigation_{};
        aelura::desktop::TrayIcon tray_;
        int closeAction_{};
        bool closeDialogOpen_{}, startupBusy_{}, startupActivation_{}, recordingCloseBusy_{};
        bool elevationBusy_{};
        bool moveBusy_{};
        enum class FileTask { None, Migration, Hash };
        FileTask fileTask_{FileTask::None};
        bool fileTaskNotice_{};
        aelura::hash::Session hashSession_;
        std::unique_ptr<aelura::recording::Panel> recording_;
        std::unique_ptr<aelura::account::Panel> account_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer hashTimer_{nullptr};
        bool ready_{}, localizing_{}, startupPlayed_{};
        int languagePreference_{};
        Microsoft::UI::Xaml::Media::Animation::Storyboard startupBoard_{nullptr};
        std::atomic<bool> cancelMove_{false};
        std::atomic<uintmax_t> moveDone_{0}, moveTotal_{0};
        Microsoft::UI::Dispatching::DispatcherQueueTimer moveTimer_{nullptr};
        std::filesystem::path config_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };
        UINT pending_{}; unsigned attempts_{};
        void Page(int index, int category = -1, bool remember = true);
        void GoBack();
        void GoHistoryBack();
        void SetupSearch();
        void UpdateSearch();
        void OpenSearchResult(winrt::hstring const& label);
        void SetupLifecycle();
        void LocalizeLifecycle();
        winrt::fire_and_forget Elevate();
        void RestoreWindow();
        bool HideToTray();
        bool SaveBehavior(wchar_t const* key, int value);
        winrt::fire_and_forget RequestClose(bool explicitExit = false);
        winrt::fire_and_forget StartupSetting(std::optional<bool> enable = std::nullopt);
        void Notice(hstring const& message, bool error = false);
        void RenderTools();
        void ApplyLanguage();
        void PlayStartup(bool preview = false);
        void FinishStartup();
        void RenderKeys();
        void ActivateTool(std::wstring const& id);
        void LaunchStrike();
        winrt::fire_and_forget EditKey(size_t index);
        winrt::fire_and_forget MoveFiles(bool inspectOnly);
        void PickPath(bool source, bool folder);
        bool AcquireFileTask(FileTask task);
        void ReleaseFileTask();
        void SetupHash();
        void LocalizeHash();
        void RenderHash();
        void InvalidateHash();
        void CancelHash();
        void PickHash(bool second);
        void CopyHash(bool second);
        winrt::fire_and_forget RunHash();
        void Invoke(UINT command);
        HWND EngineWindow();
        static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    };
}
namespace winrt::AeluraTool::factory_implementation {
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
