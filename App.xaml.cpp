#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Integration/Diagnostics.h"
#include "Integration/Privileges.h"
#include <shlobj.h>
#include <winrt/Windows.ApplicationModel.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::AeluraTool::implementation
{
    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            aelura::diagnostics::failure("xaml.unhandled", e.Exception(), e.Message());
        });
        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
            {
                if (IsDebuggerPresent())
                {
                    auto errorMessage = e.Message();
                    __debugbreak();
                }
            });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    fire_and_forget App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        auto lifetime = get_strong();
        winrt::apartment_context ui;
        bool startupActivation = false;
        // The replacement waits for its predecessor. Keep the XAML dispatcher
        // pumping so shell activation can finish while that handoff is pending.
        co_await winrt::resume_background();
        if(aelura::ipv6::helperLaunch()) { co_await ui;Exit();co_return; }
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool prepared = aelura::privileges::prepareLaunch(startupActivation);
        if (SUCCEEDED(initialized)) CoUninitialize();
        co_await ui;
        if (!prepared) { Exit(); co_return; }
        try {
            auto activation = Windows::ApplicationModel::AppInstance::GetActivatedEventArgs();
            startupActivation = startupActivation || (activation && activation.Kind() == Windows::ApplicationModel::Activation::ActivationKind::StartupTask);
        } catch (hresult_error const&) { /* Keep a manual launch accessible if activation metadata is unavailable. */ }
        PWSTR local{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            auto config = std::filesystem::path(local) / L"AeluraTool" / L"hotkeys.ini"; CoTaskMemFree(local);
            if (aelura::privileges::requestAutomatic(nullptr, config, startupActivation)) { Exit(); co_return; }
        }
        try {
            auto main = make<MainWindow>();
            window = main;
            get_self<MainWindow>(main)->Setup(startupActivation);
            if (get_self<MainWindow>(main)->Ready()) get_self<MainWindow>(main)->ShowInitial();
        } catch (hresult_error const& error) {
            aelura::diagnostics::failure("app.startup", error.code(), error.message());
            Exit();
        } catch (std::exception const& error) {
            aelura::diagnostics::failure("app.startup.native", E_FAIL, to_hstring(error.what()));
            Exit();
        }
    }
}
