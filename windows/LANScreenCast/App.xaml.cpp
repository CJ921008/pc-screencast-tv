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
        window = winrt::make<MainWindow>();
        window.Activate();
    }
}
