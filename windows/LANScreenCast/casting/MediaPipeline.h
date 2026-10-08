#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace LANScreenCast::casting {
    struct VideoSettings { int width = 1280; int height = 720; int fps = 30; int bitrate = 4000000; };
    struct VideoFrame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        int64_t time100ns = 0;
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    };
    struct AccessUnit { std::vector<uint8_t> bytes; int64_t time100ns; bool keyframe; };
    class DesktopSource {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        DesktopSource();
        ~DesktopSource();
        ID3D11Device* Device() const;
        VideoFrame Next(int fps);
    };
    class H264Encoder {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        H264Encoder(ID3D11Device* device, VideoSettings settings, bool forceSoftware = false);
        ~H264Encoder();
        VideoSettings Settings() const;
        bool Hardware() const;
        std::wstring Name() const;
        void ForceKeyFrame();
        void Encode(VideoFrame const& frame, bool keyframe, std::atomic_bool const& stop,
            std::function<void(AccessUnit)> const& emit);
        void EncodeNV12(std::vector<uint8_t> const& bytes, int64_t time,
            std::atomic_bool const& stop, std::function<void(AccessUnit)> const& emit);
        void Drain(std::function<void(AccessUnit)> const& emit);
    };
    std::vector<uint8_t> H264AnnexB(std::vector<uint8_t> const& input);
}
