#pragma once
#include "MainWindow.g.h"
#include <filesystem>
#include <atomic>
#include "Integration/HotkeyManager.h"
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
namespace winrt::AeluraTool::implementation {
    struct MainWindow : MainWindowT<MainWindow> {
        MainWindow() = default;
        ~MainWindow();
        void Setup();
        bool Ready() const { return ready_; }
    private:
        HWND hwnd_{};
        HANDLE process_{}, job_{}, singleton_{};
        DWORD pid_{};
        aelura::hotkeys::Manager hotkeys_;
        HANDLE strike_{};
        int category_{}, page_{};
        bool moveBusy_{};
        bool ready_{}, localizing_{}, startupPlayed_{};
        int languagePreference_{};
        Microsoft::UI::Xaml::Media::Animation::Storyboard startupBoard_{nullptr};
        std::atomic<bool> cancelMove_{false};
        std::atomic<uintmax_t> moveDone_{0}, moveTotal_{0};
        Microsoft::UI::Dispatching::DispatcherQueueTimer moveTimer_{nullptr};
        std::filesystem::path config_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };
        UINT pending_{}; unsigned attempts_{};
        void Page(int index);
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
        void Invoke(UINT command);
        HWND EngineWindow();
        static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    };
}
namespace winrt::AeluraTool::factory_implementation {
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
