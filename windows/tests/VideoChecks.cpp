#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <rtc/rtc.hpp>
#include "../LANScreenCast/casting/MediaPipeline.h"
#include "../LANScreenCast/casting/SignalProtocol.h"
#include "../LANScreenCast/casting/RemoteInput.h"
#include "../LANScreenCast/casting/RemoteAccess.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace LANScreenCast::casting;
using Microsoft::WRL::ComPtr;
static void require(bool ok, char const* label) { if (!ok) throw std::runtime_error(label); }
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Media Foundation test HRESULT " + std::to_string(unsigned(hr))); }
static int decode(std::vector<AccessUnit> const& units) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video,MFVideoFormat_H264}, out{MFMediaType_Video,MFVideoFormat_NV12};
    IMFActivate** list = nullptr; UINT32 count = 0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,MFT_ENUM_FLAG_SYNCMFT|MFT_ENUM_FLAG_SORTANDFILTER,&in,&out,&list,&count));
    require(count>0,"No software decoder");
    ComPtr<IMFTransform> decoder; check(list[0]->ActivateObject(IID_PPV_ARGS(&decoder)));
    for (UINT32 i=0;i<count;++i) list[i]->Release(); CoTaskMemFree(list);
    ComPtr<IMFMediaType> input; check(MFCreateMediaType(&input));
    input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264);
    MFSetAttributeSize(input.Get(),MF_MT_FRAME_SIZE,1280,720);
    MFSetAttributeRatio(input.Get(),MF_MT_FRAME_RATE,30,1);
    check(decoder->SetInputType(0,input.Get(),0));
    auto negotiate = [&] {
        for (DWORD index=0;;++index) {
            ComPtr<IMFMediaType> type;
            if (FAILED(decoder->GetOutputAvailableType(0,index,&type))) break;
            GUID format{}; type->GetGUID(MF_MT_SUBTYPE,&format);
            if (format==MFVideoFormat_NV12) { check(decoder->SetOutputType(0,type.Get(),0)); return; }
        }
        throw std::runtime_error("Decoder NV12 output unavailable");
    };
    negotiate();
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0);
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
    int frames=0;
    auto read = [&] {
        while (true) {
            MFT_OUTPUT_STREAM_INFO info{}; check(decoder->GetOutputStreamInfo(0,&info));
            ComPtr<IMFSample> sample; check(MFCreateSample(&sample));
            ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize,1280*720*2),&buffer));
            sample->AddBuffer(buffer.Get()); MFT_OUTPUT_DATA_BUFFER output{}; output.pSample=sample.Get(); DWORD status=0;
            auto hr=decoder->ProcessOutput(0,1,&output,&status);
            if(output.pEvents) output.pEvents->Release();
            if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if(hr==MF_E_TRANSFORM_STREAM_CHANGE) {negotiate();continue;}
            check(hr); ++frames;
        }
    };
    for(auto const& unit:units) {
        ComPtr<IMFMediaBuffer> buffer; check(MFCreateMemoryBuffer(DWORD(unit.bytes.size()),&buffer));
        BYTE* data=nullptr; check(buffer->Lock(&data,nullptr,nullptr));
        memcpy(data,unit.bytes.data(),unit.bytes.size()); buffer->Unlock();buffer->SetCurrentLength(DWORD(unit.bytes.size()));
        ComPtr<IMFSample> sample; check(MFCreateSample(&sample));sample->AddBuffer(buffer.Get());sample->SetSampleTime(unit.time100ns);
        check(decoder->ProcessInput(0,sample.Get(),0));read();
    }
    decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0);read();
    return frames;
}
static void rtcLoopback(std::vector<AccessUnit> const& units) {
    rtc::Configuration config; config.disableAutoNegotiation=true;
    auto sender=std::make_shared<rtc::PeerConnection>(config);
    auto receiver=std::make_shared<rtc::PeerConnection>(config);
    std::atomic_int received{0}; std::atomic_bool open{false};
    std::atomic_uint keys{0};
    auto allowed = std::make_shared<std::atomic_bool>(true);
    auto input = std::make_shared<RemoteInput>(RECT{0,0,1920,1080}, allowed, [&](std::span<INPUT> events) {
        if (events.size() == 2 && events[0].ki.wScan == L'中' &&
            events[0].ki.dwFlags == KEYEVENTF_UNICODE &&
            events[1].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP)) ++keys;
        return static_cast<UINT>(events.size());
    });
    std::shared_ptr<rtc::DataChannel> receivedControl;
    receiver->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel) {
        if (channel->label() != "lsc-input-v1") return;
        receivedControl = channel;
        channel->send(std::string(R"({"type":"resume"})"));
        channel->send(std::string(R"({"type":"text","text":"中"})"));
        channel->send(std::string(R"({"type":"pause"})"));
        channel->send(std::string(R"({"type":"text","text":"中"})"));
    });
    std::shared_ptr<rtc::Track> incoming;
    receiver->onTrack([&](std::shared_ptr<rtc::Track> track) {
        incoming=track;
        auto handler=std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track->setMediaHandler(handler);
        track->onFrame([&](rtc::binary bytes,rtc::FrameInfo) {if(!bytes.empty())++received;});
    });
    sender->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state){
        if(state==rtc::PeerConnection::GatheringState::Complete) {
            receiver->setRemoteDescription(*sender->localDescription()); receiver->setLocalDescription();
        }
    });
    receiver->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state){
        if(state==rtc::PeerConnection::GatheringState::Complete) sender->setRemoteDescription(*receiver->localDescription());
    });
    rtc::Description::Video video("video",rtc::Description::Direction::SendOnly);
    video.addH264Codec(102,"profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
    video.addSSRC(42,"test","screen","video");
    auto track=sender->addTrack(video);
    auto rtp=std::make_shared<rtc::RtpPacketizationConfig>(42,"test",102,90000);
    auto handler=std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,rtp);
    handler->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    handler->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    track->setMediaHandler(handler);track->onOpen([&]{open=true;});
    auto channel = sender->createDataChannel("lsc-input-v1");
    channel->onMessage([input](rtc::message_variant message) {
        if (auto text = std::get_if<std::string>(&message)) input->Receive(*text);
    });
    sender->setLocalDescription();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!open && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(open,"WebRTC connection timeout");
    for(auto const& unit:units) {
        track->sendFrame(reinterpret_cast<std::byte const*>(unit.bytes.data()),unit.bytes.size(),
            rtc::FrameInfo(std::chrono::duration<double>(unit.time100ns/10000000.0)));
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sender->close();receiver->close();
    require(received>=50,"WebRTC H264 frames were lost in loopback");
    require(keys == 1, "Data channel did not deliver Unicode input only while active");
    std::cout<<"WebRTC frames received: "<<received<<std::endl;
}
static void checkRemoteInput() {
    auto rect = VideoContentRect(1024, 768, VideoSettings{});
    require(rect.left == 160 && rect.top == 0 && rect.right == 1120 && rect.bottom == 720,
        "Desktop touch layout did not exclude letterboxing");
    auto allowed = std::make_shared<std::atomic_bool>(false);
    std::vector<INPUT> injected;
    RemoteInput input({0,0,1920,1080}, allowed, [&](std::span<INPUT> events) {
        injected.insert(injected.end(), events.begin(), events.end());
        return static_cast<UINT>(events.size());
    });
    auto resume = [&] { input.Receive(R"({"type":"resume"})"); };
    auto drag = [&] { input.Receive(R"({"type":"drag","down":true,"x":5000,"y":5000})"); };
    resume();
    drag();
    require(injected.empty(), "Unapproved control sent input");
    *allowed = true;
    drag();
    require(injected.empty(), "Input was accepted without a foreground lease");
    resume();
    input.Receive(R"({"type":"click","button":"left","x":18446744073709551615,"y":5000})");
    require(injected.empty(), "Out-of-range coordinate reached SendInput");
    drag();
    require(injected.size() == 2 && (injected[0].mi.dwFlags & MOUSEEVENTF_ABSOLUTE) &&
        injected[1].mi.dwFlags == MOUSEEVENTF_LEFTDOWN, "Drag did not position before pressing");
    input.Expire(RemoteInput::Clock::now() + std::chrono::seconds(2));
    require(injected.size() == 3 && injected.back().mi.dwFlags == MOUSEEVENTF_LEFTUP,
        "Foreground lease timeout did not release the mouse");
    resume();
    drag();
    *allowed = false;
    input.Expire(RemoteInput::Clock::now());
    require(injected.size() == 6 && injected.back().mi.dwFlags == MOUSEEVENTF_LEFTUP,
        "Permission revocation did not release the mouse");
    input.Close();
    require(injected.size() == 6, "Closing twice produced an extra mouse release");
}
static void checkRemoteTls() {
    auto identity = std::make_shared<RemoteIdentity>(RemoteIdentity::Generate());
    auto code = identity->PairingCode();
    require(code.starts_with("LSC1.") && code.size() == 92, "Invalid pairing code format");
    require(!identity->Authorizes(R"({"type":"remote_auth","version":1,"token":"wrong"})"),
        "Invalid token was accepted");
    auto auth = nlohmann::json{{"type", "remote_auth"}, {"version", 1}, {"token", identity->token}}.dump();
    require(!RemoteIdentity::Generate().Authorizes(auth), "Rotating pairing did not invalidate the old token");

    rtc::WebSocketServerConfiguration config;
    config.port = 0;
    config.bindAddress = "::1";
    config.enableTls = true;
    config.certificatePemFile = identity->certificate;
    config.keyPemFile = identity->privateKey;
    rtc::WebSocketServer server(config);
    std::shared_ptr<rtc::WebSocket> incoming;
    server.onClient([&incoming, identity](std::shared_ptr<rtc::WebSocket> peer) {
        incoming = peer;
        peer->onMessage([identity, weak = std::weak_ptr(peer)](rtc::message_variant message) {
            if (auto current = weak.lock()) {
                if (auto text = std::get_if<std::string>(&message); text && current->path() == "/remote")
                    current->send(std::string(identity->Authorizes(*text) ? "authorized" : "rejected"));
            }
        });
    });
    rtc::WebSocketConfiguration clientConfig;
    // Local loopback exercises the server TLS socket. Android verifies the out-of-band certificate pin.
    clientConfig.disableTlsVerification = true;
    std::atomic_bool rejected{false}, authorized{false};
    auto client = std::make_shared<rtc::WebSocket>(clientConfig);
    client->onOpen([weak = std::weak_ptr(client), auth] {
        if (auto active = weak.lock()) {
            active->send(std::string(R"({"type":"remote_auth","version":1,"token":"wrong"})"));
            active->send(auth);
        }
    });
    client->onMessage([&](rtc::message_variant message) {
        if (auto text = std::get_if<std::string>(&message)) {
            if (*text == "rejected") rejected = true;
            if (*text == "authorized") authorized = true;
        }
    });
    client->open("wss://[::1]:" + std::to_string(server.port()) + "/remote");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!authorized && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    client->forceClose();
    server.stop();
    if (incoming) incoming->forceClose();
    require(rejected && authorized, "IPv6 WSS pairing exchange failed");
}

int main() {
    try {
        checkRemoteInput();
        checkRemoteTls();
        std::string session="14a92ef7-721c-4670-b65e-cb6791be36dd";
        nlohmann::json signal={{"protocolVersion",2},{"type","ping"},{"requestId",session},{"timestamp",0},{"payload",{{"sessionId",session}}}};
        ValidateSignal(signal,session);
        for(int test=0;test<4;++test) {
            auto bad=signal;
            if(test==0)bad["protocolVersion"]=1;
            if(test==1)bad["requestId"]="invalid";
            if(test==2)bad["timestamp"]=-1;
            if(test==3)bad["payload"]["sessionId"]="wrong";
            bool rejected=false;try{ValidateSignal(bad,session);}catch(...){rejected=true;}
            require(rejected,"Invalid signal envelope was accepted");
        }
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));check(MFStartup(MF_VERSION));
        require(H264AnnexB({0,0,0,2,0x65,1})==std::vector<uint8_t>({0,0,0,1,0x65,1}),"AVCC normalization failed");
        std::vector<AccessUnit> units;std::atomic_bool stop{false};
        {
            H264Encoder encoder(nullptr,VideoSettings{},true);
            for(int i=0;i<60;++i) {
                if(i==0 || i==30) encoder.ForceKeyFrame();
                std::vector<uint8_t> frame(1280*720*3/2,128);
                for(int y=0;y<720;++y) for(int x=0;x<1280;++x) frame[y*1280+x]=uint8_t(16+((x+i*8)%220));
                encoder.EncodeNV12(frame,i*10000000LL/30,stop,[&](AccessUnit unit){units.push_back(std::move(unit));});
            }
            encoder.Drain([&](AccessUnit unit){units.push_back(std::move(unit));});
        }
        require(units.size()>=55,"Too few encoded frames");
        for(size_t i=1;i<units.size();++i) require(units[i].time100ns>units[i-1].time100ns,"Nonmonotonic H264 timestamps");
        int keys=0;for(auto const& unit:units)if(unit.keyframe)++keys;
        require(keys>=2,"Keyframe request was ignored");
        int decoded=decode(units);require(decoded>=55,"H264 bitstream did not decode");
        {
            ComPtr<ID3D11Device> device;
            check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr));
            H264Encoder encoder(device.Get(),VideoSettings{},true);
            int converted=0;
            for(int i=0;i<8;++i) {
                std::vector<uint8_t> bgra(1280*720*4);
                for(size_t p=0;p<bgra.size();p+=4){bgra[p]=uint8_t(i*20);bgra[p+1]=100;bgra[p+2]=200;bgra[p+3]=255;}
                D3D11_TEXTURE2D_DESC desc{};desc.Width=1280;desc.Height=720;desc.MipLevels=desc.ArraySize=1;
                desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
                D3D11_SUBRESOURCE_DATA data{};data.pSysMem=bgra.data();data.SysMemPitch=1280*4;
                VideoFrame frame;check(device->CreateTexture2D(&desc,&data,&frame.texture));frame.time100ns=i*10000000LL/30;
                encoder.Encode(frame,i==0,stop,[&](AccessUnit){++converted;});
            }
            encoder.Drain([&](AccessUnit){++converted;});
            require(converted>=6,"BGRA to NV12 fallback did not encode");
            std::cout<<"BGRA conversion frames: "<<converted<<std::endl;
        }
        std::cout<<"Software H264 encoded/decoded: "<<units.size()<<"/"<<decoded<<std::endl;
        rtcLoopback(units);MFShutdown();CoUninitialize();return 0;
    } catch(std::exception const& error) {std::cerr<<error.what()<<std::endl;return 1;}
}
