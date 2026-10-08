#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <rtc/rtc.hpp>
#include "../LANScreenCast/casting/MediaPipeline.h"
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
    std::cout<<"WebRTC frames received: "<<received<<std::endl;
}
int main() {
    try {
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
        std::cout<<"Software H264 encoded/decoded: "<<units.size()<<"/"<<decoded<<std::endl;
        rtcLoopback(units);MFShutdown();CoUninitialize();return 0;
    } catch(std::exception const& error) {std::cerr<<error.what()<<std::endl;return 1;}
}
