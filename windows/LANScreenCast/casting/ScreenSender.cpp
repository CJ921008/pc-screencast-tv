#include "pch.h"
#include "ScreenSender.h"
#include <gdiplus.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Gdiplus.lib")

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

        std::vector<BYTE> CaptureJpeg(CLSID const& encoder)
        {
            HDC screen = GetDC(nullptr);
            if (!screen) throw std::runtime_error("Cannot capture desktop");
            HDC memory = CreateCompatibleDC(screen);
            int sourceWidth = GetSystemMetrics(SM_CXSCREEN);
            int sourceHeight = GetSystemMetrics(SM_CYSCREEN);
            int width = std::min(sourceWidth, 1280);
            int height = std::max(1, sourceHeight * width / sourceWidth);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
            if (!memory || !bitmap)
            {
                if (bitmap) DeleteObject(bitmap);
                if (memory) DeleteDC(memory);
                ReleaseDC(nullptr, screen);
                throw std::runtime_error("Cannot allocate capture bitmap");
            }
            HGDIOBJ old = SelectObject(memory, bitmap);
            SetStretchBltMode(memory, HALFTONE);
            BOOL copied = StretchBlt(memory, 0, 0, width, height, screen, 0, 0,
                sourceWidth, sourceHeight, SRCCOPY | CAPTUREBLT);
            SelectObject(memory, old);
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            if (!copied)
            {
                DeleteObject(bitmap);
                throw std::runtime_error("Desktop capture failed");
            }

            Gdiplus::Bitmap image(bitmap, nullptr);
            DeleteObject(bitmap);
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
            return socket;
        }
    }

    void StreamDesktop(std::wstring const& receiverIp, std::atomic_bool const& stop,
        std::function<void(std::wstring const&)> const& onStatus)
    {
        Runtime runtime;
        CLSID encoder = JpegEncoder();
        SOCKET socket = Connect(receiverIp);
        try
        {
            SendAll(socket, "LSC1", 4);
            onStatus(L"正在投屏");
            while (!stop)
            {
                auto start = std::chrono::steady_clock::now();
                auto image = CaptureJpeg(encoder);
                if (image.empty() || image.size() > 8 * 1024 * 1024)
                    throw std::runtime_error("Captured frame is too large");
                uint32_t length = htonl(static_cast<uint32_t>(image.size()));
                SendAll(socket, reinterpret_cast<char const*>(&length), sizeof(length));
                SendAll(socket, reinterpret_cast<char const*>(image.data()), image.size());
                std::this_thread::sleep_until(start + std::chrono::milliseconds(100));
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
