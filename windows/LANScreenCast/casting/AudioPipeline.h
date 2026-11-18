#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace LANScreenCast::casting {
class OpusAudioEncoder {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;
    static constexpr int FrameSamples = 960;

    OpusAudioEncoder();
    ~OpusAudioEncoder();
    std::vector<uint8_t> Encode(std::span<float const> samples);
};

class SystemAudioSender {
    std::chrono::steady_clock::time_point origin;
public:
    explicit SystemAudioSender(std::chrono::steady_clock::time_point clockOrigin);
    void Run(std::atomic_bool const& stop,
        std::function<void(std::vector<uint8_t>, int64_t)> const& emit,
        std::function<void(std::string const&)> const& onStatus);
};
}
