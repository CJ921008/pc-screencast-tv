#define NOMINMAX
#include <windows.h>
#include "MediaPipeline.h"
#include "logging/FileLogger.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <cstring>
#include <sstream>
#pragma comment(lib, "D3d11.lib")
#pragma comment(lib, "Dxgi.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Mf.lib")

namespace LANScreenCast::casting {
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr, char const* action) {
    if (FAILED(hr)) {
        std::ostringstream text; text << action << " HRESULT=0x" << std::hex << static_cast<unsigned>(hr);
        throw std::runtime_error(text.str());
    }
}

std::vector<uint8_t> H264AnnexB(std::vector<uint8_t> const& input) {
    if (input.size() >= 3 && input[0] == 0 && input[1] == 0 &&
        (input[2] == 1 || (input.size() >= 4 && input[2] == 0 && input[3] == 1))) return input;
    std::vector<uint8_t> result;
    if (input.size() >= 7 && input[0] == 1) {
        size_t position = 6; unsigned count = input[5] & 31;
        for (unsigned group = 0; group < 2; ++group) {
            for (unsigned i = 0; i < count; ++i) {
                if (position + 2 > input.size()) throw std::runtime_error("Invalid AVC parameter sets");
                size_t length = (input[position] << 8) | input[position + 1]; position += 2;
                if (!length || position + length > input.size()) throw std::runtime_error("Invalid AVC parameter set");
                result.insert(result.end(), {0,0,0,1});
                result.insert(result.end(), input.begin() + position, input.begin() + position + length);
                position += length;
            }
            if (group == 0) { if (position >= input.size()) throw std::runtime_error("Missing AVC PPS"); count = input[position++]; }
        }
        return result;
    }
    for (size_t p = 0; p < input.size();) {
        if (p + 4 > input.size()) throw std::runtime_error("Invalid H264 access unit");
        size_t length = (size_t(input[p]) << 24) | (size_t(input[p+1]) << 16) | (size_t(input[p+2]) << 8) | input[p+3];
        p += 4;
        if (!length || p + length > input.size()) throw std::runtime_error("Invalid H264 NAL length");
        result.insert(result.end(), {0,0,0,1});
        result.insert(result.end(), input.begin() + p, input.begin() + p + length); p += length;
    }
    return result;
}

struct DesktopSource::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplicate;
    ComPtr<ID3D11Texture2D> latest;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    Impl() {
        ComPtr<IDXGIFactory1> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC d{}; Check(output->GetDesc(&d), "Display description");
                if (!d.AttachedToDesktop || d.DesktopCoordinates.left > 0 || d.DesktopCoordinates.top > 0 ||
                    d.DesktopCoordinates.right <= 0 || d.DesktopCoordinates.bottom <= 0) continue;
                device.Reset(); context.Reset();
                Check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                    D3D11_SDK_VERSION, &device, nullptr, &context), "Create capture device");
                ComPtr<ID3D11Multithread> protection; Check(context.As(&protection), "D3D thread protection");
                protection->SetMultithreadProtected(TRUE);
                ComPtr<IDXGIOutput1> output1; Check(output.As(&output1), "Capture output");
                Check(output1->DuplicateOutput(device.Get(), &duplicate), "Duplicate primary display");
                return;
            }
        }
        throw std::runtime_error("Primary display unavailable");
    }
    VideoFrame next(int fps) {
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
        auto hr = duplicate->AcquireNextFrame(1000 / fps, &info, &resource);
        if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            Check(hr, "Display changed; restart casting");
            struct Release { IDXGIOutputDuplication* p; ~Release() { p->ReleaseFrame(); } } release{duplicate.Get()};
            ComPtr<ID3D11Texture2D> surface; Check(resource.As(&surface), "Desktop texture");
            D3D11_TEXTURE2D_DESC d{}; surface->GetDesc(&d);
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
            d.CPUAccessFlags = 0; d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> copy; Check(device->CreateTexture2D(&d, nullptr, &copy), "Copy capture texture");
            context->CopyResource(copy.Get(), surface.Get()); latest = copy;
        }
        VideoFrame frame{latest};
        frame.time100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - origin).count() / 100;
        return frame;
    }
};
DesktopSource::DesktopSource() : impl(std::make_unique<Impl>()) {}
DesktopSource::~DesktopSource() = default;
ID3D11Device* DesktopSource::Device() const { return impl->device.Get(); }
VideoFrame DesktopSource::Next(int fps) { return impl->next(fps); }

struct H264Encoder::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> staging;
    UINT inputWidth = 0, inputHeight = 0;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ICodecAPI> codec;
    VideoSettings settings;
    std::wstring name;
    bool hardware = false, gpuInput = false, asynchronous = false;
    int inputCredits = 0;
    std::vector<uint8_t> parameters;

    void property(GUID key, ULONG value, bool boolean = false) {
        if (!codec) return;
        VARIANT v{}; v.vt = boolean ? VT_BOOL : VT_UI4;
        if (boolean) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal = value;
        codec->SetValue(&key, &v);
    }
    void setup(IMFActivate* activation, bool hw) {
        transform.Reset(); events.Reset(); manager.Reset(); codec.Reset();
        Check(activation->ActivateObject(IID_PPV_ARGS(&transform)), "Activate H264 encoder");
        UINT32 length = 0; wchar_t* label = nullptr;
        if (SUCCEEDED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &label, &length))) {
            name.assign(label, length); CoTaskMemFree(label);
        }
        hardware = hw;
        ComPtr<IMFAttributes> attrs; Check(transform->GetAttributes(&attrs), "Encoder attributes");
        UINT32 value = 0; attrs->GetUINT32(MF_TRANSFORM_ASYNC, &value); asynchronous = value != 0;
        if (asynchronous) {
            Check(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "Unlock hardware encoder");
            Check(transform.As(&events), "Encoder events");
        }
        value = 0; attrs->GetUINT32(MF_SA_D3D11_AWARE, &value); gpuInput = hw && device && value;
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (gpuInput) {
            UINT token = 0; Check(MFCreateDXGIDeviceManager(&token, &manager), "Encoder D3D manager");
            Check(manager->ResetDevice(device.Get(), token), "Encoder D3D device");
            Check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())), "Set encoder GPU");
        }
        transform.As(&codec);
        property(CODECAPI_AVLowLatencyMode, TRUE, true);
        property(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        property(CODECAPI_AVEncMPVGOPSize, settings.fps * 2);
        property(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        property(CODECAPI_AVEncCommonMeanBitRate, settings.bitrate);
        ComPtr<IMFMediaType> output; Check(MFCreateMediaType(&output), "Encoder output type");
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, settings.bitrate);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        output->SetUINT32(MF_MT_MPEG2_LEVEL, settings.width == 1920 ? 40 : 31);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height);
        MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, settings.fps, 1);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        Check(transform->SetOutputType(0, output.Get(), 0), "Set H264 output");
        ComPtr<IMFMediaType> input; Check(MFCreateMediaType(&input), "Encoder input type");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, settings.fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        Check(transform->SetInputType(0, input.Get(), 0), "Set NV12 input");
        Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin encoder");
        Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start encoder");
    }
    bool select(bool hw) {
        IMFActivate** list = nullptr; UINT32 count = 0;
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
        HRESULT hr;
        if (hw) {
            ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs, 1), "Enumeration attributes");
            ComPtr<IDXGIDevice> dxgi; Check(device.As(&dxgi), "GPU adapter");
            ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter), "GPU adapter");
            DXGI_ADAPTER_DESC description{}; Check(adapter->GetDesc(&description), "Adapter LUID");
            attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<BYTE*>(&description.AdapterLuid), sizeof(LUID));
            using Enum2 = HRESULT (WINAPI*)(GUID,UINT32,const MFT_REGISTER_TYPE_INFO*,const MFT_REGISTER_TYPE_INFO*,IMFAttributes*,IMFActivate***,UINT32*);
            auto fn = reinterpret_cast<Enum2>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2"));
            hr = fn ? fn(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                &in, &out, attrs.Get(), &list, &count) : E_NOTIMPL;
        } else hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &in, &out, &list, &count);
        bool selected = false;
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count && !selected; ++i) {
                try { setup(list[i], hw); selected = true; }
                catch (std::exception const& e) {
                    OutputDebugStringA(e.what());
                    shutdown(); list[i]->ShutdownObject();
                }
            }
        }
        for (UINT32 i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree(list);
        return selected;
    }
    Impl(ID3D11Device* d, VideoSettings s, bool force) : device(d), settings(s) {
        if (device) { device->GetImmediateContext(&context); Check(device.As(&videoDevice), "Video device"); Check(context.As(&videoContext), "Video context"); }
        if (device && !force && select(true)) return;
        settings.width = 1280; settings.height = 720; settings.bitrate = 4000000;
        if (!select(false)) throw std::runtime_error("No usable H264 encoder");
    }
    void shutdown() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            ComPtr<IMFShutdown> closing; if (SUCCEEDED(transform.As(&closing))) closing->Shutdown();
        }
        transform.Reset(); events.Reset(); inputCredits = 0;
    }
    ~Impl() { shutdown(); }
    ComPtr<IMFSample> sample(ComPtr<IMFMediaBuffer> const& buffer, int64_t time) {
        ComPtr<IMFSample> result; Check(MFCreateSample(&result), "Create NV12 sample");
        Check(result->AddBuffer(buffer.Get()), "Add sample buffer");
        result->SetSampleTime(time); result->SetSampleDuration(10000000 / settings.fps);
        return result;
    }
    ComPtr<IMFSample> memorySample(std::vector<uint8_t> const& bytes, int64_t time) {
        ComPtr<IMFMediaBuffer> buffer; Check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer), "NV12 buffer");
        BYTE* data = nullptr; Check(buffer->Lock(&data, nullptr, nullptr), "Lock NV12");
        memcpy(data, bytes.data(), bytes.size()); buffer->Unlock(); buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
        return sample(buffer, time);
    }
    ComPtr<IMFSample> convert(VideoFrame const& frame) {
        D3D11_TEXTURE2D_DESC source{}; frame.texture->GetDesc(&source);
        if (!processor || inputWidth != source.Width || inputHeight != source.Height) {
            inputWidth = source.Width; inputHeight = source.Height;
            enumerator.Reset(); processor.Reset(); staging.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = source.Width; content.InputHeight = source.Height;
            content.OutputWidth = settings.width; content.OutputHeight = settings.height;
            content.InputFrameRate = {static_cast<UINT>(settings.fps), 1}; content.OutputFrameRate = content.InputFrameRate;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            Check(videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator), "Video conversion enumerator");
            Check(videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor), "NV12 video processor");
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = settings.width; desc.Height = settings.height;
        desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> nv12; Check(device->CreateTexture2D(&desc, nullptr, &nv12), "NV12 GPU texture");
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> input;
        Check(videoDevice->CreateVideoProcessorInputView(frame.texture.Get(), enumerator.Get(), &iv, &input), "Processor input");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{}; ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        Check(videoDevice->CreateVideoProcessorOutputView(nv12.Get(), enumerator.Get(), &ov, &output), "Processor output");
        RECT src{0,0,static_cast<LONG>(source.Width),static_cast<LONG>(source.Height)};
        float scale = std::min(float(settings.width)/source.Width, float(settings.height)/source.Height);
        LONG width = static_cast<LONG>(source.Width * scale) & ~1, height = static_cast<LONG>(source.Height * scale) & ~1;
        RECT dst{(settings.width-width)/2,(settings.height-height)/2,(settings.width+width)/2,(settings.height+height)/2};
        RECT whole{0,0,settings.width,settings.height};
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &whole);
        D3D11_VIDEO_COLOR black{}; black.YCbCr.Y = 16.0f/255; black.YCbCr.Cb = black.YCbCr.Cr = .5f; black.YCbCr.A = 1;
        videoContext->VideoProcessorSetOutputBackgroundColor(processor.Get(), TRUE, &black);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{}; color.YCbCr_Matrix = 1; color.Nominal_Range = 2;
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &color);
        color.Nominal_Range = 1; videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &color);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input.Get();
        Check(videoContext->VideoProcessorBlt(processor.Get(), output.Get(), 0, 1, &stream), "BGRA to NV12");
        if (gpuInput) {
            ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE, &buffer), "GPU encoder buffer");
            return sample(buffer, frame.time100ns);
        }
        if (!staging) {
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(device->CreateTexture2D(&desc, nullptr, &staging), "Software encoder readback");
        }
        context->CopyResource(staging.Get(), nv12.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map NV12");
        std::vector<uint8_t> bytes(settings.width * settings.height * 3 / 2);
        auto sourceBytes = static_cast<BYTE*>(mapped.pData);
        for (int y = 0; y < settings.height * 3 / 2; ++y)
            memcpy(bytes.data() + y * settings.width, sourceBytes + y * mapped.RowPitch, settings.width);
        context->Unmap(staging.Get(), 0);
        return memorySample(bytes, frame.time100ns);
    }
    bool output(std::function<void(AccessUnit)> const& emit) {
        MFT_OUTPUT_STREAM_INFO info{}; Check(transform->GetOutputStreamInfo(0, &info), "Encoder output buffer info");
        ComPtr<IMFSample> allocated;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            ComPtr<IMFMediaBuffer> buffer; Check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, settings.width * settings.height * 2), &buffer), "Encoded buffer");
            Check(MFCreateSample(&allocated), "Encoded sample"); allocated->AddBuffer(buffer.Get());
        }
        MFT_OUTPUT_DATA_BUFFER data{}; data.pSample = allocated.Get(); DWORD status = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) data.pEvents->Release();
        ComPtr<IMFSample> encoded;
        if (data.pSample && data.pSample != allocated.Get()) encoded.Attach(data.pSample); else encoded = allocated;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type; Check(transform->GetOutputAvailableType(0, 0, &type), "Changed output type");
            Check(transform->SetOutputType(0, type.Get(), 0), "Changed encoder output"); return true;
        }
        Check(hr, "H264 encode output");
        if (!encoded) throw std::runtime_error("Encoder returned no sample");
        ComPtr<IMFMediaBuffer> buffer; Check(encoded->ConvertToContiguousBuffer(&buffer), "Encoded bytes");
        BYTE* ptr = nullptr; DWORD length = 0; Check(buffer->Lock(&ptr, nullptr, &length), "Read H264 bytes");
        std::vector<uint8_t> bytes(ptr, ptr + length); buffer->Unlock();
        int64_t time = 0; Check(encoded->GetSampleTime(&time), "H264 timestamp");
        UINT32 key = 0; encoded->GetUINT32(MFSampleExtension_CleanPoint, &key);
        ComPtr<IMFMediaType> type; Check(transform->GetOutputCurrentType(0, &type), "Current H264 type");
        UINT32 size = 0;
        if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            std::vector<uint8_t> header(size); type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr);
            parameters = H264AnnexB(header);
        }
        auto annex = H264AnnexB(bytes);
        if (key && !parameters.empty()) annex.insert(annex.begin(), parameters.begin(), parameters.end());
        emit({std::move(annex), time, key != 0});
        return true;
    }
    void poll(std::function<void(AccessUnit)> const& emit) {
        if (!asynchronous) { while (output(emit)) {} return; }
        ComPtr<IMFMediaEvent> event;
        while (events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event) == S_OK) {
            MediaEventType kind{}; event->GetType(&kind);
            HRESULT status = S_OK; event->GetStatus(&status); Check(status, "Hardware encoder event");
            if (kind == METransformNeedInput) ++inputCredits;
            if (kind == METransformHaveOutput) output(emit);
            event.Reset();
        }
    }
    void feed(IMFSample* s, std::atomic_bool const& stop, std::function<void(AccessUnit)> const& emit) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        if (asynchronous) {
            while (!inputCredits && !stop) {
                poll(emit);
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Hardware encoder input timeout");
                if (!inputCredits) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (stop) return;
            --inputCredits;
        }
        HRESULT hr = transform->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING && !asynchronous) { poll(emit); hr = transform->ProcessInput(0, s, 0); }
        Check(hr, "H264 encode input"); poll(emit);
    }
};
H264Encoder::H264Encoder(ID3D11Device* d, VideoSettings s, bool force) : impl(std::make_unique<Impl>(d,s,force)) {}
H264Encoder::~H264Encoder() = default;
VideoSettings H264Encoder::Settings() const { return impl->settings; }
bool H264Encoder::Hardware() const { return impl->hardware; }
std::wstring H264Encoder::Name() const { return impl->name; }
void H264Encoder::Encode(VideoFrame const& frame, bool key, std::atomic_bool const& stop, std::function<void(AccessUnit)> const& emit) {
    if (key) impl->property(CODECAPI_AVEncVideoForceKeyFrame, 1);
    impl->feed(impl->convert(frame).Get(), stop, emit);
}
void H264Encoder::EncodeNV12(std::vector<uint8_t> const& bytes, int64_t time, std::atomic_bool const& stop, std::function<void(AccessUnit)> const& emit) {
    impl->feed(impl->memorySample(bytes,time).Get(), stop, emit);
}
void H264Encoder::Drain(std::function<void(AccessUnit)> const& emit) {
    impl->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do { impl->poll(emit); if (!impl->asynchronous) break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while (std::chrono::steady_clock::now() < deadline);
}
}
