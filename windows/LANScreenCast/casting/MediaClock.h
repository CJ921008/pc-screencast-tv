#pragma once
#include <rtc/rtc.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace LANScreenCast::casting {
class MediaClock {
    const std::chrono::system_clock::time_point wallOrigin;

public:
    using Clock = std::chrono::steady_clock;
    const Clock::time_point origin;

    MediaClock(Clock::time_point start = Clock::now(),
        std::chrono::system_clock::time_point wall = std::chrono::system_clock::now())
        : wallOrigin(wall), origin(start) {}

    uint64_t NtpAt(Clock::time_point now) const {
        auto elapsed = now - origin;
        auto unixTime = std::chrono::duration_cast<std::chrono::nanoseconds>(wallOrigin.time_since_epoch() + elapsed).count();
        auto seconds = static_cast<uint64_t>(unixTime / 1000000000) + 2208988800ULL;
        auto fraction = static_cast<uint64_t>(unixTime % 1000000000) * (1ULL << 32) / 1000000000;
        return (seconds << 32) | fraction;
    }

    uint32_t RtpAt(rtc::RtpPacketizationConfig const& config, Clock::time_point now) const {
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin).count();
        auto ticks = static_cast<uint64_t>(elapsed / 1000000000) * config.clockRate +
            static_cast<uint64_t>(elapsed % 1000000000) * config.clockRate / 1000000000;
        return config.startTimestamp + static_cast<uint32_t>(ticks);
    }
};

// RTP frame times and SR wall times must describe the same instant, including capture/encode delay.
class SynchronizedRtcpReporter final : public rtc::MediaHandler {
    const std::shared_ptr<rtc::RtpPacketizationConfig> config;
    const std::shared_ptr<MediaClock const> clock;
    rtc::RtcpSrReporter reporter;

public:
    SynchronizedRtcpReporter(std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig,
        std::shared_ptr<MediaClock const> mediaClock)
        : config(std::move(rtpConfig)), clock(std::move(mediaClock)), reporter(config) {}

    void outgoing(rtc::message_vector& messages, rtc::message_callback const& send) override {
        reporter.outgoing(messages, [&](rtc::message_ptr message) {
            if (message->type == rtc::Message::Control && message->size() >= rtc::RtcpSr::Size(0)) {
                auto* report = reinterpret_cast<rtc::RtcpSr*>(message->data());
                if (report->header.payloadType() == 200 && report->senderSSRC() == config->ssrc) {
                    auto now = MediaClock::Clock::now();
                    report->setNtpTimestamp(clock->NtpAt(now));
                    report->setRtpTimestamp(clock->RtpAt(*config, now));
                }
            }
            send(std::move(message));
        });
    }
};
}
