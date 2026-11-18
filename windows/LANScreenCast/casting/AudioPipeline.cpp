#include "AudioPipeline.h"
#include <opus/opus.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace LANScreenCast::casting {
struct OpusAudioEncoder::Impl {
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{nullptr, opus_encoder_destroy};

    Impl() {
        int error = OPUS_OK;
        encoder.reset(opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_AUDIO, &error));
        if (error != OPUS_OK || !encoder) throw std::runtime_error("Cannot initialize Opus audio encoder");
        if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(128000)) != OPUS_OK ||
            opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
            opus_encoder_ctl(encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPUS_OK)
            throw std::runtime_error("Cannot configure Opus audio encoder");
    }
};

OpusAudioEncoder::OpusAudioEncoder() : impl(std::make_unique<Impl>()) {}
OpusAudioEncoder::~OpusAudioEncoder() = default;

std::vector<uint8_t> OpusAudioEncoder::Encode(std::span<float const> samples) {
    if (samples.size() != FrameSamples * Channels)
        throw std::invalid_argument("An Opus frame requires 960 stereo samples");
    std::vector<uint8_t> packet(4000);
    auto count = opus_encode_float(impl->encoder.get(), samples.data(), FrameSamples, packet.data(),
        static_cast<opus_int32>(packet.size()));
    if (count < 0) throw std::runtime_error(std::string("Opus encoding failed: ") + opus_strerror(count));
    packet.resize(static_cast<size_t>(count));
    return packet;
}
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <sstream>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "opus.lib")

namespace LANScreenCast::casting {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int Rate = OpusAudioEncoder::SampleRate;
constexpr int Channels = OpusAudioEncoder::Channels;
constexpr int FrameSamples = OpusAudioEncoder::FrameSamples;
constexpr int BufferedSamples = Rate / 5;
constexpr auto CaptureMargin = 40ms;

void Check(HRESULT result, char const* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream message;
    message << operation << " (HRESULT 0x" << std::hex << static_cast<unsigned>(result) << ')';
    throw std::runtime_error(message.str());
}

std::wstring DeviceId(IMMDevice& device) {
    wchar_t* id = nullptr;
    Check(device.GetId(&id), "Cannot identify the audio output device");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(id, CoTaskMemFree);
    return id;
}

// Tags keep holes silent and discard late samples, without queuing old audio after a stall.
class AudioTimeline {
    std::array<float, BufferedSamples * Channels> samples{};
    std::array<int64_t, BufferedSamples> tags;
    int64_t nextSample;
public:
    explicit AudioTimeline(int64_t firstSample) : nextSample(firstSample) { Clear(); }

    void Clear() { tags.fill(-1); }
    int64_t NextSample() const { return nextSample; }

    void SkipTo(int64_t firstSample) {
        nextSample = firstSample;
        Clear();
    }

    void Write(float const* input, UINT32 frames, int64_t firstSample, bool silent) {
        for (UINT32 frame = 0; frame < frames; ++frame) {
            auto const position = firstSample + frame;
            if (position < nextSample || position >= nextSample + BufferedSamples) continue;
            auto const index = static_cast<size_t>(position % BufferedSamples);
            tags[index] = position;
            for (int channel = 0; channel < Channels; ++channel) {
                auto value = silent ? 0.f : input[static_cast<size_t>(frame) * Channels + channel];
                samples[index * Channels + channel] = std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f;
            }
        }
    }

    std::array<float, FrameSamples * Channels> Read() {
        std::array<float, FrameSamples * Channels> frame{};
        for (int offset = 0; offset < FrameSamples; ++offset) {
            auto const position = nextSample + offset;
            auto const index = static_cast<size_t>(position % BufferedSamples);
            if (tags[index] != position) continue;
            std::copy_n(samples.data() + index * Channels, Channels, frame.data() + offset * Channels);
            tags[index] = -1;
        }
        nextSample += FrameSamples;
        return frame;
    }
};

struct CaptureDevice {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::wstring id;

    CaptureDevice(IMMDeviceEnumerator& enumerator) {
        ComPtr<IMMDevice> device;
        Check(enumerator.GetDefaultAudioEndpoint(eRender, eConsole, &device), "No default audio output device");
        id = DeviceId(*device.Get());
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Cannot open system audio capture");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = Channels;
        format.nSamplesPerSec = Rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = Channels * sizeof(float);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            1000000, 0, &format, nullptr), "Cannot configure 48 kHz stereo loopback capture");
        Check(client->GetService(IID_PPV_ARGS(&capture)), "Cannot obtain the system audio stream");
        Check(client->Start(), "Cannot start system audio capture");
    }

    ~CaptureDevice() { if (client) client->Stop(); }

    bool IsDefault(IMMDeviceEnumerator& enumerator) const {
        ComPtr<IMMDevice> device;
        return SUCCEEDED(enumerator.GetDefaultAudioEndpoint(eRender, eConsole, &device)) && DeviceId(*device.Get()) == id;
    }

    void Drain(AudioTimeline& timeline, int64_t qpcOrigin100ns, int64_t now100ns) {
        UINT32 available = 0;
        Check(capture->GetNextPacketSize(&available), "Cannot read system audio packet size");
        while (available) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            Check(capture->GetBuffer(&data, &frames, &flags, nullptr, &position), "Cannot read system audio samples");
            struct Release {
                IAudioCaptureClient& client;
                UINT32 frames;
                ~Release() { if (frames) client.ReleaseBuffer(frames); }
            } release{*capture.Get(), frames};
            int64_t packetTime = static_cast<int64_t>(position) - qpcOrigin100ns;
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || packetTime < 0 || packetTime > now100ns + 1000000)
                packetTime = now100ns - static_cast<int64_t>(frames) * 10000000 / Rate;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) timeline.Clear();
            timeline.Write(reinterpret_cast<float const*>(data), frames, packetTime * Rate / 10000000,
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr);
            // ReleaseBuffer must precede querying the next packet.
            Check(capture->ReleaseBuffer(frames), "Cannot release system audio samples");
            release.frames = 0;
            Check(capture->GetNextPacketSize(&available), "Cannot read system audio packet size");
        }
    }
};
}

SystemAudioSender::SystemAudioSender(Clock::time_point clockOrigin) : origin(clockOrigin) {}

void SystemAudioSender::Run(std::atomic_bool const& stop,
    std::function<void(std::vector<uint8_t>, int64_t)> const& emit,
    std::function<void(std::string const&)> const& onStatus) {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Cannot initialize audio capture COM");
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    OpusAudioEncoder encoder;
    ComPtr<IMMDeviceEnumerator> enumerator;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
        "Cannot enumerate audio output devices");
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    auto const anchor = Clock::now();
    auto const anchor100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(anchor - origin).count() / 100;
    auto const qpcOrigin100ns = static_cast<int64_t>(static_cast<long double>(counter.QuadPart) * 10000000 / frequency.QuadPart) - anchor100ns;
    auto const firstSample = anchor100ns * Rate / 10000000 / FrameSamples * FrameSamples;
    AudioTimeline timeline(firstSample);
    std::unique_ptr<CaptureDevice> device;
    auto deviceCheck = Clock::time_point{};
    std::string previousStatus;
    auto report = [&](std::string const& message) {
        if (message == previousStatus) return;
        previousStatus = message;
        onStatus(message);
    };
    while (!stop) {
        auto const now = Clock::now();
        auto const now100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin).count() / 100;
        try {
            if (now >= deviceCheck) {
                deviceCheck = now + 1s;
                if (device && !device->IsDefault(*enumerator.Get())) {
                    device.reset();
                    timeline.Clear();
                }
                if (!device) {
                    device = std::make_unique<CaptureDevice>(*enumerator.Get());
                    report("系统声音 · Opus 48kHz 双声道 · 128kbps");
                }
            }
            if (device) device->Drain(timeline, qpcOrigin100ns, now100ns);
        } catch (std::exception const& error) {
            device.reset();
            timeline.Clear();
            report(std::string("系统声音暂不可用，正在重试 · ") + error.what());
        }

        auto next = timeline.NextSample();
        auto const readyAt = origin + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<int64_t, std::ratio<1, Rate>>(next + FrameSamples)) + CaptureMargin;
        if (now >= readyAt) {
            if (now - readyAt > 100ms) {
                // A blocked sender must resume with current audio instead of replaying its backlog.
                auto current = (now100ns - 400000) * Rate / 10000000;
                timeline.SkipTo(std::max(next, current / FrameSamples * FrameSamples));
            } else {
                auto pcm = timeline.Read();
                emit(encoder.Encode(pcm), next * 10000000 / Rate);
            }
        }
        std::this_thread::sleep_for(5ms);
    }
}
}
#endif
