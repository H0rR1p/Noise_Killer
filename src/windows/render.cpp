#include "noisekiller/render.hpp"
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <wrl/client.h>
#include <cstring>
#include <thread>

namespace nk {
namespace win {
using Microsoft::WRL::ComPtr;
namespace {
constexpr HRESULT FifoOverflow = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x401);
constexpr HRESULT InvalidPcm = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x402);
constexpr HRESULT DriftFailure = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x403);
constexpr HRESULT UnsupportedRoute = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x404);
std::string explain_render(HRESULT hr) {
    if (hr == FifoOverflow)
        return "Virtual-cable render FIFO overflow; stopped instead of accumulating/replaying audio";
    if (hr == InvalidPcm)
        return "Invalid render PCM/resampler state; output stopped without raw bypass";
    if (hr == DriftFailure)
        return "Render clock/underrun is outside bounded asynchronous correction; explicitly restart after "
               "fixing the route";
    if (hr == UnsupportedRoute)
        return "Select an active supported VB-CABLE playback endpoint (CABLE Input), not a recording "
               "endpoint or ordinary speaker";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
        return "Selected cable shared format/buffering is unsupported; no default-speaker fallback";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED)
        return "Selected virtual-cable playback device was removed/suspended; session stopped";
    return "Virtual-cable WASAPI failure (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")";
}
struct RenderFormat {
    unsigned rate = 0, channels = 0, bits = 0, valid_bits = 0, alignment = 0;
    bool floating = false;
    bool set(const WAVEFORMATEX *f) {
        if (!f || !f->nChannels || f->nChannels > 2 || f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 192000)
            return false;
        WORD tag = f->wFormatTag;
        valid_bits = f->wBitsPerSample;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (f->cbSize < 22)
                return false;
            const auto *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
            valid_bits = e->Samples.wValidBitsPerSample;
            if (e->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (e->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
                tag = WAVE_FORMAT_PCM;
            else
                return false;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        bits = f->wBitsPerSample;
        if (floating ? bits != 32 : (tag != WAVE_FORMAT_PCM || (bits != 16 && bits != 24 && bits != 32)))
            return false;
        if (!valid_bits)
            valid_bits = bits;
        if (valid_bits > bits || valid_bits < 16)
            return false;
        rate = f->nSamplesPerSec;
        channels = f->nChannels;
        alignment = f->nBlockAlign;
        return alignment == channels * (bits / 8);
    }
    void encode(unsigned char *output, float value) const noexcept {
        value = std::clamp(value, -1.0f, 1.0f);
        for (unsigned c = 0; c < channels; ++c) {
            auto *at = output + c * (bits / 8);
            if (floating)
                std::memcpy(at, &value, 4);
            else {
                const double scale = std::ldexp(1.0, static_cast<int>(valid_bits) - 1);
                const auto signed_value =
                    static_cast<std::int64_t>(std::llround(value * (value < 0 ? scale : scale - 1)));
                const auto container = static_cast<std::uint32_t>(signed_value) << (bits - valid_bits);
                for (unsigned byte = 0; byte < bits / 8; ++byte)
                    at[byte] = static_cast<unsigned char>(container >> (8 * byte));
            }
        }
    }
};
bool supported_device(IMMDevice *device) {
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow = eAll;
    DWORD state = 0;
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&endpoint))) || FAILED(endpoint->GetDataFlow(&flow)) ||
        flow != eRender || FAILED(device->GetState(&state)) || !(state & DEVICE_STATE_ACTIVE) ||
        FAILED(device->OpenPropertyStore(STGM_READ, &properties)))
        return false;
    PROPVARIANT name, manufacturer;
    PropVariantInit(&name);
    PropVariantInit(&manufacturer);
    properties->GetValue(PKEY_Device_FriendlyName, &name);
    properties->GetValue(PKEY_Device_Manufacturer, &manufacturer);
    const bool supported =
        name.vt == VT_LPWSTR &&
        known_cable_render(name.pwszVal, manufacturer.vt == VT_LPWSTR ? manufacturer.pwszVal : L"");
    PropVariantClear(&name);
    PropVariantClear(&manufacturer);
    return supported;
}
} // namespace
struct WasapiRenderAdapter::State {
    RenderSamplePump pump;
    RenderDriftController drift;
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr),
           ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    std::wstring device_id;
    std::uint64_t epoch = 0;
    std::atomic<bool> stopping{true}, running{false};
    std::atomic<HRESULT> fault{S_OK};
    std::atomic<unsigned> rate{0}, channels{0}, period{0}, buffer{0}, padding{0}, queued{0}, high_water{0};
    std::atomic<std::uint64_t> underflows{0}, underflow_frames{0}, frames_rendered{0}, warmup{0};
    std::atomic<double> ppm{0}, compute{0}, maximum_compute{0};
    std::atomic<bool> client3{false};
    std::atomic<HRESULT> fallback{S_OK};
    ~State() {
        stop();
        if (stop_event)
            CloseHandle(stop_event);
        if (ready_event)
            CloseHandle(ready_event);
    }
    void trip(HRESULT hr) noexcept {
        HRESULT previous = S_OK;
        fault.compare_exchange_strong(previous, hr);
        stopping.store(true, std::memory_order_release);
        SetEvent(stop_event);
    }
    void stop() noexcept {
        stopping.store(true, std::memory_order_release);
        if (stop_event)
            SetEvent(stop_event);
        if (thread.joinable())
            thread.join();
        running = false;
        pump.clear();
        drift.reset();
        epoch = 0;
        queued = 0;
        padding = 0;
    }
    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> renderer;
        WAVEFORMATEX *mix = nullptr;
        HANDLE sample_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE mmcss = nullptr;
        DWORD task = 0;
        RenderFormat format;
        const auto cleanup = [&]() {
            if (client) {
                client->Stop();
                client->Reset();
            }
            running = false;
            if (mmcss)
                AvRevertMmThreadCharacteristics(mmcss);
            if (mix)
                CoTaskMemFree(mix);
            if (sample_event)
                CloseHandle(sample_event);
            renderer.Reset();
            client.Reset();
            device.Reset();
            enumerator.Reset();
            if (SUCCEEDED(com))
                CoUninitialize();
        };
        HRESULT hr = com;
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr))
            hr = enumerator->GetDevice(device_id.c_str(), &device);
        if (SUCCEEDED(hr) && !supported_device(device.Get()))
            hr = UnsupportedRoute;
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void **>(client.GetAddressOf()));
        if (SUCCEEDED(hr))
            hr = client->GetMixFormat(&mix);
        if (SUCCEEDED(hr) && !format.set(mix))
            hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (!sample_event && SUCCEEDED(hr))
            hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) {
            bool initialized = false;
            ComPtr<IAudioClient3> modern;
            if (SUCCEEDED(client.As(&modern))) {
                UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
                if (SUCCEEDED(
                        modern->GetSharedModeEnginePeriod(mix, &def, &fundamental, &minimum, &maximum)) &&
                    fundamental && minimum <= maximum) {
                    UINT32 chosen = std::max(minimum, ((format.rate / 100 + fundamental - 1) / fundamental) *
                                                          fundamental);
                    chosen = std::min(chosen, maximum);
                    hr = modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, chosen, mix,
                                                             nullptr);
                    initialized = SUCCEEDED(hr);
                    if (initialized) {
                        period = chosen;
                        client3 = true;
                    } else
                        fallback = hr;
                }
            }
            if (!initialized) {
                if (fallback.load() == S_OK)
                    fallback = E_NOINTERFACE;
                modern.Reset();
                client.Reset();
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                      reinterpret_cast<void **>(client.GetAddressOf()));
                if (SUCCEEDED(hr))
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                            200000, 0, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    REFERENCE_TIME def = 0, minimum = 0;
                    hr = client->GetDevicePeriod(&def, &minimum);
                    if (SUCCEEDED(hr))
                        period = static_cast<unsigned>(def * format.rate / 10000000);
                }
            }
        }
        UINT32 size = 0;
        if (SUCCEEDED(hr))
            hr = client->GetBufferSize(&size);
        // Explicit implementation caps: never silently expand latency to make
        // an unsupported device usable. Not a measured end-to-end guarantee.
        if (SUCCEEDED(hr) && (!size || size > MaximumRenderFrames || size > format.rate * 40 / 1000 ||
                              !period.load() || period.load() > format.rate * 20 / 1000))
            hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (SUCCEEDED(hr))
            hr = client->SetEventHandle(sample_event);
        if (SUCCEEDED(hr))
            hr = client->GetService(IID_PPV_ARGS(&renderer));
        if (SUCCEEDED(hr) && !pump.configure(format.rate, epoch))
            hr = InvalidPcm;
        if (SUCCEEDED(hr)) {
            BYTE *initial = nullptr;
            hr = renderer->GetBuffer(size, &initial);
            if (SUCCEEDED(hr))
                hr = renderer->ReleaseBuffer(size, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (SUCCEEDED(hr) && !stopping.load())
            hr = client->Start();
        if (FAILED(hr)) {
            trip(hr);
            SetEvent(ready_event);
            cleanup();
            return;
        }
        rate = format.rate;
        channels = format.channels;
        buffer = size;
        running = true;
        drift.reset();
        SetEvent(ready_event);
        mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        HANDLE waits[2]{stop_event, sample_event};
        std::array<float, MaximumRenderFrames> mono{};
        bool primed = false;
        unsigned consecutive_underflows = 0;
        LARGE_INTEGER frequency{}, previous{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);
        while (!stopping.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2000);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait != WAIT_OBJECT_0 + 1) {
                trip(HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
                break;
            }
            QueryPerformanceCounter(&now);
            const double dt = double(now.QuadPart - previous.QuadPart) / frequency.QuadPart;
            previous = now;
            UINT32 filled = 0;
            hr = client->GetCurrentPadding(&filled);
            if (FAILED(hr)) {
                trip(hr);
                break;
            }
            if (filled > size) {
                trip(InvalidPcm);
                break;
            }
            padding = filled;
            const UINT32 available = size - filled;
            if (!available)
                continue;
            BYTE *output = nullptr;
            hr = renderer->GetBuffer(available, &output);
            if (FAILED(hr)) {
                trip(hr);
                break;
            }
            bool silent = false;
            HRESULT frame_fault = S_OK;
            const unsigned occupancy = pump.queued_input_samples();
            queued = occupancy;
            unsigned high = high_water.load();
            while (occupancy > high && !high_water.compare_exchange_weak(high, occupancy)) {
            }
            if (!primed && occupancy < 2 * Hop) {
                silent = true;
                warmup += available;
            } else {
                primed = true;
                const double correction = drift.update(occupancy, dt);
                ppm = correction;
                if (drift.faulted())
                    frame_fault = DriftFailure;
                else {
                    const auto result = pump.fill(mono.data(), available, correction);
                    if (result.fault)
                        frame_fault = InvalidPcm;
                    else {
                        if (result.underflow_frames) {
                            ++underflows;
                            underflow_frames += result.underflow_frames;
                            ++consecutive_underflows;
                        } else
                            consecutive_underflows = 0;
                        if (consecutive_underflows >= 10)
                            frame_fault = DriftFailure;
                        if (!output)
                            frame_fault = InvalidPcm;
                        if (SUCCEEDED(frame_fault))
                            for (UINT32 frame = 0; frame < available; ++frame)
                                format.encode(output + std::size_t(frame) * format.alignment, mono[frame]);
                    }
                }
            }
            if (stopping.load() || FAILED(frame_fault))
                silent = true;
            hr = renderer->ReleaseBuffer(available, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
            frames_rendered += available;
            queued = pump.queued_input_samples();
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            const double us = double(end.QuadPart - now.QuadPart) * 1000000.0 / frequency.QuadPart;
            compute = us;
            maximum_compute = std::max(maximum_compute.load(), us);
            if (FAILED(frame_fault)) {
                trip(frame_fault);
                break;
            }
            if (FAILED(hr)) {
                trip(hr);
                break;
            }
        }
        mono.fill(0);
        cleanup();
    }
};
WasapiRenderAdapter::WasapiRenderAdapter() : state_(std::make_unique<State>()) {}
WasapiRenderAdapter::~WasapiRenderAdapter() = default;
bool WasapiRenderAdapter::start(const std::wstring &id, std::uint64_t epoch, std::string &error) {
    stop();
    error.clear();
    if (id.empty() || !epoch) {
        error = "Explicit virtual-cable playback selection and nonzero epoch are required";
        return false;
    }
    auto &s = *state_;
    s.device_id = id;
    s.epoch = epoch;
    s.fault = S_OK;
    s.stopping = false;
    s.rate = 0;
    s.channels = 0;
    s.period = 0;
    s.buffer = 0;
    s.padding = 0;
    s.queued = 0;
    s.high_water = 0;
    s.underflows = 0;
    s.underflow_frames = 0;
    s.frames_rendered = 0;
    s.warmup = 0;
    s.ppm = 0;
    s.compute = 0;
    s.maximum_compute = 0;
    s.client3 = false;
    s.fallback = S_OK;
    if (!s.stop_event || !s.ready_event) {
        error = "Unable to allocate render events";
        return false;
    }
    ResetEvent(s.stop_event);
    ResetEvent(s.ready_event);
    try {
        s.thread = std::thread([&s] {
            try {
                s.run();
            } catch (...) {
                s.trip(E_FAIL);
                SetEvent(s.ready_event);
                s.running = false;
            }
        });
    } catch (...) {
        error = "Unable to create dedicated render thread";
        s.stopping = true;
        return false;
    }
    if (WaitForSingleObject(s.ready_event, 5000) != WAIT_OBJECT_0 || FAILED(s.fault.load()) ||
        !s.running.load()) {
        error = FAILED(s.fault.load()) ? explain_render(s.fault.load())
                                       : "Render initialization timed out or stopped";
        stop();
        return false;
    }
    return true;
}
bool WasapiRenderAdapter::enqueue(const float *samples, std::size_t count, std::uint64_t epoch) noexcept {
    auto &s = *state_;
    if (s.stopping.load(std::memory_order_acquire) || !s.running.load(std::memory_order_acquire) ||
        epoch != s.epoch)
        return false;
    const auto result = s.pump.enqueue(samples, count, epoch);
    if (result == RenderQueueResult::Ok)
        return true;
    s.trip(result == RenderQueueResult::Full ? FifoOverflow : InvalidPcm);
    return false;
}
void WasapiRenderAdapter::stop() noexcept { state_->stop(); }
bool WasapiRenderAdapter::healthy() const noexcept {
    return state_->running.load(std::memory_order_acquire) &&
           !state_->stopping.load(std::memory_order_acquire) && SUCCEEDED(state_->fault.load());
}
RenderSnapshot WasapiRenderAdapter::snapshot() const {
    const auto &s = *state_;
    RenderSnapshot result;
    result.running = s.running.load();
    result.fault = FAILED(s.fault.load());
    result.sample_rate = s.rate.load();
    result.channels = s.channels.load();
    result.period_frames = s.period.load();
    result.buffer_frames = s.buffer.load();
    result.padding_frames = s.padding.load();
    result.queued_input_samples = s.queued.load();
    result.queue_high_water = s.high_water.load();
    result.underflow_events = s.underflows.load();
    result.underflow_frames = s.underflow_frames.load();
    result.rendered_frames = s.frames_rendered.load();
    result.warmup_zero_frames = s.warmup.load();
    result.drift_ppm = s.ppm.load();
    result.compute_us = s.compute.load();
    result.maximum_compute_us = s.maximum_compute.load();
    result.client3 = s.client3.load();
    result.shared_fallback_hresult = s.fallback.load();
    if (result.fault)
        result.error = explain_render(s.fault.load());
    return result;
}
} // namespace win
} // namespace nk
#endif
