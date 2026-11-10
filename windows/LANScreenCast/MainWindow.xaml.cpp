#include "pch.h"
#include "MainWindow.xaml.h"
#include "casting/ScreenSender.h"
#include "casting/WebRtcSender.h"
#include "casting/RemoteAccess.h"
#include "logging/FileLogger.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::LANScreenCast::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"LAN ScreenCast");
        Closed([this](auto const&, auto const&) { *m_controlPermission = false; m_stop = true; });
    }

    MainWindow::~MainWindow()
    {
        *m_controlPermission = false;
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
        AllowRemoteInput().IsEnabled(!running);
        ResetPairing().IsEnabled(!running);
        CopyPairingButton().IsEnabled(running && !m_pairingCode.empty());
        RevokeInputButton().IsEnabled(running && *m_controlPermission);
    }

    void MainWindow::Start_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_running) return;
        auto ip = std::wstring(ReceiverIp().Text().c_str());
        if (ip.empty())
        {
            SetStatus(TransportMode().SelectedIndex() == 2 ? L"请输入这台电脑的公网 IPv6 地址" :
                L"请输入接收设备画面上的 IP 地址", false);
            return;
        }
        if (m_worker.joinable()) m_worker.join();
        constexpr int frameRates[] = { 10, 15, 20, 30 };
        int selected = FrameRate().SelectedIndex();
        int targetFps = frameRates[selected >= 0 && selected < 4 ? selected : 2];
        bool remoteMode = TransportMode().SelectedIndex() == 2;
        bool rtcMode = TransportMode().SelectedIndex() != 1;
        auto reset = ResetPairing().IsChecked();
        bool resetPairing = remoteMode && reset && reset.Value();
        std::filesystem::path identityPath;
        if (remoteMode)
            identityPath = std::filesystem::path(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) / L"remote-identity.bin";
        auto checked = AllowRemoteInput().IsChecked();
        *m_controlPermission = rtcMode && checked && checked.Value();
        ::LANScreenCast::casting::VideoSettings settings;
        settings.fps = targetFps;
        if (Resolution().SelectedIndex() == 1) { settings.width = 1920; settings.height = 1080; settings.bitrate = 8000000; }
        m_stop = false;
        m_running = true;
        SetStatus(remoteMode ? L"正在启动 IPv6 远程接入…" : L"正在连接接收设备…", true);
        if (!remoteMode) ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"CAST", L"CONNECTING", ip);
        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        m_worker = std::thread([this, ip, targetFps, rtcMode, remoteMode, resetPairing, identityPath, settings, dispatcher, weak]()
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
                if (remoteMode)
                {
                    auto pairing = [dispatcher, weak](std::wstring const& code) {
                        dispatcher.TryEnqueue([weak, code = winrt::hstring(code)] {
                            if (auto self = weak.get(); self && !self->m_stop) {
                                self->m_pairingCode = code;
                                self->CopyPairingButton().IsEnabled(true);
                                self->ResetPairing().IsChecked(winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>());
                            }
                        });
                    };
                    ::LANScreenCast::casting::ServeRemoteWebRtc(ip, identityPath, resetPairing, settings, m_stop, m_controlPermission, update, pairing);
                    finalStatus = L"远程接入已关闭";
                }
                else if (rtcMode) ::LANScreenCast::casting::StreamWebRtc(ip, settings, m_stop, m_controlPermission, update);
                else ::LANScreenCast::casting::StreamDesktop(ip, targetFps, m_stop, update);
                ::LANScreenCast::logging::FileLogger::Write(L"INFO", L"CAST", L"STOPPED", L"Sender stopped");
            }
            catch (std::exception const& error)
            {
                finalStatus = std::wstring(L"投屏失败：") + winrt::to_hstring(error.what()).c_str();
                ::LANScreenCast::logging::FileLogger::Write(L"ERROR", L"CAST", L"FAILED", finalStatus);
            }
            *m_controlPermission = false;
            m_running = false;
            dispatcher.TryEnqueue([weak, text = winrt::hstring(finalStatus)]()
            {
                if (auto self = weak.get()) {
                    self->m_pairingCode = winrt::hstring{};
                    self->AllowRemoteInput().IsChecked(winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>());
                    self->SetStatus(text, false);
                }
            });
        });
    }

    void MainWindow::Stop_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        *m_controlPermission = false;
        m_stop = true;
        SetStatus(L"正在停止…", true);
    }

    void MainWindow::RevokeInput_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        *m_controlPermission = false;
        AllowRemoteInput().IsChecked(winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>());
        RevokeInputButton().IsEnabled(false);
    }

    void MainWindow::CopyPairing_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_pairingCode.empty() || m_stop) return;
        try {
            using namespace winrt::Windows::ApplicationModel::DataTransfer;
            DataPackage data;
            data.SetText(m_pairingCode);
            ClipboardContentOptions options;
            options.IsAllowedInHistory(false);
            options.IsRoamable(false);
            if (Clipboard::SetContentWithOptions(data, options)) StatusText().Text(L"配对码已复制");
            else StatusText().Text(L"无法复制配对码，请重试");
        } catch (winrt::hresult_error const&) {
            StatusText().Text(L"无法复制配对码，请重试");
        }
    }
}
