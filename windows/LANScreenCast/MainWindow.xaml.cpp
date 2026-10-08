#include "pch.h"
#include "MainWindow.xaml.h"
#include "casting/ScreenSender.h"
#include "casting/WebRtcSender.h"
#include "logging/FileLogger.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::LANScreenCast::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"LAN ScreenCast");
        Closed([this](auto const&, auto const&) { m_stop = true; });
    }

    MainWindow::~MainWindow()
    {
        m_stop = true;
        if (m_worker.joinable()) m_worker.join();
    }

    void MainWindow::SetStatus(winrt::hstring const& message, bool running)
    {
        StatusText().Text(message);
        StartButton().IsEnabled(!running);
        StopButton().IsEnabled(running);
        ReceiverIp().IsEnabled(!running);
        TransportMode().IsEnabled(!running);
        Resolution().IsEnabled(!running);
        FrameRate().IsEnabled(!running);
    }

    void MainWindow::Start_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_running) return;
        auto ip = std::wstring(ReceiverIp().Text().c_str());
        if (ip.empty())
        {
            SetStatus(L"请输入接收设备画面上的 IP 地址", false);
            return;
        }
        if (m_worker.joinable()) m_worker.join();
        constexpr int frameRates[] = { 10, 15, 20, 30 };
        int selected = FrameRate().SelectedIndex();
        int targetFps = frameRates[selected >= 0 && selected < 4 ? selected : 2];
        bool rtcMode = TransportMode().SelectedIndex() == 0;
        ::LANScreenCast::casting::VideoSettings settings;
        settings.fps = targetFps;
        if (Resolution().SelectedIndex() == 1) { settings.width = 1920; settings.height = 1080; settings.bitrate = 8000000; }
        m_stop = false;
        m_running = true;
        SetStatus(L"正在连接接收设备…", true);
        ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"CAST", L"CONNECTING", ip);
        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        m_worker = std::thread([this, ip, targetFps, rtcMode, settings, dispatcher, weak]()
        {
            std::wstring finalStatus = L"投屏已停止";
            try
            {
                auto update = [dispatcher, weak](std::wstring const& message)
                    {
                        dispatcher.TryEnqueue([weak, text = winrt::hstring(message)]()
                        {
                            if (auto self = weak.get()) self->StatusText().Text(text);
                        });
                    };
                if (rtcMode) ::LANScreenCast::casting::StreamWebRtc(ip, settings, m_stop, update);
                else ::LANScreenCast::casting::StreamDesktop(ip, targetFps, m_stop, update);
                ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"CAST", L"STOPPED", L"Sender stopped");
            }
            catch (std::exception const& error)
            {
                finalStatus = std::wstring(L"投屏失败：") + winrt::to_hstring(error.what()).c_str();
                ::LANScreenCast::logging::FileLogger::Write(L"ERROR", L"CAST", L"FAILED", finalStatus);
            }
            m_running = false;
            dispatcher.TryEnqueue([weak, text = winrt::hstring(finalStatus)]()
            {
                if (auto self = weak.get()) self->SetStatus(text, false);
            });
        });
    }

    void MainWindow::Stop_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_stop = true;
        SetStatus(L"正在停止…", true);
    }
}
