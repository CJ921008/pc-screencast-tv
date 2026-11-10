#pragma once
#include "pch.h"
#include "MainWindow.g.h"
#include <atomic>
#include <memory>
#include <thread>

namespace winrt::LANScreenCast::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();
        void Start_Click(winrt::Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void Stop_Click(winrt::Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void RevokeInput_Click(winrt::Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        std::atomic_bool m_stop{ false };
        std::atomic_bool m_running{ false };
        std::shared_ptr<std::atomic_bool> m_controlPermission = std::make_shared<std::atomic_bool>(false);
        std::thread m_worker;
        void SetStatus(winrt::hstring const& message, bool running);
    };
}

namespace winrt::LANScreenCast::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
