#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include "WebRtcSender.h"
#include "SignalProtocol.h"
#include "logging/FileLogger.h"
#include <rtc/rtc.hpp>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stdexcept>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace LANScreenCast::casting {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static std::string Utf8(std::wstring const& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
static std::wstring Wide(std::string const& text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
static std::string Id() {
    GUID guid{}; CoCreateGuid(&guid); wchar_t buffer[40]{}; StringFromGUID2(guid, buffer, 40);
    return Utf8(std::wstring(buffer + 1, 36));
}
static json Message(std::string const& type, std::string const& session, json payload) {
    payload["sessionId"] = session;
    return {{"protocolVersion",2},{"type",type},{"requestId",Id()},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()}, {"payload",payload}};
}
struct Shared {
    std::mutex mutex;
    std::deque<std::string> signals;
    std::deque<VideoFrame> frames;
    std::condition_variable ready;
    std::string error;
    std::atomic_bool done{false}, trackOpen{false}, forceKey{true};
    std::atomic_uint64_t dropped{0}, captureUs{0};
    void fail(std::string const& value) { std::lock_guard lock(mutex); if (!done && error.empty()) error = value; ready.notify_all(); }
};
void StreamWebRtc(std::wstring const& ip, VideoSettings settings, std::atomic_bool const& stop,
    std::function<void(std::wstring const&)> const& onStatus) {
    in_addr address{}; if (InetPtonW(AF_INET, ip.c_str(), &address) != 1) throw std::runtime_error("请输入有效的 IPv4 地址");
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) throw std::runtime_error("COM initialization failed");
    struct Runtime { ~Runtime() { MFShutdown(); CoUninitialize(); } } runtime;
    if (FAILED(MFStartup(MF_VERSION))) throw std::runtime_error("Media Foundation unavailable");
    settings.fps = std::clamp(settings.fps, 10, 30);
    DesktopSource capture;
    auto encoder = std::make_unique<H264Encoder>(capture.Device(), settings);
    settings = encoder->Settings();
    auto shared = std::make_shared<Shared>();
    rtc::WebSocketConfiguration wsConfig; wsConfig.connectionTimeout = std::chrono::seconds(3);
    wsConfig.maxMessageSize = 1024 * 1024;
    auto ws = std::make_shared<rtc::WebSocket>(wsConfig);
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::thread producer;
    std::string session = Id();
    struct Cleanup {
        std::shared_ptr<Shared> state;
        std::thread& producer;
        std::shared_ptr<rtc::WebSocket> ws;
        std::shared_ptr<rtc::PeerConnection>& pc;
        std::string session;
        ~Cleanup() {
            state->done = true; state->ready.notify_all();
            if (producer.joinable()) producer.join();
            try { if (ws->isOpen()) ws->send(Message("disconnect", session, json::object()).dump()); } catch (...) {}
            if (pc) pc->close();
            ws->close();
        }
    } cleanup{shared,producer,ws,pc,session};
    ws->onMessage([shared](rtc::message_variant message) {
        if (auto text = std::get_if<std::string>(&message)) {
            std::lock_guard lock(shared->mutex);
            if (shared->signals.size() >= 128) shared->error = "Signal queue overflow";
            else shared->signals.push_back(*text);
            shared->ready.notify_all();
        }
    });
    ws->onError([shared](std::string value) { shared->fail(value); });
    ws->onClosed([shared] { shared->fail("信令连接已断开"); });
    ws->open("ws://" + Utf8(ip) + ":47475/signaling");
    auto connectDeadline = Clock::now() + std::chrono::seconds(3);
    while (!ws->isOpen() && !stop) {
        { std::lock_guard lock(shared->mutex); if (!shared->error.empty()) throw std::runtime_error(shared->error); }
        if (Clock::now() > connectDeadline) throw std::runtime_error("信令连接超时，请检查 IP 和端口 47475");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (stop) return;
    auto send = [ws, session](std::string type, json payload = json::object()) {
        if (ws->isOpen()) ws->send(Message(type, session, payload).dump());
    };
    wchar_t hostname[256]{}; DWORD length = 256; GetComputerNameW(hostname, &length);
    send("hello", {{"computerName",Utf8(hostname)}});
    onStatus(L"请在接收设备上接受连接");
    auto lastAlive = Clock::now(), lastPing = lastAlive;
    auto handshakeDeadline = lastAlive + std::chrono::seconds(30);
    bool offered = false, answered = false, started = false;
    std::vector<rtc::Candidate> pendingIce;
    auto statsStart = Clock::now(); int sent = 0; uint64_t bytes = 0;
    int64_t lastKeyTime = -20000000;
    uint64_t encodeUs = 0; int encodedFrames = 0;
    auto processTime = [] {
        FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
        ULARGE_INTEGER k{},u{}; k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;
    };
    auto previousCpu = processTime();
    SYSTEM_INFO system{};GetSystemInfo(&system);
    auto emit = [&](AccessUnit unit) {
        if (shared->done || !track || !track->isOpen()) return;
        try {
            track->sendFrame(reinterpret_cast<std::byte const*>(unit.bytes.data()), unit.bytes.size(),
                rtc::FrameInfo(std::chrono::duration<double>(unit.time100ns / 10000000.0)));
        } catch (std::exception const& error) { shared->fail(error.what()); return; }
        ++sent; bytes += unit.bytes.size();
    };
    auto encoderStatus = [&] {
        return std::wstring(encoder->Hardware() ? L"硬件 " : L"当前显卡硬编码不可用，软件 720p ") + encoder->Name() +
            L" · " + std::to_wstring(settings.width) + L"×" + std::to_wstring(settings.height);
    };
    while (!stop) {
        std::deque<std::string> messages;
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->error.empty()) throw std::runtime_error(shared->error);
            messages.swap(shared->signals);
        }
        for (auto const& raw : messages) {
            auto message = json::parse(raw);
            ValidateSignal(message,session);
            auto payload = message.at("payload");
            if (message.at("type") == "error") throw std::runtime_error(payload.value("message","Receiver error"));
            if (payload.value("sessionId","") != session) throw std::runtime_error("Invalid signal session");
            lastAlive = Clock::now();
            auto type = message.at("type").get<std::string>();
            if (type == "ping") send("pong");
            else if (type == "pong") {}
            else if (type == "error") throw std::runtime_error(payload.value("message","Receiver error"));
            else if (type == "disconnect") return;
            else if (type == "capabilities") {
                if (offered || !payload.value("h264",false)) throw std::runtime_error("Receiver has no negotiated H264 codec");
                if (settings.width == 1920 && !payload.value("supports1080p30",false))
                    throw std::runtime_error("接收设备不支持 1080p30，请选择 720p");
                rtc::Configuration config; config.disableAutoNegotiation = true; config.enableIceTcp = false;
                pc = std::make_shared<rtc::PeerConnection>(config);
                pc->onStateChange([shared](rtc::PeerConnection::State state) {
                    if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected)
                        shared->fail("WebRTC 视频连接已断开");
                });
                pc->onLocalDescription([send,settings](rtc::Description description) {
                    send("offer", {{"sdp",std::string(description)},{"video",{{"width",settings.width},
                        {"height",settings.height},{"fps",settings.fps},{"bitrate",settings.bitrate}}}});
                });
                pc->onLocalCandidate([send](rtc::Candidate candidate) {
                    send("ice_candidate",{{"candidate",std::string(candidate)},{"mid",candidate.mid()},{"mLineIndex",0}});
                });
                rtc::Description::Video video("video",rtc::Description::Direction::SendOnly);
                video.addH264Codec(102, std::string("profile-level-id=") + (settings.width == 1920 ? "42e028" : "42e01f") +
                    ";packetization-mode=1;level-asymmetry-allowed=1");
                video.addSSRC(42,"lanscreencast","screen","video");
                track = pc->addTrack(video);
                auto configRtp = std::make_shared<rtc::RtpPacketizationConfig>(42,"lanscreencast",102,90000);
                auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,configRtp);
                packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(configRtp));
                packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
                packetizer->addToChain(std::make_shared<rtc::PliHandler>([shared] { shared->forceKey = true; }));
                track->setMediaHandler(packetizer);
                track->onOpen([shared] { shared->trackOpen = true; });
                offered = true; handshakeDeadline = Clock::now() + std::chrono::seconds(10);
                pc->setLocalDescription();
                onStatus(L"正在建立 H.264 视频连接 · " + encoderStatus());
            } else if (type == "answer") {
                if (!pc || answered) throw std::runtime_error("Unexpected SDP answer");
                pc->setRemoteDescription(rtc::Description(payload.at("sdp").get<std::string>(),"answer"));
                answered = true;
                for (auto const& candidate : pendingIce) pc->addRemoteCandidate(candidate);
                pendingIce.clear();
            } else if (type == "ice_candidate") {
                rtc::Candidate candidate(payload.at("candidate").get<std::string>(),payload.value("mid","video"));
                if (answered) pc->addRemoteCandidate(candidate);
                else { if (pendingIce.size() >= 128) throw std::runtime_error("Too many ICE candidates"); pendingIce.push_back(candidate); }
            } else if (type == "stats") {
                if (!started) continue;
                auto elapsed = std::chrono::duration<double>(Clock::now() - statsStart).count();
                auto cpuTime = processTime();
                double cpu = (cpuTime-previousCpu)/10000000.0/std::max(.1,elapsed)/system.dwNumberOfProcessors*100;
                auto text = encoderStatus() + L" · 发送 " + std::to_wstring(int(sent / std::max(.1,elapsed))) +
                    L" FPS · 接收 " + std::to_wstring(int(payload.value("fps",0.0))) +
                    L" FPS · " + std::to_wstring(int(bytes * 8 / std::max(.1,elapsed) / 1000000)) +
                    L" Mbps · RTT " + std::to_wstring(int(payload.value("rttMs",0.0))) +
                    L" ms · 丢包 " + std::to_wstring(int(payload.value("lossPercent",0.0))) + L"%";
                onStatus(text);
                payload["encoder"]=Utf8(encoder->Name());payload["hardware"]=encoder->Hardware();
                payload["width"]=settings.width;payload["height"]=settings.height;payload["cpuPercent"]=cpu;
                payload["encodeMs"]=encodedFrames ? encodeUs/1000.0/encodedFrames : 0;
                payload["captureMs"]=shared->captureUs.load()/1000.0;payload["queueDropped"]=shared->dropped.load();
                logging::FileLogger::Write(L"INFO",L"VIDEO",L"STATS",Wide(payload.dump()));
                statsStart = Clock::now(); sent = 0; bytes = 0;
                previousCpu=cpuTime;encodeUs=0;encodedFrames=0;
            } else throw std::runtime_error("Unknown receiver message");
        }
        auto now = Clock::now();
        if (now - lastAlive > std::chrono::seconds(6)) throw std::runtime_error("接收端心跳超时");
        if (now - lastPing >= std::chrono::seconds(2)) { send("ping"); lastPing = now; }
        if (!started && now > handshakeDeadline) throw std::runtime_error("视频建链超时");
        if (shared->trackOpen && !started) {
            started = true; statsStart = Clock::now();
            logging::FileLogger::Write(L"INFO",L"VIDEO",L"ENCODER",encoderStatus());
            onStatus(L"正在投屏 · " + encoderStatus());
            producer = std::thread([shared,&capture,fps=settings.fps] {
                try {
                    auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/fps));
                    while (!shared->done) {
                        auto start = Clock::now(); auto frame = capture.Next(fps);
                        shared->captureUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();
                        if (frame.texture) {
                            std::lock_guard lock(shared->mutex);
                            if (shared->frames.size() == 2) {shared->frames.pop_front();++shared->dropped;}
                            shared->frames.push_back(std::move(frame)); shared->ready.notify_all();
                        }
                        std::this_thread::sleep_until(start + period);
                    }
                } catch (std::exception const& error) { shared->fail(error.what()); }
            });
        }
        VideoFrame frame;
        {
            std::unique_lock lock(shared->mutex);
            if (shared->frames.empty()) shared->ready.wait_for(lock,std::chrono::milliseconds(10));
            if (!shared->frames.empty()) { frame = std::move(shared->frames.front()); shared->frames.pop_front(); }
        }
        if (frame.texture) {
            auto encodeStart=Clock::now();
            bool key = shared->forceKey.exchange(false) || frame.time100ns-lastKeyTime >= 20000000;
            if (key) lastKeyTime = frame.time100ns;
            try { encoder->Encode(frame,key,stop,emit); }
            catch (std::exception const& error) {
                if (!encoder->Hardware()) throw;
                logging::FileLogger::Write(L"WARN",L"VIDEO",L"HARDWARE_FALLBACK",Wide(error.what()));
                encoder = std::make_unique<H264Encoder>(capture.Device(),settings,true);
                settings = encoder->Settings(); shared->forceKey = true;
                onStatus(L"硬件编码失败，已切换软件 720p");
                encoder->Encode(frame,true,stop,emit);
            }
            encodeUs+=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-encodeStart).count();
            ++encodedFrames;
        }
    }
}
}
