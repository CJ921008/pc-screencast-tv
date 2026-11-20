#include "pch.h"
#include "MainWindow.xaml.h"
#include "casting/ScreenSender.h"
#include "casting/WebRtcSender.h"
#include "casting/RemoteAccess.h"
#include "casting/LocalIPv6.h"
#include "logging/FileLogger.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::LANScreenCast::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;

    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"VanJScreen");
        auto package = Windows::ApplicationModel::Package::Current();
        auto version = package.Id().Version();
        AppVersion().Text(L"VanJScreen  /  " + std::to_wstring(version.Major) + L"." +
            std::to_wstring(version.Minor) + L"." + std::to_wstring(version.Build));
        auto icon = std::filesystem::path(package.InstalledLocation().Path().c_str()) / L"Assets" / L"VanJScreen.ico";
        try {
            AppWindow().SetIcon(icon.c_str());
        } catch (winrt::hresult_error const& error) {
            ::LANScreenCast::logging::FileLogger::Write(L"WARN", L"APP", L"WINDOW_ICON", error.message().c_str());
        }
        m_initialized = true;
        ChangeMode(false);
        Closed([this](auto const&, auto const&) {
            ++m_lookupGeneration;
            *m_controlPermission = false;
            m_stop = true;
        });
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
        RefreshControls(running);
    }

    void MainWindow::RefreshControls(bool running)
    {
        bool jpeg = !m_remoteMode && TransportMode().SelectedIndex() == 1;
        auto visible = [](bool show) { return show ? Visibility::Visible : Visibility::Collapsed; };
        LanSettings().Visibility(visible(!m_remoteMode));
        RemoteSettings().Visibility(visible(m_remoteMode));
        PairingPanel().Visibility(visible(m_remoteMode));
        LanTab().IsEnabled(!running);
        RemoteTab().IsEnabled(!running);
        StartButton().Content(winrt::box_value(winrt::hstring(m_remoteMode ? L"开启远程接入" : L"开始投屏")));
        StopButton().Content(winrt::box_value(winrt::hstring(m_remoteMode ? L"关闭远程接入" : L"停止投屏")));
        StartButton().Visibility(visible(!running));
        StopButton().Visibility(visible(running));
        StartButton().IsEnabled(!running);
        StopButton().IsEnabled(running && !m_stop);
        ReceiverIp().IsEnabled(!running);
        RemoteIp().IsEnabled(!running);
        DetectButton().IsEnabled(!running && !m_detecting);
        TransportMode().IsEnabled(!running);
        Resolution().IsEnabled(!running && !jpeg);
        FrameRate().IsEnabled(!running);
        AllowRemoteInput().Visibility(visible(!jpeg));
        AllowRemoteInput().IsEnabled(!running);
        ResetPairing().IsEnabled(!running);
        CopyPairingButton().IsEnabled(running && !m_stop && !m_pairingCode.empty());
        RevokeInputButton().Visibility(visible(running && *m_controlPermission));
        if (m_remoteMode) {
            StatusHint().Text(running ? L"在手机上打开接收器，连接这台电脑。" :
                L"填写本机公网 IPv6 后开启接入。");
        } else {
            StatusHint().Text(jpeg ? L"JPEG 兼容模式 · 720p · 仅画面" :
                L"H.264 模式 · 桌面画面与系统声音");
        }
    }

    void MainWindow::Body_SizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const& args)
    {
        if (!m_initialized) return;
        bool compact = args.NewSize().Width < 740;
        if (compact == m_compact) return;
        m_compact = compact;
        Grid::SetColumnSpan(SettingsPanel(), compact ? 2 : 1);
        Grid::SetColumn(StatusPanel(), compact ? 0 : 1);
        Grid::SetRow(StatusPanel(), compact ? 1 : 0);
        Grid::SetColumnSpan(StatusPanel(), compact ? 2 : 1);
    }

    void MainWindow::ChangeMode(bool remote)
    {
        if (m_running) return;
        bool changed = m_remoteMode != remote;
        m_remoteMode = remote;
        LanTab().IsChecked(winrt::box_value(!remote).as<Windows::Foundation::IReference<bool>>());
        RemoteTab().IsChecked(winrt::box_value(remote).as<Windows::Foundation::IReference<bool>>());
        if (changed) {
            CancelDetection();
            Ipv6Tip().IsOpen(false);
            StatusText().Text(L"等待连接");
        }
        RefreshControls(false);
    }

    void MainWindow::Lan_Click(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ChangeMode(false);
    }

    void MainWindow::Remote_Click(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ChangeMode(true);
    }

    void MainWindow::Transport_Changed(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_initialized) RefreshControls(m_running);
    }

    void MainWindow::CancelDetection()
    {
        if (!m_detecting) return;
        ++m_lookupGeneration;
        m_detecting = false;
        DetectButton().Content(winrt::box_value(L"获取 IPv6"));
        DetectButton().IsEnabled(!m_running);
        Ipv6Tip().IsOpen(false);
    }

    void MainWindow::RemoteIp_Changed(Windows::Foundation::IInspectable const&, TextChangedEventArgs const&)
    {
        if (m_initialized) {
            CancelDetection();
            Ipv6Tip().IsOpen(false);
        }
    }

    void MainWindow::Detect_Click(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_remoteMode && !m_running && !m_detecting) DetectAddress();
    }

    winrt::fire_and_forget MainWindow::DetectAddress()
    {
        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        auto generation = ++m_lookupGeneration;
        auto original = RemoteIp().Text();
        m_detecting = true;
        DetectButton().IsEnabled(false);
        DetectButton().Content(winrt::box_value(L"正在检测…"));
        Ipv6Tip().IsOpen(false);
        co_await winrt::resume_background();
        std::optional<std::wstring> address;
        try {
            address = ::LANScreenCast::casting::DetectLocalIPv6();
        } catch (std::exception const&) {
            // Leave the edit field intact if Windows cannot read the adapters.
        }
        dispatcher.TryEnqueue([weak, generation, original, address = std::move(address)] {
            auto self = weak.get();
            if (!self || self->m_lookupGeneration != generation) return;
            self->CancelDetection();
            if (!self->m_remoteMode || self->m_running || self->RemoteIp().Text() != original) return;
            if (address) self->RemoteIp().Text(*address);
            self->Ipv6Tip().Severity(address ? InfoBarSeverity::Success : InfoBarSeverity::Warning);
            self->Ipv6Tip().Message(address ? L"已填入本机公网 IPv6。" :
                L"未找到公网 IPv6，请检查网络或手动输入。");
            self->Ipv6Tip().IsOpen(true);
        });
    }

    void MainWindow::Start_Click(winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_running) return;
        bool remoteMode = m_remoteMode;
        auto addressField = remoteMode ? RemoteIp() : ReceiverIp();
        auto ip = std::wstring(addressField.Text());
        if (ip.empty())
        {
            SetStatus(remoteMode ? L"请输入这台电脑的公网 IPv6 地址" :
                L"请输入接收设备画面上的 IP 地址", false);
            addressField.Focus(FocusState::Programmatic);
            return;
        }
        CancelDetection();
        if (m_worker.joinable()) m_worker.join();
        constexpr int frameRates[] = { 10, 15, 20, 30 };
        int selected = FrameRate().SelectedIndex();
        int targetFps = frameRates[selected >= 0 && selected < 4 ? selected : 2];
        bool rtcMode = remoteMode || TransportMode().SelectedIndex() != 1;
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
                            if (auto self = weak.get(); self && !self->m_stop) self->StatusText().Text(text);
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
        RevokeInputButton().Visibility(Visibility::Collapsed);
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
