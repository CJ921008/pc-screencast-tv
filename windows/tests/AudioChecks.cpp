#include "../LANScreenCast/casting/AudioPipeline.h"
#include "../LANScreenCast/casting/MediaClock.h"
#include <opus/opus.h>
#include <rtc/rtc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace LANScreenCast::casting;
using namespace std::chrono_literals;

void Require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::vector<uint8_t>> CheckOpus() {
    OpusAudioEncoder encoder;
    int error = OPUS_OK;
    auto decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>(
        opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
    Require(decoder && error == OPUS_OK, "Opus stereo decoder initialization failed");
    std::vector<std::vector<uint8_t>> packets;
    double leftEnergy = 0, rightEnergy = 0, differenceEnergy = 0;
    for (int frame = 0; frame < 50; ++frame) {
        std::array<float, 1920> source;
        for (int sample = 0; sample < 960; ++sample) {
            double time = double(frame * 960 + sample) / 48000;
            source[sample * 2] = static_cast<float>(.3 * std::sin(2 * 3.141592653589793 * 440 * time));
            source[sample * 2 + 1] = static_cast<float>(.3 * std::sin(2 * 3.141592653589793 * 880 * time));
        }
        auto encoded = encoder.Encode(source);
        Require(!encoded.empty(), "Opus returned an empty encoded frame");
        Require(opus_packet_get_nb_samples(encoded.data(), static_cast<opus_int32>(encoded.size()), 48000) == 960,
            "Opus packet does not describe 20 milliseconds at 48 kHz");
        std::array<float, 1920> decoded;
        auto samples = opus_decode_float(decoder.get(), encoded.data(), static_cast<opus_int32>(encoded.size()),
            decoded.data(), 960, 0);
        Require(samples == 960, "Opus roundtrip changed the audio sample count");
        for (int sample = 0; sample < samples; ++sample) {
            auto left = decoded[sample * 2], right = decoded[sample * 2 + 1];
            Require(std::isfinite(left) && std::isfinite(right), "Opus decoded non-finite PCM");
            leftEnergy += left * left;
            rightEnergy += right * right;
            differenceEnergy += (left - right) * (left - right);
        }
        packets.push_back(std::move(encoded));
    }
    Require(leftEnergy > 100 && rightEnergy > 100 && differenceEnergy > 100,
        "Opus stereo channels were silent or collapsed to mono");
    bool rejected = false;
    try { encoder.Encode(std::array<float, 3>{}); }
    catch (std::exception const&) { rejected = true; }
    Require(rejected, "Opus encoder accepted an incomplete interleaved frame");
    return packets;
}

void CheckMediaClock() {
    auto start = MediaClock::Clock::now();
    auto wall = std::chrono::system_clock::time_point{1700000000s + 125ms};
    auto clock = std::make_shared<MediaClock>(start, wall);
    rtc::RtpPacketizationConfig audio(43, "same-session", 111, 48000);
    rtc::RtpPacketizationConfig video(42, "same-session", 102, 90000);
    audio.startTimestamp = 0xfffffff0;
    video.startTimestamp = 0xffffff00;
    Require(clock->RtpAt(audio, start + 20ms) == uint32_t(audio.startTimestamp + 960),
        "Audio RTP timestamps did not wrap at 32 bits");
    Require(clock->RtpAt(video, start + 20ms) == uint32_t(video.startTimestamp + 1800),
        "Video RTP timestamps did not map the shared clock");
    auto fractionalNtp = (uint64_t(1700000000) + 2208988800ULL) << 32;
    fractionalNtp += (1ULL << 32) / 8;
    Require(clock->NtpAt(start) == fractionalNtp, "Media clock NTP epoch is incorrect");
    Require(clock->NtpAt(start + 1250ms) == fractionalNtp + (1ULL << 32) + (1ULL << 30),
        "Media clock NTP fraction accumulated drift");

    auto delayedClock = std::make_shared<MediaClock>(start - 250ms, wall);
    auto config = std::make_shared<rtc::RtpPacketizationConfig>(43, "same-session", 111, 48000);
    config->startTimestamp = 0xfffffff0;
    SynchronizedRtcpReporter reporter(config, delayedClock);
    auto packet = rtc::make_message(sizeof(rtc::RtpHeader) + 1);
    auto* header = reinterpret_cast<rtc::RtpHeader*>(packet->data());
    header->setSsrc(config->ssrc);
    header->setPayloadType(111);
    header->setTimestamp(config->startTimestamp);
    rtc::message_vector messages{packet};
    rtc::message_ptr report;
    auto before = MediaClock::Clock::now();
    reporter.outgoing(messages, [&](rtc::message_ptr value) { report = std::move(value); });
    auto after = MediaClock::Clock::now();
    Require(report && report->type == rtc::Message::Control && report->size() >= rtc::RtcpSr::Size(0),
        "Synchronized reporter did not generate an SR");
    auto* sr = reinterpret_cast<rtc::RtcpSr*>(report->data());
    Require(sr->senderSSRC() == 43 && sr->packetCount() == 1, "Sender report accounting was lost");
    Require(sr->ntpTimestamp() >= delayedClock->NtpAt(before) && sr->ntpTimestamp() <= delayedClock->NtpAt(after),
        "Sender report did not use the common NTP epoch");
    Require(uint32_t(sr->rtpTimestamp() - config->startTimestamp) >= 12000,
        "Sender report mapped delayed PCM time to current wall time");
    double ntpElapsed = double(sr->ntpTimestamp() - delayedClock->NtpAt(delayedClock->origin)) / double(1ULL << 32);
    double rtpElapsed = double(uint32_t(sr->rtpTimestamp() - config->startTimestamp)) / 48000;
    Require(std::abs(ntpElapsed - rtpElapsed) <= 1.0 / 48000, "SR audio RTP and NTP timestamps refer to different instants");
}

struct Received {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<uint32_t> audioTimes;
    std::vector<uint32_t> videoTimes;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    int decodedSamples = 0;
    int decodeError = OPUS_OK;
};

void CheckRtc(std::vector<std::vector<uint8_t>> const& packets, bool ipv6) {
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    config.bindAddress = ipv6 ? "::1" : "127.0.0.1";
    auto sender = std::make_shared<rtc::PeerConnection>(config);
    auto receiver = std::make_shared<rtc::PeerConnection>(config);
    auto received = std::make_shared<Received>();
    int error = OPUS_OK;
    auto decoder = std::shared_ptr<OpusDecoder>(opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
    Require(decoder && error == OPUS_OK, "Loopback decoder initialization failed");
    receiver->onTrack([received, decoder](std::shared_ptr<rtc::Track> track) {
        bool audio = track->description().mid() == "audio";
        std::shared_ptr<rtc::MediaHandler> handler;
        if (audio) handler = std::make_shared<rtc::OpusRtpDepacketizer>();
        else handler = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track->setMediaHandler(handler);
        track->onFrame([received, decoder, audio](rtc::binary bytes, rtc::FrameInfo info) {
            std::lock_guard lock(received->mutex);
            if (audio) {
                std::array<float, 1920> pcm;
                auto frames = opus_decode_float(decoder.get(), reinterpret_cast<unsigned char const*>(bytes.data()),
                    static_cast<opus_int32>(bytes.size()), pcm.data(), 960, 0);
                if (frames < 0) received->decodeError = frames;
                else received->decodedSamples += frames;
                received->audioTimes.push_back(info.timestamp);
            } else received->videoTimes.push_back(info.timestamp);
            received->ready.notify_all();
        });
        std::lock_guard lock(received->mutex);
        received->tracks.push_back(std::move(track));
    });
    sender->onGatheringStateChange([send = std::weak_ptr(sender), receive = std::weak_ptr(receiver)](auto state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) return;
        if (auto source = send.lock(), sink = receive.lock(); source && sink) {
            sink->setRemoteDescription(*source->localDescription());
            sink->setLocalDescription();
        }
    });
    receiver->onGatheringStateChange([send = std::weak_ptr(sender), receive = std::weak_ptr(receiver)](auto state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) return;
        if (auto source = send.lock(), sink = receive.lock(); source && sink)
            source->setRemoteDescription(*sink->localDescription());
    });
    auto clock = std::make_shared<MediaClock>();
    rtc::Description::Audio audioDescription("audio", rtc::Description::Direction::SendOnly);
    audioDescription.addOpusCodec(111);
    audioDescription.addSSRC(43, "same-session", "screen", "audio");
    auto audio = sender->addTrack(audioDescription);
    auto audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(43, "same-session", 111, 48000);
    auto audioHandler = std::make_shared<rtc::OpusRtpPacketizer>(audioConfig);
    audioHandler->addToChain(std::make_shared<SynchronizedRtcpReporter>(audioConfig, clock));
    audio->setMediaHandler(audioHandler);
    rtc::Description::Video videoDescription("video", rtc::Description::Direction::SendOnly);
    videoDescription.addH264Codec(102, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
    videoDescription.addSSRC(42, "same-session", "screen", "video");
    auto video = sender->addTrack(videoDescription);
    auto videoConfig = std::make_shared<rtc::RtpPacketizationConfig>(42, "same-session", 102, 90000);
    auto videoHandler = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, videoConfig);
    videoHandler->addToChain(std::make_shared<SynchronizedRtcpReporter>(videoConfig, clock));
    video->setMediaHandler(videoHandler);
    sender->setLocalDescription();
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((!audio->isOpen() || !video->isOpen()) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    Require(audio->isOpen() && video->isOpen(), "Combined audio/video WebRTC connection timed out");
    rtc::Candidate local, remote;
    Require(sender->getSelectedCandidatePair(&local, &remote), "WebRTC candidate pair unavailable");
    Require(local.address() == config.bindAddress && remote.address() == config.bindAddress,
        "Audio/video loopback selected an unexpected address family");
    auto sdp = std::string(*sender->localDescription());
    Require(sdp.find("a=group:BUNDLE") != std::string::npos && sdp.find("opus/48000/2") != std::string::npos,
        "Opus stereo was not bundled into the video session");
    const std::array<std::byte, 7> h264{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x65}, std::byte{0x88}, std::byte{0x84}};
    for (size_t index = 0; index < packets.size(); ++index) {
        auto at = rtc::FrameInfo(std::chrono::duration<double>(2.5 + .02 * index));
        auto const& packet = packets[index];
        audio->sendFrame(reinterpret_cast<std::byte const*>(packet.data()), packet.size(), at);
        if (index % 2 == 0) video->sendFrame(h264.data(), h264.size(), at);
        std::this_thread::sleep_for(20ms);
    }
    {
        std::unique_lock lock(received->mutex);
        Require(received->ready.wait_for(lock, 3s, [&] {
            return received->audioTimes.size() == packets.size() && received->videoTimes.size() == packets.size() / 2;
        }), "Audio/video loopback lost media packets");
        Require(received->decodeError == OPUS_OK && received->decodedSamples == 48000,
            "Network Opus frames did not decode to one second of 48 kHz stereo");
        for (size_t index = 0; index < received->audioTimes.size(); ++index) {
            auto expectedAudio = audioConfig->startTimestamp + uint32_t(120000 + 960 * index);
            Require(received->audioTimes[index] == expectedAudio, "Audio RTP clock drifted from 20 ms PCM frames");
        }
        for (size_t index = 0; index < received->videoTimes.size(); ++index) {
            auto audioTicks = uint32_t(received->audioTimes[index * 2] - audioConfig->startTimestamp);
            auto videoTicks = uint32_t(received->videoTimes[index] - videoConfig->startTimestamp);
            Require(std::abs(double(audioTicks) / 48000 - double(videoTicks) / 90000) <= 1.0 / 48000,
                "Audio and video lost their shared capture timestamps");
        }
    }
    sender->close();
    receiver->close();
    std::cout << (ipv6 ? "IPv6" : "IPv4") << " bundled Opus/video roundtrip verified\n";
}
}

int main() {
    try {
        auto packets = CheckOpus();
        CheckMediaClock();
        CheckRtc(packets, false);
        CheckRtc(packets, true);
        rtc::Cleanup().get();
        std::cout << "48 kHz stereo Opus and synchronized RTP/NTP checks passed\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
