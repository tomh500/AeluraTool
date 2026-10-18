#pragma once
#include "MainWindow.g.h"
#include <filesystem>
namespace winrt::AeluraTool::implementation {
    struct MainWindow : MainWindowT<MainWindow> {
        MainWindow() = default;
        ~MainWindow();
        void Setup();
    private:
        HWND hwnd_{};
        HANDLE process_{}, job_{}, singleton_{};
        DWORD pid_{};
        int slot_ = 100;
        bool registered_ = false, print_ = false, enabled_ = true;
        UINT modifiers_ = MOD_CONTROL | MOD_ALT, key_ = 'A';
        std::filesystem::path config_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };
        UINT pending_{}; unsigned attempts_{};
        void Page(int index);
        void Notice(hstring const& message, bool error = false);
        bool Bind(UINT modifiers, UINT key, bool enabled, bool print, bool persist);
        void Invoke(UINT command);
        HWND EngineWindow();
        static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    };
}
namespace winrt::AeluraTool::factory_implementation {
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
