#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "logging/FileLogger.h"

namespace winrt::LANScreenCast::implementation
{
    App::App()
    {
        InitializeComponent();
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"APP", L"STARTED", L"Sender waiting for connection");
        try
        {
            window = winrt::make<MainWindow>();
            window.Activate();
            ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"APP", L"WINDOW_READY", L"Main window activated");
        }
        catch (winrt::hresult_error const& error)
        {
            ::LANScreenCast::logging::FileLogger::Write(L"ERROR", L"APP", L"STARTUP_FAILED", error.message());
            throw;
        }
    }
}
