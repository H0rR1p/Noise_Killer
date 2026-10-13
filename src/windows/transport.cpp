#include "noisekiller/transport.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace nk::win {
namespace {
constexpr DWORD IoTimeoutMs = 20;
// One irrecoverably pending native operation can retain its fixed buffer. Do
// not reuse/free OVERLAPPED storage while the OS may still touch it. The process
// then refuses all reconnects until restarted: this is a bounded safety failure,
// not a silent accumulation of abandoned requests.
std::atomic<bool> permanently_pending{false};
std::atomic<bool> client_claimed{false};
struct IoState {
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::array<unsigned char, sizeof(NK_SUBMIT)> input{};
    std::array<unsigned char, sizeof(NK_CAPABILITIES) + sizeof(NK_STATS)> output{};
    bool pending = false;
    ~IoState() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (event) CloseHandle(event);
    }
};
IoState* retained_pending = nullptr; // at most one allocation; never reused
void win_error(std::string& error, const char* action, DWORD code = GetLastError()) {
    error = std::string(action) + " (Win32=" + std::to_string(code) + ")";
}
bool complete(IoState& io, BOOL immediate, DWORD& bytes, std::string& error) {
    if (immediate) return true;
    const auto code = GetLastError();
    if (code != ERROR_IO_PENDING) { win_error(error, "Driver/broker I/O failed", code); return false; }
    io.pending = true;
    if (WaitForSingleObject(io.event, IoTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(io.handle, &io.overlapped);
        if (WaitForSingleObject(io.event, IoTimeoutMs) == WAIT_OBJECT_0) {
            DWORD discarded = 0;
            GetOverlappedResult(io.handle, &io.overlapped, &discarded, FALSE);
            io.pending = false;
        }
        error = "Driver/broker response exceeded bounded timeout; session must stop";
        return false;
    }
    io.pending = false;
    if (!GetOverlappedResult(io.handle, &io.overlapped, &bytes, FALSE)) {
        win_error(error, "Driver/broker completion failed"); return false;
    }
    return true;
}
void prepare(IoState& io) noexcept {
    io.overlapped = {};
    io.overlapped.hEvent = io.event;
    ResetEvent(io.event);
    io.output.fill(0);
}
bool ioctl(IoState& io, DWORD code, const void* input, DWORD input_size,
           void* output, DWORD output_size, std::string& error) {
    if (io.pending || input_size > io.input.size() || output_size > io.output.size()) {
        error = "Native operation unavailable or oversized"; return false;
    }
    prepare(io);
    std::memcpy(io.input.data(), input, input_size);
    DWORD bytes = 0;
    const auto done = DeviceIoControl(io.handle, code, io.input.data(), input_size,
        output_size ? io.output.data() : nullptr, output_size, &bytes, &io.overlapped);
    if (!complete(io, done, bytes, error)) return false;
    if (bytes != output_size) { error = "Driver returned an incompatible fixed-size response"; return false; }
    if (output_size) std::memcpy(output, io.output.data(), output_size);
    return true;
}
bool broker_exchange(IoState& io, const NK_BROKER_REQUEST& request,
                     NK_BROKER_REPLY& reply, std::string& error) {
    prepare(io);
    std::memcpy(io.input.data(), &request, sizeof(request));
    DWORD bytes = 0;
    if (!complete(io, WriteFile(io.handle, io.input.data(), sizeof(request), &bytes,
        &io.overlapped), bytes, error)) return false;
    if (bytes != sizeof(request)) { error = "Broker request was truncated"; return false; }
    prepare(io);
    if (!complete(io, ReadFile(io.handle, io.output.data(), sizeof(reply), &bytes,
        &io.overlapped), bytes, error)) return false;
    if (bytes != sizeof(reply)) { error = "Broker response was truncated"; return false; }
    std::memcpy(&reply, io.output.data(), sizeof(reply));
    return true;
}
bool trusted_service_pid(DWORD& process_id, std::string& error) {
    const auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { win_error(error, "Cannot query TrustedBroker installation"); return false; }
    const auto service = OpenServiceW(scm, NK_BROKER_SERVICE_NAME,
        SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    CloseServiceHandle(scm);
    if (!service) {
        error = "NoiseKiller signed capture driver/TrustedBroker is not installed; live output is disabled";
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    const bool running = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) &&
        status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId != 0;
    DWORD config_bytes = 0;
    QueryServiceConfigW(service, nullptr, 0, &config_bytes);
    // A bounded configuration read is outside the real-time path.
    std::vector<BYTE> config(config_bytes <= 64 * 1024 ? config_bytes : 0);
    bool system = false;
    if (!config.empty()) {
        auto* entry = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data());
        system = QueryServiceConfigW(service, entry, config_bytes, &needed) &&
            entry->lpServiceStartName &&
            (_wcsicmp(entry->lpServiceStartName, L"LocalSystem") == 0);
    }
    CloseServiceHandle(service);
    if (!running || !system) {
        error = "Installed TrustedBroker is not a running LocalSystem service; refusing producer access";
        return false;
    }
    process_id = status.dwProcessId;
    return true;
}
bool valid_header(const NK_HEADER& h, std::size_t size, NK_OPERATION op) noexcept {
    return h.size == size && h.version == NK_DRIVER_VERSION && h.operation == op;
}
}
struct DriverTransport::Impl {
    std::unique_ptr<IoState> io;
    std::uint64_t epoch = 0, sequence = 0, next_sample = 0;
    bool ready = false, active = false, failed = false, owns_lease = false;
};
DriverTransport::DriverTransport() : impl_(std::make_unique<Impl>()) {}
DriverTransport::~DriverTransport() { close(); }
bool DriverTransport::available() const noexcept { return impl_->ready && !impl_->failed; }
bool DriverTransport::open(std::string& error) {
    close();
    error.clear();
    if (permanently_pending.load()) { error = "Native I/O remained pending; restart NoiseKiller before retry"; return false; }
    DWORD service_pid = 0;
    if (!trusted_service_pid(service_pid, error)) return false;
    bool unclaimed = false;
    if (!client_claimed.compare_exchange_strong(unclaimed, true)) {
        error = "A native producer client is already open in this process"; return false;
    }
    impl_->owns_lease = true;
    auto& io = impl_->io;
    io = std::make_unique<IoState>();
    io->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io->event) { win_error(error, "Cannot allocate native completion event"); close(); return false; }
    io->handle = CreateFileW(NK_BROKER_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (io->handle == INVALID_HANDLE_VALUE) {
        win_error(error, "TrustedBroker pipe is unavailable"); close(); return false;
    }
    ULONG server_pid = 0;
    DWORD service_pid_after = 0;
    if (!GetNamedPipeServerProcessId(io->handle, &server_pid) || server_pid != service_pid ||
        !trusted_service_pid(service_pid_after, error) || service_pid != service_pid_after) {
        error = "Broker pipe peer does not match the SCM-authenticated service process";
        close(); return false;
    }
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        win_error(error, "Cannot determine client session"); close(); return false;
    }
    NK_BROKER_REQUEST request{};
    request.header = {sizeof(request), NK_DRIVER_VERSION, NK_OP_GRANT, 0};
    request.claimed_process_id = GetCurrentProcessId();
    request.claimed_session_id = session;
    NK_BROKER_REPLY reply{};
    if (!broker_exchange(*io, request, reply, error)) { close(); return false; }
    if (!valid_header(reply.header, sizeof(reply), NK_OP_GRANT) || reply.header.epoch ||
        reply.status || reply.reserved || !reply.duplicated_device_handle ||
        reply.duplicated_device_handle == UINT64_MAX ||
        reply.duplicated_device_handle > static_cast<std::uint64_t>(UINTPTR_MAX)) {
        error = "TrustedBroker did not grant a compatible authorized producer handle";
        close(); return false;
    }
    const auto device = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(reply.duplicated_device_handle));
    DWORD handle_flags = 0;
    if (!GetHandleInformation(device, &handle_flags) ||
        !SetHandleInformation(device, HANDLE_FLAG_INHERIT, 0)) {
        win_error(error, "Broker granted an invalid producer handle"); close(); return false;
    }
    CloseHandle(io->handle);
    io->handle = device;
    NK_HEADER query{sizeof(query), NK_DRIVER_VERSION, NK_OP_CAPABILITIES, 0};
    NK_CAPABILITIES capabilities{};
    if (!ioctl(*io, NK_IOCTL_CAPABILITIES, &query, sizeof(query), &capabilities,
        sizeof(capabilities), error)) { close(); return false; }
    LARGE_INTEGER qpc_frequency{};
    QueryPerformanceFrequency(&qpc_frequency);
    if (!valid_header(capabilities.header, sizeof(capabilities), NK_OP_CAPABILITIES) ||
        capabilities.header.epoch ||
        (capabilities.flags & NK_REQUIRED_CAPABILITIES) != NK_REQUIRED_CAPABILITIES ||
        capabilities.sample_rate != NK_DRIVER_RATE || capabilities.channels != NK_DRIVER_CHANNELS ||
        capabilities.bits != NK_DRIVER_BITS || capabilities.hop_samples != NK_DRIVER_HOP ||
        capabilities.max_queue_hops != NK_DRIVER_QUEUE_HOPS ||
        capabilities.watchdog_ms != NK_DRIVER_WATCHDOG_MS || !capabilities.driver_build ||
        capabilities.qpc_frequency != static_cast<std::uint64_t>(qpc_frequency.QuadPart)) {
        error = "Capture driver capabilities/format/security/clock do not satisfy DriverContract v1";
        close(); return false;
    }
    impl_->ready = true;
    impl_->failed = false;
    return true;
}
bool DriverTransport::start(std::uint64_t epoch, std::string& error) {
    error.clear();
    if (!available() || impl_->active || !epoch) { error = "Driver unavailable, already active, or epoch invalid"; return false; }
    NK_CONFIGURE request{};
    request.header = {sizeof(request), NK_DRIVER_VERSION, NK_OP_CONFIGURE, epoch};
    request.sample_rate = NK_DRIVER_RATE;
    request.channels = NK_DRIVER_CHANNELS;
    request.bits = NK_DRIVER_BITS;
    request.hop_samples = NK_DRIVER_HOP;
    request.max_queue_hops = NK_DRIVER_QUEUE_HOPS;
    if (!ioctl(*impl_->io, NK_IOCTL_CONFIGURE, &request, sizeof(request), nullptr, 0, error)) {
        impl_->failed = true; close(); return false;
    }
    impl_->epoch = epoch;
    impl_->sequence = impl_->next_sample = 0;
    impl_->active = true;
    return true;
}
bool DriverTransport::write(const float* pcm, std::size_t count, std::uint64_t first_sample,
                             std::uint64_t capture_qpc, std::string& error) {
    error.clear();
    if (!available() || !impl_->active || !pcm || count != NK_DRIVER_HOP ||
        first_sample != impl_->next_sample || impl_->next_sample > UINT64_MAX - count ||
        impl_->sequence == UINT64_MAX) {
        error = "Driver PCM submit is not an active, contiguous, fixed-size hop";
        stop(); return false;
    }
    NK_SUBMIT request{};
    request.header = {sizeof(request), NK_DRIVER_VERSION, NK_OP_SUBMIT, impl_->epoch};
    request.sequence = impl_->sequence;
    request.first_sample_index = first_sample;
    request.capture_qpc = capture_qpc;
    request.sample_count = NK_DRIVER_HOP;
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(pcm[i])) { error = "Non-finite PCM rejected; session stopped"; stop(); return false; }
        const auto clipped = std::clamp(pcm[i], -1.0f, 1.0f);
        request.pcm[i] = static_cast<std::int16_t>(std::lround(clipped * (clipped < 0 ? 32768.0f : 32767.0f)));
    }
    if (!ioctl(*impl_->io, NK_IOCTL_SUBMIT, &request, sizeof(request), nullptr, 0, error)) {
        impl_->failed = true; close(); return false;
    }
    ++impl_->sequence;
    impl_->next_sample += count;
    return true;
}
bool DriverTransport::stats(TransportStats& result, std::string& error) {
    result = {};
    error.clear();
    if (!available()) { error = "Driver statistics unavailable"; return false; }
    NK_HEADER request{sizeof(request), NK_DRIVER_VERSION, NK_OP_STATS, impl_->epoch};
    if (!ioctl(*impl_->io, NK_IOCTL_STATS, &request, sizeof(request), &result, sizeof(result), error)) {
        impl_->failed = true; close(); return false;
    }
    if (!valid_header(result.header, sizeof(result), NK_OP_STATS) || result.reserved ||
        result.header.epoch != impl_->epoch || result.owner_active > 1 ||
        result.queued_samples > NK_DRIVER_HOP * NK_DRIVER_QUEUE_HOPS) {
        error = "Driver returned an invalid statistics snapshot"; impl_->failed = true; close(); return false;
    }
    return true;
}
void DriverTransport::stop() noexcept {
    if (!impl_->active) return;
    try {
        std::string ignored;
        NK_HEADER request{sizeof(request), NK_DRIVER_VERSION, NK_OP_STOP, impl_->epoch};
        if (!impl_->io || impl_->io->pending ||
            !ioctl(*impl_->io, NK_IOCTL_STOP, &request, sizeof(request), nullptr, 0, ignored))
            impl_->failed = true;
    } catch (...) { impl_->failed = true; }
    impl_->active = false;
    impl_->epoch = 0;
}
void DriverTransport::close() noexcept {
    stop();
    if (impl_->io && impl_->io->pending) {
        // Its completion never arrived during the cancellation grace period.
        // Retain bounded storage and ownership until process exit.
        permanently_pending.store(true);
        retained_pending = impl_->io.release();
    } else impl_->io.reset();
    if (impl_->owns_lease && !permanently_pending.load()) client_claimed.store(false);
    impl_->owns_lease = false;
    impl_->ready = false;
    impl_->active = false;
    impl_->epoch = impl_->sequence = impl_->next_sample = 0;
}
}
#endif
