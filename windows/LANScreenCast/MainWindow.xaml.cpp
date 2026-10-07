#include "pch.h"
#include "MainWindow.xaml.h"
#include "casting/ScreenSender.h"
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
        m_stop = false;
        m_running = true;
        SetStatus(L"正在连接接收设备…", true);
        ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"CAST", L"CONNECTING", ip);
        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        m_worker = std::thread([this, ip, targetFps, dispatcher, weak]()
        {
            std::wstring finalStatus = L"投屏已停止";
            try
            {
                ::LANScreenCast::casting::StreamDesktop(ip, targetFps, m_stop,
                    [dispatcher, weak](std::wstring const& message)
                    {
                        dispatcher.TryEnqueue([weak, text = winrt::hstring(message)]()
                        {
                            if (auto self = weak.get()) self->StatusText().Text(text);
                        });
                    });
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
