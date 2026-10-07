#include "pch.h"
#include "ScreenSender.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "D3d11.lib")
#pragma comment(lib, "Dxgi.lib")

namespace LANScreenCast::casting
{
    namespace
    {
        constexpr int port = 47474;

        struct Runtime
        {
            ULONG_PTR gdiplusToken{};
            Runtime()
            {
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                    throw std::runtime_error("Winsock initialization failed");
                Gdiplus::GdiplusStartupInput input;
                if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok)
                {
                    WSACleanup();
                    throw std::runtime_error("JPEG encoder initialization failed");
                }
            }
            ~Runtime() { Gdiplus::GdiplusShutdown(gdiplusToken); WSACleanup(); }
        };

        CLSID JpegEncoder()
        {
            UINT count = 0, bytes = 0;
            Gdiplus::GetImageEncodersSize(&count, &bytes);
            std::vector<BYTE> storage(bytes);
            auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
            if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok)
                throw std::runtime_error("JPEG encoder unavailable");
            for (UINT i = 0; i < count; ++i)
                if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) return codecs[i].Clsid;
            throw std::runtime_error("JPEG encoder unavailable");
        }

        std::vector<BYTE> EncodeJpeg(Gdiplus::Bitmap& image, CLSID const& encoder)
        {
            IStream* stream = nullptr;
            if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
                throw std::runtime_error("Cannot create JPEG stream");
            ULONG quality = 70;
            Gdiplus::EncoderParameters parameters{};
            parameters.Count = 1;
            parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
            parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            parameters.Parameter[0].NumberOfValues = 1;
            parameters.Parameter[0].Value = &quality;
            if (image.Save(stream, &encoder, &parameters) != Gdiplus::Ok)
            {
                stream->Release();
                throw std::runtime_error("JPEG encoding failed");
            }
            HGLOBAL memoryHandle{};
            if (FAILED(GetHGlobalFromStream(stream, &memoryHandle)))
            {
                stream->Release();
                throw std::runtime_error("JPEG stream read failed");
            }
            STATSTG details{};
            if (FAILED(stream->Stat(&details, STATFLAG_NONAME)))
            {
                stream->Release();
                throw std::runtime_error("JPEG stream size unavailable");
            }
            SIZE_T size = static_cast<SIZE_T>(details.cbSize.QuadPart);
            auto bytes = static_cast<BYTE const*>(GlobalLock(memoryHandle));
            if (!bytes)
            {
                stream->Release();
                throw std::runtime_error("JPEG stream lock failed");
            }
            std::vector<BYTE> result(bytes, bytes + size);
            GlobalUnlock(memoryHandle);
            stream->Release();
            return result;
        }

        class DesktopCapture
        {
            using ComPtrDevice = Microsoft::WRL::ComPtr<ID3D11Device>;
            ComPtrDevice device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;

        public:
            DesktopCapture()
            {
                Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                    throw std::runtime_error("Desktop capture is unavailable");
                for (UINT adapterIndex = 0; ; ++adapterIndex)
                {
                    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                    if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                    for (UINT outputIndex = 0; ; ++outputIndex)
                    {
                        Microsoft::WRL::ComPtr<IDXGIOutput> output;
                        if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
                        DXGI_OUTPUT_DESC outputDescription{};
                        if (FAILED(output->GetDesc(&outputDescription))) continue;
                        auto area = outputDescription.DesktopCoordinates;
                        if (!outputDescription.AttachedToDesktop || area.left > 0 || area.top > 0 ||
                            area.right <= 0 || area.bottom <= 0) continue;
                        D3D_FEATURE_LEVEL level{};
                        device.Reset();
                        context.Reset();
                        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                            &device, &level, &context))) continue;
                        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
                        if (FAILED(output.As(&output1))) continue;
                        if (SUCCEEDED(output1->DuplicateOutput(device.Get(), &duplication))) return;
                    }
                }
                throw std::runtime_error("Cannot duplicate the primary display; check display permissions");
            }

            std::vector<BYTE> CaptureJpeg(CLSID const& encoder)
            {
                DXGI_OUTDUPL_FRAME_INFO frameInfo{};
                Microsoft::WRL::ComPtr<IDXGIResource> resource;
                HRESULT result = duplication->AcquireNextFrame(100, &frameInfo, &resource);
                if (result == DXGI_ERROR_WAIT_TIMEOUT) return {};
                if (FAILED(result)) throw std::runtime_error("Display capture stopped; restart casting");
                struct ReleaseFrame
                {
                    IDXGIOutputDuplication* value;
                    ~ReleaseFrame() { value->ReleaseFrame(); }
                } release{ duplication.Get() };

                Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
                if (FAILED(resource.As(&frame))) throw std::runtime_error("Cannot read desktop frame");
                D3D11_TEXTURE2D_DESC description{};
                frame->GetDesc(&description);
                if (!staging)
                {
                    description.Usage = D3D11_USAGE_STAGING;
                    description.BindFlags = 0;
                    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    description.MiscFlags = 0;
                    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)))
                        throw std::runtime_error("Cannot allocate desktop readback");
                }
                context->CopyResource(staging.Get(), frame.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                    throw std::runtime_error("Cannot map desktop frame");
                struct UnmapFrame
                {
                    ID3D11DeviceContext* context;
                    ID3D11Texture2D* texture;
                    ~UnmapFrame() { context->Unmap(texture, 0); }
                } unmap{ context.Get(), staging.Get() };

                int sourceWidth = static_cast<int>(description.Width);
                int sourceHeight = static_cast<int>(description.Height);
                int width = std::min(sourceWidth, 1280);
                int height = std::max(1, sourceHeight * width / sourceWidth);
                Gdiplus::Bitmap source(sourceWidth, sourceHeight,
                    static_cast<INT>(mapped.RowPitch), PixelFormat32bppRGB,
                    static_cast<BYTE*>(mapped.pData));
                Gdiplus::Bitmap scaled(width, height, PixelFormat32bppRGB);
                {
                    Gdiplus::Graphics graphics(&scaled);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                    if (graphics.DrawImage(&source, 0, 0, width, height) != Gdiplus::Ok)
                        throw std::runtime_error("Desktop frame scaling failed");
                }
                return EncodeJpeg(scaled, encoder);
            }
        };

        void SendAll(SOCKET socket, char const* bytes, size_t length)
        {
            while (length)
            {
                int chunk = static_cast<int>(std::min<size_t>(length, 64 * 1024));
                int sent = send(socket, bytes, chunk, 0);
                if (sent <= 0) throw std::runtime_error("Connection lost while sending video");
                bytes += sent;
                length -= sent;
            }
        }

        SOCKET Connect(std::wstring const& ip)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            if (InetPtonW(AF_INET, ip.c_str(), &address.sin_addr) != 1)
                throw std::runtime_error("Enter a valid IPv4 address shown on the receiver");
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) throw std::runtime_error("Cannot create network socket");
            u_long nonblocking = 1;
            ioctlsocket(socket, FIONBIO, &nonblocking);
            int result = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
            {
                closesocket(socket);
                throw std::runtime_error("Cannot connect to receiver");
            }
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            timeval timeout{ 3, 0 };
            result = select(0, nullptr, &writable, nullptr, &timeout);
            int socketError = 0;
            int optionLength = sizeof(socketError);
            getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &optionLength);
            if (result <= 0 || socketError != 0)
            {
                closesocket(socket);
                throw std::runtime_error("Receiver unreachable; check IP and Wi-Fi");
            }
            nonblocking = 0;
            ioctlsocket(socket, FIONBIO, &nonblocking);
            DWORD sendTimeout = 2000;
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<char*>(&sendTimeout), sizeof(sendTimeout));
            int sendBuffer = 256 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                reinterpret_cast<char*>(&sendBuffer), sizeof(sendBuffer));
            return socket;
        }
    }

    void StreamDesktop(std::wstring const& receiverIp, int targetFps, std::atomic_bool const& stop,
        std::function<void(std::wstring const&)> const& onStatus)
    {
        targetFps = std::clamp(targetFps, 10, 30);
        Runtime runtime;
        CLSID encoder = JpegEncoder();
        DesktopCapture capture;
        SOCKET socket = Connect(receiverIp);
        try
        {
            SendAll(socket, "LSC1", 4);
            onStatus(L"正在投屏");
            std::vector<BYTE> latestImage;
            auto lastSent = std::chrono::steady_clock::now();
            auto statsStart = lastSent;
            int sentFrames = 0;
            auto framePeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / targetFps));
            while (!stop)
            {
                auto start = std::chrono::steady_clock::now();
                auto image = capture.CaptureJpeg(encoder);
                bool changed = !image.empty();
                if (changed) latestImage = std::move(image);
                auto now = std::chrono::steady_clock::now();
                if (!latestImage.empty() && (changed || now - lastSent >= std::chrono::seconds(1)))
                {
                    if (latestImage.size() > 8 * 1024 * 1024)
                        throw std::runtime_error("Captured frame is too large");
                    uint32_t length = htonl(static_cast<uint32_t>(latestImage.size()));
                    SendAll(socket, reinterpret_cast<char const*>(&length), sizeof(length));
                    SendAll(socket, reinterpret_cast<char const*>(latestImage.data()), latestImage.size());
                    lastSent = std::chrono::steady_clock::now();
                    ++sentFrames;
                }
                now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - statsStart).count();
                if (elapsed >= 3000)
                {
                    int actualFps = static_cast<int>(sentFrames * 1000LL / elapsed);
                    onStatus(std::wstring(L"正在投屏 · 目标 ") + std::to_wstring(targetFps) +
                        L" FPS · 实际画面更新 " + std::to_wstring(actualFps) + L" FPS");
                    statsStart = now;
                    sentFrames = 0;
                }
                std::this_thread::sleep_until(start + framePeriod);
            }
        }
        catch (...)
        {
            closesocket(socket);
            throw;
        }
        closesocket(socket);
    }
}
