#pragma once
#include "pch.h"
#include "MainWindow.g.h"
#include <atomic>
#include <cstdint>
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
        void CopyPairing_Click(winrt::Windows::Foundation::IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void Body_SizeChanged(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void Lan_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Remote_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Transport_Changed(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void RemoteIp_Changed(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void Detect_Click(winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        std::atomic_bool m_stop{ false };
        std::atomic_bool m_running{ false };
        std::shared_ptr<std::atomic_bool> m_controlPermission = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> m_gamepadPermission = std::make_shared<std::atomic_bool>(false);
        std::thread m_worker;
        winrt::hstring m_pairingCode;
        bool m_initialized = false;
        bool m_compact = false;
        bool m_remoteMode = false;
        bool m_detecting = false;
        uint64_t m_lookupGeneration = 0;
        void ChangeMode(bool remote);
        void RefreshControls(bool running);
        void CancelDetection();
        winrt::fire_and_forget DetectAddress();
        void SetStatus(winrt::hstring const& message, bool running);
    };
}

namespace winrt::LANScreenCast::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
