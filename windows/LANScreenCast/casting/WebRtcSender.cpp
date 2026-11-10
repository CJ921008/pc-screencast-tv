#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include "WebRtcSender.h"
#include "RemoteInput.h"
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
#include <sstream>
#include <iomanip>
#include <cmath>
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
struct FrameTimer {
    HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    FrameTimer(){if(!handle)throw std::runtime_error("High-resolution frame timer unavailable");}
    ~FrameTimer(){CloseHandle(handle);}
    void until(Clock::time_point deadline) {
        auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-Clock::now()).count();
        if(remaining<=0)return;
        LARGE_INTEGER due{};due.QuadPart=-std::max<int64_t>(1,remaining/100);
        if(!SetWaitableTimer(handle,&due,0,nullptr,nullptr,FALSE))
            throw std::runtime_error("Cannot schedule video frame");
        if(WaitForSingleObject(handle,INFINITE)!=WAIT_OBJECT_0)
            throw std::runtime_error("Video frame timer failed");
    }
};
void StreamWebRtc(std::wstring const& ip, VideoSettings settings, std::atomic_bool const& stop,
    std::shared_ptr<std::atomic_bool> controlPermission,
    std::function<void(std::wstring const&)> const& onStatus) {
    WSADATA sockets{};
    if(WSAStartup(MAKEWORD(2,2),&sockets)!=0)throw std::runtime_error("Winsock initialization failed");
    struct SocketRuntime {~SocketRuntime(){WSACleanup();}} socketRuntime;
    in_addr address{}; if (InetPtonW(AF_INET, ip.c_str(), &address) != 1) throw std::runtime_error("请输入有效的 IPv4 地址");
    rtc::WebSocketConfiguration config; config.connectionTimeout = std::chrono::seconds(3);
    config.maxMessageSize = 1024 * 1024;
    auto ws = std::make_shared<rtc::WebSocket>(config);
    StreamWebRtcSocket(ws, "ws://" + Utf8(ip) + ":47475/signaling", {}, settings, stop, controlPermission, onStatus);
}

void StreamWebRtcSocket(std::shared_ptr<rtc::WebSocket> const& ws, std::string const& lanUrl,
    std::string const& bindIPv6, VideoSettings settings, std::atomic_bool const& stop,
    std::shared_ptr<std::atomic_bool> controlPermission,
    std::function<void(std::wstring const&)> const& onStatus) {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) throw std::runtime_error("COM initialization failed");
    struct Runtime { ~Runtime() { MFShutdown(); CoUninitialize(); } } runtime;
    if (FAILED(MFStartup(MF_VERSION))) throw std::runtime_error("Media Foundation unavailable");
    settings.fps = std::clamp(settings.fps, 10, 30);
    DesktopSource capture;
    auto encoder = std::make_unique<H264Encoder>(capture.Device(), settings);
    settings = encoder->Settings();
    auto shared = std::make_shared<Shared>();
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> control;
    auto input = std::make_shared<RemoteInput>(capture.Bounds(), controlPermission);
    std::jthread controlWatchdog;
    bool separatePointer = false;
    auto lastView = Clock::time_point{};
    std::string previousView;
    std::thread producer;
    std::string session = Id();
    struct Cleanup {
        std::shared_ptr<Shared> state;
        std::thread& producer;
        std::shared_ptr<rtc::WebSocket> ws;
        std::shared_ptr<rtc::PeerConnection>& pc;
        std::shared_ptr<RemoteInput> input;
        std::string session;
        ~Cleanup() {
            input->Close();
            state->done = true; state->ready.notify_all();
            if (producer.joinable()) producer.join();
            try { if (ws->isOpen()) ws->send(Message("disconnect", session, json::object()).dump()); } catch (...) {}
            if (pc) pc->close();
            ws->close();
        }
    } cleanup{shared,producer,ws,pc,input,session};
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
    if (!lanUrl.empty()) ws->open(lanUrl);
    auto connectDeadline = Clock::now() + std::chrono::seconds(3);
    while (!ws->isOpen() && !stop) {
        { std::lock_guard lock(shared->mutex); if (!shared->error.empty()) throw std::runtime_error(shared->error); }
        if (ws->isClosed()) throw std::runtime_error("信令连接已关闭");
        if (Clock::now() > connectDeadline) throw std::runtime_error("信令连接超时，请检查 IP 和端口 47475");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (stop) return;
    auto send = [ws, session](std::string type, json payload = json::object()) {
        if (ws->isOpen()) ws->send(Message(type, session, payload).dump());
    };
    wchar_t hostname[256]{}; DWORD length = 256; GetComputerNameW(hostname, &length);
    send("hello", {{"computerName",Utf8(hostname)}});
    onStatus(bindIPv6.empty() ? L"请在接收设备上接受连接" : L"配对已验证，正在建立远程会话");
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
                if (!bindIPv6.empty()) {
                    config.bindAddress = bindIPv6;
                    config.portRangeBegin = RemoteMediaFirstPort;
                    config.portRangeEnd = RemoteMediaLastPort;
                }
                pc = std::make_shared<rtc::PeerConnection>(config);
                pc->onStateChange([shared,input](rtc::PeerConnection::State state) {
                    if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected) {
                        input->Close();
                        shared->fail("WebRTC 视频连接已断开");
                    }
                });
                pc->onLocalDescription([send,settings](rtc::Description description) {
                    send("offer", {{"sdp",std::string(description)},{"video",{{"width",settings.width},
                        {"height",settings.height},{"fps",settings.fps},{"bitrate",settings.bitrate}}}});
                });
                pc->onLocalCandidate([send, weak = std::weak_ptr(pc)](rtc::Candidate candidate) {
                    int index = 0;
                    if (auto active = weak.lock()) {
                        if (auto description = active->localDescription()) {
                            for (int i = 0; i < description->mediaCount(); ++i) {
                                if (std::visit([](auto* media) { return media->mid(); }, description->media(i)) == candidate.mid()) {
                                    index = i;
                                    break;
                                }
                            }
                        }
                    }
                    send("ice_candidate",{{"candidate",std::string(candidate)},{"mid",candidate.mid()},{"mLineIndex",index}});
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
                if (*controlPermission && payload.value("remoteInput", false)) {
                    control = pc->createDataChannel("lsc-input-v1");
                    control->onMessage([input](rtc::message_variant message) {
                        if (auto text = std::get_if<std::string>(&message)) input->Receive(*text);
                    });
                    control->onClosed([input] { input->Close(); });
                    control->onError([input](std::string) { input->Close(); });
                    controlWatchdog = std::jthread([input](std::stop_token stopToken) {
                        while (!stopToken.stop_requested()) {
                            input->Expire(Clock::now());
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    });
                }
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
                std::wostringstream rate,loss;
                rate<<std::fixed<<std::setprecision(2)<<bytes*8/std::max(.1,elapsed)/1000000;
                loss<<std::fixed<<std::setprecision(1)<<payload.value("lossPercent",0.0);
                auto text = encoderStatus() + L" · 发送 " + std::to_wstring(std::lround(sent / std::max(.1,elapsed))) +
                    L" FPS · 接收 " + std::to_wstring(std::lround(payload.value("fps",0.0))) +
                    L" FPS · " + rate.str() +
                    L" Mbps · RTT " + std::to_wstring(int(payload.value("rttMs",0.0))) +
                    L" ms · 丢包 " + loss.str() + L"%";
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
        if (control && !*controlPermission) {
            input->Close();
            control->close();
            control.reset();
        }
        if (started && control && control->isOpen() && control->bufferedAmount() == 0 &&
            now - lastView >= std::chrono::milliseconds(33)) {
            lastView = now;
            auto bounds = capture.Bounds();
            int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
            auto content = VideoContentRect(width, height, settings);
            json cursor = nullptr;
            CURSORINFO pointer{sizeof(CURSORINFO)};
            if (separatePointer && GetCursorInfo(&pointer) && (pointer.flags & CURSOR_SHOWING) &&
                PtInRect(&bounds, pointer.ptScreenPos)) {
                cursor = {(pointer.ptScreenPos.x - bounds.left) * 10000 / std::max(1, width - 1),
                    (pointer.ptScreenPos.y - bounds.top) * 10000 / std::max(1, height - 1)};
            }
            auto view = json{{"type","view"},{"rect",{content.left * 10000 / settings.width,
                content.top * 10000 / settings.height, content.right * 10000 / settings.width,
                content.bottom * 10000 / settings.height}}, {"cursor",cursor}, {"blocked",input->InputBlocked()}}.dump();
            if (view != previousView) {
                try { control->send(view); }
                catch (std::exception const&) { input->Close(); control->close(); }
                previousView = std::move(view);
            }
        }
        if (now - lastAlive > std::chrono::seconds(6)) throw std::runtime_error("接收端心跳超时");
        if (now - lastPing >= std::chrono::seconds(2)) { send("ping"); lastPing = now; }
        if (!started && now > handshakeDeadline) {
            if (!bindIPv6.empty()) {
                send("error", {{"code","MEDIA_TIMEOUT"},{"message","视频建链超时，请检查 UDP 47500–47509 入站"}});
            }
            throw std::runtime_error("视频建链超时");
        }
        if (shared->trackOpen && !started) {
            started = true; statsStart = Clock::now();
            previousCpu=processTime();
            logging::FileLogger::Write(L"INFO",L"VIDEO",L"ENCODER",encoderStatus());
            onStatus(L"正在投屏 · " + encoderStatus());
            producer = std::thread([shared,&capture,fps=settings.fps] {
                try {
                    FrameTimer timer;
                    auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/fps));
                    while (!shared->done) {
                        auto start = Clock::now(); auto frame = capture.Next(fps);
                        shared->captureUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();
                        if (frame.texture) {
                            std::lock_guard lock(shared->mutex);
                            if (shared->frames.size() == 2) {shared->frames.pop_front();++shared->dropped;}
                            shared->frames.push_back(std::move(frame)); shared->ready.notify_all();
                        }
                        timer.until(start + period);
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
            separatePointer = frame.separatePointer;
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
