#include "noisekiller/io.hpp"
#include "noisekiller/render.hpp"
#include "noisekiller/windows.hpp"
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <avrt.h>
#include <cmath>
#include <cstring>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mutex>
#include <thread>
#include <wrl/client.h>

namespace nk {
namespace win {
using Microsoft::WRL::ComPtr;
namespace {
constexpr std::size_t QueueCapacity = 8, PacketCapacity = 65536;
constexpr HRESULT QueueOverflow = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x301);
constexpr HRESULT Discontinuity = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x302);
constexpr HRESULT BadSamples = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x303);
constexpr HRESULT EngineFault = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x304);
std::string explain(HRESULT hr) {
    if (hr == E_ACCESSDENIED)
        return "Microphone permission denied; allow desktop microphone access in Windows privacy settings";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED)
        return "Input device removed, suspended or invalidated; session is silent. Re-select and start "
               "explicitly";
    if (hr == QueueOverflow)
        return "Input queue overflow; session stopped and old audio discarded";
    if (hr == Discontinuity)
        return "Capture timeline discontinuity; session stopped and old audio discarded";
    if (hr == BadSamples)
        return "Invalid capture sample or resampler state; fail-silent stop";
    if (hr == EngineFault)
        return "MECAG produced an invalid numerical/contribution state; fail-silent stop, no raw bypass";
    return "Windows audio failure (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")";
}
struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};
struct Format {
    unsigned rate = 0, channels = 0, bits = 0, alignment = 0;
    bool floating = false;
    bool set(const WAVEFORMATEX *f) {
        if (!f || f->nChannels == 0 || f->nChannels > 8 || f->nSamplesPerSec < 8000 ||
            f->nSamplesPerSec > 192000)
            return false;
        WORD tag = f->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (f->cbSize < 22)
                return false;
            const auto *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
            if (e->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (e->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
                tag = WAVE_FORMAT_PCM;
            else
                return false;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        bits = f->wBitsPerSample;
        if (floating ? bits != 32
                     : (tag != WAVE_FORMAT_PCM || (bits != 8 && bits != 16 && bits != 24 && bits != 32)))
            return false;
        rate = f->nSamplesPerSec;
        channels = f->nChannels;
        alignment = f->nBlockAlign;
        return alignment == channels * (bits / 8);
    }
    float mono(const unsigned char *p) const noexcept {
        double sum = 0;
        for (unsigned c = 0; c < channels; ++c) {
            const auto *s = p + c * (bits / 8);
            float v = 0;
            if (floating)
                std::memcpy(&v, s, 4);
            else if (bits == 8)
                v = (int(s[0]) - 128) / 128.0f;
            else if (bits == 16) {
                std::int16_t n = 0;
                std::memcpy(&n, s, 2);
                v = n / 32768.0f;
            } else if (bits == 24) {
                std::int32_t n = s[0] | (std::int32_t(s[1]) << 8) | (std::int32_t(s[2]) << 16);
                if (n & 0x800000)
                    n -= 0x1000000;
                v = n / 8388608.0f;
            } else {
                std::int32_t n = 0;
                std::memcpy(&n, s, 4);
                v = static_cast<float>(n / 2147483648.0);
            }
            if (!std::isfinite(v))
                return v;
            sum += std::clamp(v, -1.0f, 1.0f);
        }
        return static_cast<float>(sum / channels);
    }
};
struct Packet {
    UINT32 frames = 0;
    DWORD flags = 0;
    std::uint64_t device_position = 0, capture_qpc = 0;
    std::array<unsigned char, PacketCapacity> bytes{};
};
std::uint64_t next_epoch() {
    static std::atomic<std::uint64_t> sequence{1};
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return (static_cast<std::uint64_t>(q.QuadPart) << 8) ^ GetCurrentProcessId() ^ sequence.fetch_add(1);
}
} // namespace
bool enumerate_devices(std::vector<Device> &devices, std::string &error, EDataFlow requested_flow) {
    devices.clear();
    error.clear();
    ComApartment com;
    if (!com.usable()) {
        error = explain(com.hr);
        return false;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        error = explain(hr);
        return false;
    }
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(requested_flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        error = explain(hr);
        return false;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        LPWSTR id = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        PROPVARIANT manufacturer;
        PropVariantInit(&manufacturer);
        if (SUCCEEDED(collection->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
            SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name))) {
            Device item;
            item.id = id;
            item.name = name.vt == VT_LPWSTR ? name.pwszVal : L"Unnamed capture device";
            properties->GetValue(PKEY_Device_Manufacturer, &manufacturer);
            item.manufacturer = manufacturer.vt == VT_LPWSTR ? manufacturer.pwszVal : L"";
            item.virtual_microphone =
                requested_flow == eCapture && virtual_capture_hint(item.name, item.manufacturer);
            item.virtual_cable =
                requested_flow == eRender && known_cable_render(item.name, item.manufacturer);
            ComPtr<IMMEndpoint> endpoint;
            EDataFlow actual = eAll;
            DWORD state = 0;
            if (SUCCEEDED(device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&actual)) &&
                actual == requested_flow && SUCCEEDED(device->GetState(&state)) &&
                (state & DEVICE_STATE_ACTIVE))
                devices.push_back(std::move(item));
        }
        if (id)
            CoTaskMemFree(id);
        PropVariantClear(&name);
        PropVariantClear(&manufacturer);
    }
    return true;
}
bool enumerate_capture_devices(std::vector<Device> &devices, std::string &error) {
    return enumerate_devices(devices, error, eCapture);
}
bool enumerate_render_devices(std::vector<Device> &devices, std::string &error) {
    return enumerate_devices(devices, error, eRender);
}
struct AudioSession::State {
    std::array<Packet, QueueCapacity> queue{};
    std::atomic<std::uint64_t> produced{0}, consumed{0};
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr),
           data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr),
           ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread capture_thread, worker_thread;
    std::atomic<bool> stopping{false};
    std::atomic<HRESULT> fault{S_OK};
    std::atomic<SessionState> mode{SessionState::Stopped};
    Format format;
    std::wstring device_id;
    std::wstring render_device_id;
    std::uint64_t epoch = 0;
    bool enrollment = false, bypass = false;
    std::vector<float> reference;
    std::atomic<unsigned> reference_count{0};
    unsigned reference_goal = 0;
    std::unique_ptr<Engine> engine;
    WasapiRenderAdapter renderer;
    std::atomic<std::uint64_t> captured{0}, hops{0}, dropped{0}, discontinuities{0}, misses{0};
    std::atomic<unsigned> high_water{0}, period{0};
    std::atomic<float> confidence{0}, residual{0};
    std::atomic<double> compute{0}, maximum_compute{0}, engine_compute{0};
    mutable std::mutex error_mutex;
    std::string error;
    ~State() {
        stop();
        if (stop_event)
            CloseHandle(stop_event);
        if (data_event)
            CloseHandle(data_event);
        if (ready_event)
            CloseHandle(ready_event);
    }
    void set_error(std::string value) {
        std::lock_guard<std::mutex> lock(error_mutex);
        error = std::move(value);
    }
    // This callback-path operation only touches bounded scalar atomics/events.
    void trip(HRESULT hr) noexcept {
        HRESULT previous = S_OK;
        fault.compare_exchange_strong(previous, hr);
        stopping.store(true, std::memory_order_release);
        SetEvent(stop_event);
        SetEvent(data_event);
    }
    void stop() noexcept {
        stopping.store(true, std::memory_order_release);
        if (stop_event)
            SetEvent(stop_event);
        if (data_event)
            SetEvent(data_event);
        if (capture_thread.joinable())
            capture_thread.join();
        if (worker_thread.joinable())
            worker_thread.join();
        renderer.stop();
        if (engine) {
            engine->reset(next_epoch());
            engine.reset();
        }
        for (auto &packet : queue)
            SecureZeroMemory(packet.bytes.data(), packet.bytes.size());
        produced = 0;
        consumed = 0;
        if (!reference.empty())
            SecureZeroMemory(reference.data(), reference.size() * sizeof(float));
        reference.clear();
        reference_count = 0;
        mode.store(SessionState::Stopped);
        epoch = next_epoch();
    }
    void capture() {
        ComApartment com;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture_client;
        WAVEFORMATEX *mix = nullptr;
        HANDLE sample_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE mmcss = nullptr;
        DWORD task = 0;
        auto cleanup = [&]() {
            if (client)
                client->Stop();
            if (mmcss)
                AvRevertMmThreadCharacteristics(mmcss);
            if (mix)
                CoTaskMemFree(mix);
            if (sample_event)
                CloseHandle(sample_event);
        };
        HRESULT hr = com.hr;
        if (com.usable())
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr))
            hr = enumerator->GetDevice(device_id.c_str(), &device);
        if (SUCCEEDED(hr)) {
            ComPtr<IMMEndpoint> endpoint;
            EDataFlow flow = eAll;
            DWORD device_state = 0;
            ComPtr<IPropertyStore> properties;
            if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&flow)) || flow != eCapture ||
                FAILED(device->GetState(&device_state)) || !(device_state & DEVICE_STATE_ACTIVE) ||
                FAILED(device->OpenPropertyStore(STGM_READ, &properties)))
                hr = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            else {
                PROPVARIANT name, manufacturer;
                PropVariantInit(&name);
                PropVariantInit(&manufacturer);
                properties->GetValue(PKEY_Device_FriendlyName, &name);
                properties->GetValue(PKEY_Device_Manufacturer, &manufacturer);
                if (name.vt != VT_LPWSTR ||
                    virtual_capture_hint(name.pwszVal,
                                         manufacturer.vt == VT_LPWSTR ? manufacturer.pwszVal : L""))
                    hr = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
                PropVariantClear(&name);
                PropVariantClear(&manufacturer);
            }
        }
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
            ComPtr<IAudioClient3> modern;
            bool initialized = false;
            if (SUCCEEDED(client.As(&modern))) {
                UINT32 default_period = 0, fundamental = 0, minimum = 0, maximum = 0;
                if (SUCCEEDED(modern->GetSharedModeEnginePeriod(mix, &default_period, &fundamental, &minimum,
                                                                &maximum)) &&
                    fundamental && minimum <= maximum) {
                    UINT32 desired = std::max(minimum, ((format.rate / 100 + fundamental - 1) / fundamental) *
                                                           fundamental);
                    desired = std::min(desired, maximum);
                    hr = modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, desired, mix,
                                                             nullptr);
                    initialized = SUCCEEDED(hr);
                    if (initialized)
                        period = desired;
                }
            }
            if (!initialized) { // Fresh client: a failed initialization must not leave partial state.
                modern.Reset();
                client.Reset();
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                      reinterpret_cast<void **>(client.GetAddressOf()));
                if (SUCCEEDED(hr))
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                            200000, 0, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    REFERENCE_TIME def = 0, min = 0;
                    client->GetDevicePeriod(&def, &min);
                    period = static_cast<unsigned>(def * format.rate / 10000000);
                }
            }
        }
        if (SUCCEEDED(hr))
            hr = client->SetEventHandle(sample_event);
        if (SUCCEEDED(hr))
            hr = client->GetService(IID_PPV_ARGS(&capture_client));
        if (SUCCEEDED(hr) && !stopping.load())
            hr = client->Start();
        if (FAILED(hr)) {
            trip(hr);
            SetEvent(ready_event);
            cleanup();
            return;
        }
        SetEvent(ready_event);
        mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        HANDLE waits[2]{stop_event, sample_event};
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        while (!stopping.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2000);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait != WAIT_OBJECT_0 + 1) {
                trip(wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                          : HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            UINT32 pending = 0;
            hr = capture_client->GetNextPacketSize(&pending);
            if (FAILED(hr)) {
                trip(hr);
                break;
            }
            unsigned drained = 0;
            while (pending && !stopping.load(std::memory_order_acquire) && drained++ < 32) {
                BYTE *samples = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 position = 0, qpc100ns = 0;
                hr = capture_client->GetBuffer(&samples, &frames, &flags, &position, &qpc100ns);
                if (FAILED(hr)) {
                    trip(hr);
                    break;
                }
                const std::uint64_t bytes = std::uint64_t(frames) * format.alignment;
                const auto write = produced.load(std::memory_order_relaxed),
                           read = consumed.load(std::memory_order_acquire);
                HRESULT packet_fault = S_OK;
                if (bytes && !(flags & AUDCLNT_BUFFERFLAGS_SILENT) && !samples)
                    packet_fault = BadSamples;
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && captured.load() > 0) {
                    ++discontinuities;
                    packet_fault = Discontinuity;
                }
                if (bytes > PacketCapacity || write - read >= QueueCapacity) {
                    ++dropped;
                    packet_fault = QueueOverflow;
                }
                if (SUCCEEDED(packet_fault)) {
                    auto &slot = queue[write % QueueCapacity];
                    slot.frames = frames;
                    slot.flags = flags;
                    slot.device_position = position;
                    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                        LARGE_INTEGER now{};
                        QueryPerformanceCounter(&now);
                        slot.capture_qpc = now.QuadPart;
                    } else
                        slot.capture_qpc = static_cast<std::uint64_t>(static_cast<long double>(qpc100ns) *
                                                                      frequency.QuadPart / 10000000.0L);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && bytes)
                        std::memcpy(slot.bytes.data(), samples, static_cast<std::size_t>(bytes));
                    produced.store(write + 1, std::memory_order_release);
                    ++captured;
                    const unsigned water = static_cast<unsigned>(write + 1 - read);
                    unsigned previous = high_water.load();
                    while (water > previous && !high_water.compare_exchange_weak(previous, water)) {
                    }
                    SetEvent(data_event);
                }
                hr = capture_client->ReleaseBuffer(frames);
                if (FAILED(packet_fault)) {
                    trip(packet_fault);
                    break;
                }
                if (FAILED(hr)) {
                    trip(hr);
                    break;
                }
                hr = capture_client->GetNextPacketSize(&pending);
                if (FAILED(hr)) {
                    trip(hr);
                    break;
                }
            }
            if (pending && !stopping.load()) {
                trip(QueueOverflow);
                break;
            }
        }
        cleanup();
        SetEvent(data_event);
    }
    void worker() {
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        MonoResampler converter(format.rate, SampleRate);
        std::array<float, Hop> input{}, output{};
        std::array<float, 8> converted{};
        unsigned fill = 0, consecutive_misses = 0;
        FrameResult result{};
        HANDLE waits[2]{stop_event, data_event};
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        LONGLONG pending_ticks = 0;
        while (!stopping.load(std::memory_order_acquire)) {
            if (!enrollment && !renderer.healthy()) {
                const auto route = renderer.snapshot();
                set_error(route.error.empty() ? "Virtual-cable renderer stopped; no output fallback"
                                              : route.error);
                trip(E_FAIL);
                break;
            }
            const auto read = consumed.load(std::memory_order_relaxed),
                       write = produced.load(std::memory_order_acquire);
            if (read == write) {
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 20);
                if (wait == WAIT_OBJECT_0)
                    break;
                if (wait == WAIT_FAILED) {
                    trip(HRESULT_FROM_WIN32(GetLastError()));
                    break;
                }
                continue;
            }
            const auto &packet = queue[read % QueueCapacity];
            LARGE_INTEGER now{}, segment_begin{};
            QueryPerformanceCounter(&segment_begin);
            now = segment_begin;
            // An explicit engineering safety limit, not a latency acceptance
            // claim: never replay a packet aged by an accumulated backlog.
            if (packet.capture_qpc && now.QuadPart > 0 &&
                static_cast<std::uint64_t>(now.QuadPart) > packet.capture_qpc &&
                static_cast<std::uint64_t>(now.QuadPart) - packet.capture_qpc >
                    static_cast<std::uint64_t>(frequency.QuadPart) * 80 / 1000) {
                trip(Discontinuity);
                break;
            }
            for (UINT32 frame = 0; frame < packet.frames && !stopping.load(std::memory_order_acquire);
                 ++frame) {
                const float sample =
                    (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        ? 0
                        : format.mono(packet.bytes.data() + std::size_t(frame) * format.alignment);
                if (!std::isfinite(sample)) {
                    trip(BadSamples);
                    break;
                }
                const std::size_t count = converter.push(sample, converted.data(), converted.size());
                if (converter.faulted()) {
                    trip(BadSamples);
                    break;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    if (enrollment) {
                        const auto index = reference_count.load(std::memory_order_relaxed);
                        if (index < reference_goal) {
                            reference[index] = converted[i];
                            reference_count.store(index + 1, std::memory_order_release);
                        }
                        if (index + 1 >= reference_goal) {
                            mode.store(SessionState::EnrollmentReady, std::memory_order_release);
                            stopping.store(true);
                            SetEvent(stop_event);
                            break;
                        }
                    } else {
                        input[fill++] = converted[i];
                        if (fill == Hop) {
                            LARGE_INTEGER before{}, after{};
                            QueryPerformanceCounter(&before);
                            pending_ticks += before.QuadPart - segment_begin.QuadPart;
                            if (bypass)
                                output = input;
                            else
                                engine->process_hop(input, output, result);
                            if (!bypass && !result.valid)
                                output.fill(0);
                            if (!bypass &&
                                (!result.solver_finite || (!result.valid && !result.budget_exhausted))) {
                                output.fill(0);
                                trip(EngineFault);
                                break;
                            }
                            bool submitted = false;
                            if (!stopping.load(std::memory_order_acquire))
                                submitted = renderer.enqueue(output.data(), output.size(), epoch);
                            if (!submitted && !stopping.load()) {
                                const auto route = renderer.snapshot();
                                set_error(route.error.empty() ? "Output render FIFO rejected the current hop"
                                                              : route.error);
                                trip(E_FAIL);
                                break;
                            }
                            QueryPerformanceCounter(&after);
                            const double us = double(pending_ticks + after.QuadPart - before.QuadPart) *
                                              1000000.0 / frequency.QuadPart;
                            pending_ticks = 0;
                            segment_begin = after;
                            compute = us;
                            maximum_compute = std::max(maximum_compute.load(), us);
                            if (us >= 10000) {
                                ++misses;
                                ++consecutive_misses;
                            } else
                                consecutive_misses = 0;
                            confidence = result.target_confidence;
                            residual = result.residual_ratio;
                            engine_compute = bypass ? 0 : result.compute_us;
                            ++hops;
                            fill = 0;
                            if (consecutive_misses >= 3) {
                                trip(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                                break;
                            }
                        }
                    }
                }
            }
            LARGE_INTEGER segment_end{};
            QueryPerformanceCounter(&segment_end);
            pending_ticks += segment_end.QuadPart - segment_begin.QuadPart;
            consumed.store(read + 1, std::memory_order_release);
        }
        input.fill(0);
        output.fill(0);
        converted.fill(0);
        converter.reset();
        if (engine)
            engine->reset(next_epoch());
        renderer.stop();
        if (FAILED(fault.load())) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (error.empty())
                error = explain(fault.load());
            mode = SessionState::Fault;
        }
        if (mmcss)
            AvRevertMmThreadCharacteristics(mmcss);
    }
    bool begin(const std::wstring &selected, std::string &output_error) {
        device_id = selected;
        epoch = next_epoch();
        stopping = false;
        fault = S_OK;
        produced = 0;
        consumed = 0;
        reference_count = 0;
        captured = 0;
        hops = 0;
        dropped = 0;
        discontinuities = 0;
        misses = 0;
        high_water = 0;
        compute = 0;
        maximum_compute = 0;
        engine_compute = 0;
        confidence = 0;
        residual = 0;
        period = 0;
        set_error({});
        if (!stop_event || !data_event || !ready_event) {
            output_error = "Unable to allocate audio events";
            return false;
        }
        ResetEvent(stop_event);
        ResetEvent(data_event);
        ResetEvent(ready_event);
        mode = SessionState::Starting;
        if (engine)
            engine->reset(epoch);
        if (!enrollment && !renderer.start(render_device_id, epoch, output_error)) {
            mode = SessionState::Fault;
            set_error(output_error);
            renderer.stop();
            return false;
        }
        try {
            capture_thread = std::thread([this] {
                try {
                    capture();
                } catch (...) {
                    trip(E_FAIL);
                    SetEvent(ready_event);
                }
            });
        } catch (...) {
            output_error = "Unable to start physical capture thread";
            renderer.stop();
            mode = SessionState::Fault;
            set_error(output_error);
            return false;
        }
        if (WaitForSingleObject(ready_event, 5000) != WAIT_OBJECT_0 || FAILED(fault.load())) {
            output_error = FAILED(fault.load()) ? explain(fault.load()) : "Audio initialization timed out";
            stop();
            set_error(output_error);
            mode = SessionState::Fault;
            return false;
        }
        try {
            worker_thread = std::thread([this] {
                try {
                    worker();
                } catch (...) {
                    set_error("Audio worker exception; fail-silent stop");
                    trip(E_FAIL);
                    renderer.stop();
                    mode = SessionState::Fault;
                }
            });
        } catch (...) {
            output_error = "Unable to start audio worker";
            stop();
            set_error(output_error);
            mode = SessionState::Fault;
            return false;
        }
        SessionState starting = SessionState::Starting;
        mode.compare_exchange_strong(starting,
                                     enrollment ? SessionState::Enrolling
                                                : (bypass ? SessionState::Bypass : SessionState::Filtering));
        return true;
    }
};
AudioSession::AudioSession() : state_(std::make_unique<State>()) {}
AudioSession::~AudioSession() = default;
bool AudioSession::start_enrollment(const std::wstring &id, unsigned seconds, std::string &error) {
    error.clear();
    stop();
    if (seconds < 10 || seconds > 30) {
        error = "Enrollment must last 10 to 30 seconds";
        return false;
    }
    std::vector<Device> devices;
    if (!enumerate_capture_devices(devices, error))
        return false;
    const auto found = std::find_if(devices.begin(), devices.end(),
                                    [&](const Device &d) { return d.id == id && !d.virtual_microphone; });
    if (found == devices.end()) {
        error = "Select an active physical microphone, not NoiseKiller Microphone";
        return false;
    }
    state_->enrollment = true;
    state_->bypass = false;
    state_->reference_goal = seconds * SampleRate;
    state_->reference.assign(state_->reference_goal, 0);
    return state_->begin(id, error);
}
bool AudioSession::start_live(const std::wstring &id, const std::wstring &output_id, const Profile &profile,
                              bool bypass, std::string &error) {
    error.clear();
    stop();
    if (!profile.valid()) {
        error = "A valid compatible enrollment profile is required";
        return false;
    }
    std::vector<Device> devices;
    if (!enumerate_capture_devices(devices, error))
        return false;
    bool physical = false;
    for (const auto &d : devices) {
        physical |= d.id == id && !d.virtual_microphone;
    }
    if (!physical) {
        error = "Select an active physical microphone";
        return false;
    }
    if (output_id.empty()) {
        error = "Select a virtual-cable playback endpoint explicitly; no default speaker is used";
        return false;
    }
    std::vector<Device> outputs;
    if (!enumerate_render_devices(outputs, error))
        return false;
    const auto route = std::find_if(outputs.begin(), outputs.end(),
                                    [&](const Device &d) { return d.id == output_id && d.virtual_cable; });
    if (route == outputs.end()) {
        error = "Selected supported VB-CABLE playback endpoint (CABLE Input) is absent/inactive; choose its "
                "corresponding CABLE Output in the target app";
        return false;
    }
    const bool recording_available = std::any_of(devices.begin(), devices.end(), [&](const Device &d) {
        return d.virtual_microphone &&
               paired_cable_endpoints(route->name, route->manufacturer, d.name, d.manufacturer);
    });
    if (!recording_available) {
        error = "Matching active virtual-cable recording endpoint (CABLE Output) is absent; playback-name "
                "matching is only a routing hint, not verified loopback or authentication";
        return false;
    }
    state_->render_device_id = output_id;
    state_->enrollment = false;
    state_->bypass = bypass;
    state_->reference_goal = 0;
    try {
        state_->engine = std::make_unique<Engine>(Config{}, profile);
    } catch (...) {
        error = "Unable to initialize MECAG engine";
        state_->renderer.stop();
        return false;
    }
    return state_->begin(id, error);
}
void AudioSession::stop() noexcept { state_->stop(); }
AudioSnapshot AudioSession::snapshot() const {
    AudioSnapshot s;
    s.state = state_->mode.load();
    s.input_rate = state_->format.rate;
    s.input_channels = state_->format.channels;
    s.capture_period_frames = state_->period.load();
    s.epoch = state_->epoch;
    s.captured_packets = state_->captured.load();
    s.processed_hops = state_->hops.load();
    s.dropped_packets = state_->dropped.load();
    s.discontinuities = state_->discontinuities.load();
    s.deadline_misses = state_->misses.load();
    s.enrollment_samples = state_->reference_count.load();
    s.enrollment_goal = state_->reference_goal;
    s.queue_high_water = state_->high_water.load();
    s.target_confidence = state_->confidence.load();
    s.residual_ratio = state_->residual.load();
    s.compute_us = state_->compute.load();
    s.maximum_compute_us = state_->maximum_compute.load();
    s.engine_compute_us = state_->engine_compute.load();
    s.output = state_->renderer.snapshot();
    {
        std::lock_guard<std::mutex> lock(state_->error_mutex);
        s.error = state_->error;
    }
    return s;
}
bool AudioSession::take_enrollment(std::vector<float> &samples, std::string &error) {
    error.clear();
    if (state_->mode.load() != SessionState::EnrollmentReady) {
        error = "Enrollment is incomplete";
        return false;
    }
    if (state_->capture_thread.joinable())
        state_->capture_thread.join();
    if (state_->worker_thread.joinable())
        state_->worker_thread.join();
    if (FAILED(state_->fault.load())) {
        error = explain(state_->fault.load());
        stop();
        return false;
    }
    for (auto &packet : state_->queue)
        SecureZeroMemory(packet.bytes.data(), packet.bytes.size());
    state_->produced = 0;
    state_->consumed = 0;
    samples = std::move(state_->reference);
    state_->reference_count = 0;
    state_->mode = SessionState::Stopped;
    state_->reference_goal = 0;
    return true;
}
} // namespace win
} // namespace nk
#endif
