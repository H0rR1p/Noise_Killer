#include "noisekiller/io.hpp"
#include "noisekiller/windows.hpp"
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

// Private, inherited stdin/stdout control transport. PCM never crosses this
// transport. Construction, status, inventory and --smoke cannot start audio.
namespace {
constexpr std::size_t MaximumLine = 65536;
constexpr std::uintmax_t MaximumJobFile = 64 * 1024 * 1024;

template <class T> struct WipeObject {
    T &value;
    ~WipeObject() { SecureZeroMemory(&value, sizeof(value)); }
};
struct WipeSamples {
    std::vector<float> &samples;
    ~WipeSamples() {
        if (!samples.empty())
            SecureZeroMemory(samples.data(), samples.size() * sizeof(float));
    }
};
struct TemporaryFile {
    std::wstring path;
    ~TemporaryFile() {
        if (!path.empty())
            DeleteFileW(path.c_str());
    }
};
std::string quote(const std::string &text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else
            result += static_cast<char>(c);
    }
    return result + '"';
}
std::string utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        throw std::runtime_error("Invalid UTF-16 device or path text");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
bool decode_argument(const std::string &encoded, std::wstring &value) {
    value.clear();
    if (encoded.empty())
        return true;
    if (encoded.size() % 4 != 0 || encoded.size() > MaximumLine)
        return false;
    const auto first_padding = encoded.find('=');
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(encoded[i]);
        const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!alphabet && !(c == '=' && i >= encoded.size() - 2 && i >= first_padding))
            return false;
        if (first_padding != std::string::npos && i >= first_padding && c != '=')
            return false;
    }
    DWORD count = 0;
    if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &count, nullptr, nullptr))
        return false;
    std::string bytes(count, '\0');
    if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              reinterpret_cast<BYTE *>(bytes.data()), &count, nullptr, nullptr))
        return false;
    bytes.resize(count);
    // Windows permits some noncanonical padding bits; round-trip to reject them.
    DWORD canonical_size = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE *>(bytes.data()), count,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &canonical_size))
        return false;
    std::string canonical(canonical_size, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE *>(bytes.data()), count,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, canonical.data(), &canonical_size))
        return false;
    if (!canonical.empty() && canonical.back() == '\0')
        canonical.pop_back();
    if (canonical != encoded || bytes.find('\0') != std::string::npos)
        return false;
    const int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                              static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_count <= 0 || wide_count > 32767)
        return false;
    value.resize(wide_count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                               static_cast<int>(bytes.size()), value.data(), wide_count) == wide_count;
}
struct Request {
    std::string id = "0", verb;
    std::vector<std::wstring> arguments;
};
bool parse(const std::string &line, Request &request) {
    if (line.empty() || line.size() > MaximumLine)
        return false;
    for (unsigned char c : line)
        if (c != '\t' && (c < 32 || c > 126))
            return false;
    std::vector<std::string> parts;
    std::size_t begin = 0;
    do {
        const auto end = line.find('\t', begin);
        parts.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    } while (parts.size() < 8);
    if (parts.size() < 3 || parts.size() > 6 || parts[0] != "NK1" || parts[1].empty() ||
        parts[1].size() > 16 || (parts[1].size() > 1 && parts[1][0] == '0'))
        return false;
    for (char c : parts[1])
        if (c < '0' || c > '9')
            return false;
    request.id = parts[1];
    request.verb = parts[2];
    for (std::size_t i = 3; i < parts.size(); ++i) {
        std::wstring value;
        if (!decode_argument(parts[i], value))
            return false;
        request.arguments.push_back(std::move(value));
    }
    return true;
}
bool read_line(std::string &line, bool &oversized) {
    line.clear();
    oversized = false;
    char c;
    while (std::cin.get(c)) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }
        if (line.size() < MaximumLine)
            line += c;
        else
            oversized = true;
    }
    // A truncated final command is never executed when the parent closes stdin.
    return false;
}
std::wstring profile_location() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!size)
        return {};
    std::wstring base(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), size);
    if (!copied || copied >= size)
        return {};
    base.resize(copied);
    return base + L"\\NoiseKiller\\active-profile.nkp";
}
bool absolute_file(const std::wstring &path) {
    if (path.empty() || path.size() > 32760 || path.rfind(L"\\\\.\\", 0) == 0 ||
        path.rfind(L"\\\\?\\", 0) == 0)
        return false;
    for (wchar_t c : path)
        if (c < 32)
            return false;
    const std::filesystem::path file(path);
    return file.is_absolute() && file.has_filename();
}
std::wstring temporary_path(const std::wstring &destination) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Unable to create unique temporary file name");
    wchar_t name[40]{};
    StringFromGUID2(guid, name, 40);
    return destination + L".nk-tmp-" + name;
}
bool allowed_file(const std::wstring &path, std::string &error) {
    if (!absolute_file(path)) {
        error = "An absolute WAV file path is required";
        return false;
    }
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size > MaximumJobFile) {
        error = "WAV is unavailable or exceeds the desktop limit of 64 MiB";
        return false;
    }
    return true;
}
double finite(double number) { return std::isfinite(number) ? number : 0; }
const char *state_name(nk::win::SessionState state) {
    switch (state) {
    case nk::win::SessionState::Stopped: return "stopped";
    case nk::win::SessionState::Starting: return "starting";
    case nk::win::SessionState::Enrolling: return "enrolling";
    case nk::win::SessionState::Filtering: return "filtering";
    case nk::win::SessionState::Bypass: return "bypass";
    case nk::win::SessionState::EnrollmentReady: return "enrollmentReady";
    case nk::win::SessionState::Fault: return "fault";
    }
    return "fault";
}
struct JobResult {
    bool ok = false, has_profile = false;
    nk::Profile profile{};
    std::string message, error;
    JobResult() = default;
    JobResult(const JobResult &) = default;
    JobResult &operator=(const JobResult &) = default;
    JobResult(JobResult &&) noexcept = default;
    JobResult &operator=(JobResult &&) noexcept = default;
    ~JobResult() { SecureZeroMemory(&profile, sizeof(profile)); }
};
class Host {
    nk::win::AudioSession audio_;
    nk::Profile profile_{};
    std::wstring profile_path_;
    std::thread worker_;
    std::atomic<bool> cancelled_{false}, finished_{false};
    std::mutex result_mutex_;
    JobResult result_;
    bool busy_ = false;
    std::string job_kind_, job_message_, job_error_;
    std::optional<nk::win::AudioSnapshot> fault_;

    bool idle(std::string &error) const {
        if (busy_) {
            error = "A background job is running; stop or wait for completion";
            return false;
        }
        if (fault_ || audio_.snapshot().state != nk::win::SessionState::Stopped) {
            error = "Stop the current session before changing registration or routing";
            return false;
        }
        return true;
    }
    void start_job(const char *kind, std::function<JobResult()> action) {
        if (worker_.joinable())
            worker_.join();
        cancelled_.store(false);
        finished_.store(false);
        busy_ = true;
        job_kind_ = kind;
        job_message_ = "Working locally; no automatic live capture";
        job_error_.clear();
        try {
            worker_ = std::thread([this, action = std::move(action)] {
                JobResult completed;
                try {
                    completed = action();
                } catch (const std::exception &exception) {
                    completed.error = exception.what();
                } catch (...) {
                    completed.error = "Background operation failed";
                }
                {
                    std::lock_guard<std::mutex> lock(result_mutex_);
                    result_ = std::move(completed);
                }
                finished_.store(true, std::memory_order_release);
            });
        } catch (...) {
            busy_ = false;
            job_error_ = "Unable to start background job";
            throw;
        }
    }
    void collect_job() {
        if (!busy_ || !finished_.load(std::memory_order_acquire))
            return;
        worker_.join();
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (result_.ok && result_.has_profile) {
            SecureZeroMemory(&profile_, sizeof(profile_));
            profile_ = result_.profile;
        }
        job_message_ = result_.message;
        job_error_ = result_.error;
        SecureZeroMemory(&result_.profile, sizeof(result_.profile));
        result_ = JobResult{};
        busy_ = false;
        finished_.store(false);
    }
    bool resample_cancelable(const nk::AudioData &data, std::vector<float> &mono) {
        nk::MonoResampler converter(data.sample_rate, nk::SampleRate);
        mono.reserve(data.samples.size() * std::size_t(nk::SampleRate) / data.sample_rate + 8);
        std::array<float, 8> converted{};
        WipeObject<decltype(converted)> wipe{converted};
        for (std::size_t i = 0; i < data.samples.size(); ++i) {
            if (i % 4096 == 0 && cancelled_.load()) {
                converter.reset();
                return false;
            }
            const auto count = converter.push(data.samples[i], converted.data(), converted.size());
            if (converter.faulted())
                throw std::runtime_error("Reference resampling failed");
            mono.insert(mono.end(), converted.begin(), converted.begin() + count);
        }
        converter.reset();
        return !cancelled_.load();
    }
    JobResult enroll_samples(std::vector<float> &samples) {
        JobResult result;
        if (cancelled_.load()) {
            result.message = "Enrollment cancelled";
            return result;
        }
        if (!nk::enroll(samples.data(), samples.size(), result.profile, result.error))
            return result;
        if (cancelled_.load()) {
            result.message = "Enrollment cancelled; previous profile is unchanged";
            return result;
        }
        if (profile_path_.empty()) {
            result.error = "Current-user profile directory is unavailable";
            return result;
        }
        std::error_code code;
        std::filesystem::create_directories(std::filesystem::path(profile_path_).parent_path(), code);
        if (code) {
            result.error = "Unable to create current-user encrypted profile directory";
            return result;
        }
        TemporaryFile encrypted{temporary_path(profile_path_)};
        if (!nk::win::save_profile(encrypted.path, result.profile, result.error))
            return result;
        if (cancelled_.load()) {
            result.message = "Enrollment cancelled; previous profile is unchanged";
            return result;
        }
        // This move is the enrollment commit point. Cancellation is best-effort
        // once committed; subsequent stop does not delete a completed profile.
        if (!MoveFileExW(encrypted.path.c_str(), profile_path_.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            result.error = "Unable to commit encrypted profile (Windows " + std::to_string(GetLastError()) + ")";
            return result;
        }
        encrypted.path.clear();
        result.ok = result.has_profile = true;
        result.message = "Registration complete; original PCM was not saved; live capture remains stopped";
        return result;
    }
    bool validate_input(const std::wstring &id, std::string &error) {
        if (id.empty()) {
            error = "Select a physical microphone explicitly";
            return false;
        }
        std::vector<nk::win::Device> devices;
        if (!nk::win::enumerate_capture_devices(devices, error))
            return false;
        for (const auto &device : devices)
            if (device.id == id && !device.virtual_microphone)
                return true;
        error = "Selected physical microphone is unavailable or is a virtual capture endpoint";
        return false;
    }
    bool validate_output(const std::wstring &id, std::string &error) {
        if (id.empty()) {
            error = "Select a supported virtual-cable playback endpoint explicitly";
            return false;
        }
        std::vector<nk::win::Device> devices;
        if (!nk::win::enumerate_render_devices(devices, error))
            return false;
        for (const auto &device : devices)
            if (device.id == id && device.virtual_cable)
                return true;
        error = "Selected virtual-cable playback endpoint is unavailable or unsupported";
        return false;
    }
    void tick() {
        collect_job();
        const auto snapshot = audio_.snapshot();
        if (snapshot.state == nk::win::SessionState::Fault) {
            fault_ = snapshot;
            audio_.stop();
        } else if (snapshot.state == nk::win::SessionState::EnrollmentReady && !busy_) {
            std::vector<float> samples;
            std::string error;
            if (!audio_.take_enrollment(samples, error)) {
                job_error_ = error;
                audio_.stop();
                return;
            }
            audio_.stop();
            // A shared buffer also wipes if thread construction throws before
            // the job action runs. It never leaves this native process.
            auto owned = std::shared_ptr<std::vector<float>>(new std::vector<float>(std::move(samples)),
                [](std::vector<float> *pcm) {
                    if (!pcm->empty())
                        SecureZeroMemory(pcm->data(), pcm->size() * sizeof(float));
                    delete pcm;
                });
            start_job("enroll", [this, owned] { return enroll_samples(*owned); });
        }
    }
    std::string profile_json() const {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "{\"registered\":" << (profile_.valid() ? "true" : "false")
            << ",\"prototypeCount\":" << (profile_.valid() ? profile_.prototype_count : 0)
            << ",\"acceptedSeconds\":" << finite(profile_.valid() ? profile_.accepted_seconds : 0) << '}';
        return out.str();
    }
    std::string status() {
        const auto snapshot = fault_ ? *fault_ : audio_.snapshot();
        const auto &output = snapshot.output;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(12) << "{\"schema\":1,\"algorithmVersion\":" << quote(nk::AlgorithmVersion)
            << ",\"featureSchema\":" << nk::FeatureSchema << ",\"profile\":" << profile_json()
            << ",\"session\":{\"state\":" << quote(state_name(snapshot.state))
            << ",\"processedHops\":" << snapshot.processed_hops
            << ",\"capturedPackets\":" << snapshot.captured_packets
            << ",\"targetConfidence\":" << finite(snapshot.target_confidence)
            << ",\"residualRatio\":" << finite(snapshot.residual_ratio)
            << ",\"computeUs\":" << finite(snapshot.compute_us)
            << ",\"maximumComputeUs\":" << finite(snapshot.maximum_compute_us)
            << ",\"engineComputeUs\":" << finite(snapshot.engine_compute_us)
            << ",\"deadlineMisses\":" << snapshot.deadline_misses
            << ",\"inputRate\":" << snapshot.input_rate << ",\"inputChannels\":" << snapshot.input_channels
            << ",\"capturePeriodFrames\":" << snapshot.capture_period_frames
            << ",\"enrollmentSamples\":" << snapshot.enrollment_samples
            << ",\"enrollmentGoal\":" << snapshot.enrollment_goal
            << ",\"queueHighWater\":" << snapshot.queue_high_water
            << ",\"droppedPackets\":" << snapshot.dropped_packets
            << ",\"discontinuities\":" << snapshot.discontinuities
            << ",\"error\":" << quote(snapshot.error)
            << ",\"output\":{\"queuedInputSamples\":" << output.queued_input_samples
            << ",\"queueHighWater\":" << output.queue_high_water
            << ",\"underflowEvents\":" << output.underflow_events
            << ",\"rate\":" << output.sample_rate << ",\"channels\":" << output.channels
            << ",\"periodFrames\":" << output.period_frames << ",\"paddingFrames\":" << output.padding_frames
            << ",\"client3\":" << (output.client3 ? "true" : "false")
            << ",\"driftPpm\":" << finite(output.drift_ppm) << ",\"error\":" << quote(output.error)
            << "}},\"job\":{\"busy\":" << (busy_ ? "true" : "false") << ",\"kind\":" << quote(job_kind_)
            << ",\"message\":" << quote(job_message_) << ",\"error\":" << quote(job_error_) << "}}";
        return out.str();
    }
    std::string devices(std::string &error) {
        std::vector<nk::win::Device> inputs, outputs;
        if (!nk::win::enumerate_capture_devices(inputs, error) ||
            !nk::win::enumerate_render_devices(outputs, error))
            return {};
        const auto list = [](const std::vector<nk::win::Device> &items, bool output) {
            std::string json = "[";
            bool first = true;
            for (const auto &device : items) {
                if (output ? !device.virtual_cable : device.virtual_microphone)
                    continue;
                if (!first)
                    json += ',';
                first = false;
                json += "{\"id\":" + quote(utf8(device.id)) + ",\"name\":" + quote(utf8(device.name)) + '}';
            }
            return json + ']';
        };
        return "{\"inputs\":" + list(inputs, false) + ",\"outputs\":" + list(outputs, true) + '}';
    }

  public:
    explicit Host(std::wstring profile_path) : profile_path_(std::move(profile_path)) {
        if (!profile_path_.empty() && GetFileAttributesW(profile_path_.c_str()) != INVALID_FILE_ATTRIBUTES)
            nk::win::load_profile(profile_path_, profile_, job_error_);
    }
    ~Host() {
        cancelled_.store(true);
        audio_.stop();
        if (worker_.joinable())
            worker_.join();
        SecureZeroMemory(&profile_, sizeof(profile_));
        SecureZeroMemory(&result_.profile, sizeof(result_.profile));
    }
    std::string execute(const Request &request, bool &shutdown, std::string &error) {
        // stop/shutdown preempt a ready recording without starting enrollment.
        if (request.verb != "stop" && request.verb != "shutdown")
            tick();
        const auto &arguments = request.arguments;
        const auto require_count = [&](std::size_t count) {
            if (arguments.size() == count)
                return true;
            error = "Wrong argument count";
            return false;
        };
        if (request.verb == "status")
            return require_count(0) ? status() : "";
        if (request.verb == "devices")
            return require_count(0) ? devices(error) : "";
        if (request.verb == "stop" || request.verb == "shutdown") {
            if (!require_count(0))
                return {};
            cancelled_.store(true);
            audio_.stop();
            fault_.reset();
            collect_job();
            shutdown = request.verb == "shutdown";
            return status();
        }
        if (request.verb == "record") {
            if (!require_count(1) || !idle(error) || !validate_input(arguments[0], error))
                return {};
            if (!audio_.start_enrollment(arguments[0], 15, error))
                return {};
            job_kind_ = "record";
            job_error_.clear();
            job_message_ = "Recording 15 seconds in memory for registration; stop cancels it";
            return status();
        }
        if (request.verb == "start") {
            if (!require_count(3) || !idle(error))
                return {};
            if (arguments[2] != L"filter" && arguments[2] != L"bypass") {
                error = "Mode must explicitly be filter or bypass";
                return {};
            }
            if (!profile_.valid()) {
                error = "Register a compatible profile first";
                return {};
            }
            if (!validate_input(arguments[0], error) || !validate_output(arguments[1], error) ||
                !audio_.start_live(arguments[0], arguments[1], profile_, arguments[2] == L"bypass", error))
                return {};
            job_error_.clear();
            return status();
        }
        if (request.verb == "deleteProfile") {
            if (!require_count(0) || !idle(error))
                return {};
            if (!nk::win::delete_profile(profile_path_, error))
                return {};
            SecureZeroMemory(&profile_, sizeof(profile_));
            job_error_.clear();
            job_message_ = "Profile deleted; no secure disk erasure is claimed";
            return status();
        }
        if (request.verb == "enrollFile") {
            if (!require_count(1) || !idle(error) || !allowed_file(arguments[0], error))
                return {};
            const auto path = arguments[0];
            start_job("enroll", [this, path] {
                JobResult result;
                nk::AudioData data;
                WipeSamples wipe_input{data.samples};
                if (!nk::read_wav(path, data, result.error))
                    return result;
                const double seconds = data.sample_rate ? double(data.samples.size()) / data.sample_rate : 0;
                if (seconds < 10 || seconds > 30) {
                    result.error = "Reference WAV must contain 10 to 30 seconds of your voice only";
                    return result;
                }
                std::vector<float> mono;
                WipeSamples wipe_mono{mono};
                if (!resample_cancelable(data, mono)) {
                    result.message = "Enrollment cancelled";
                    return result;
                }
                return enroll_samples(mono);
            });
            return status();
        }
        if (request.verb == "processFile") {
            if (!require_count(2) || !idle(error) || !allowed_file(arguments[0], error))
                return {};
            if (!profile_.valid()) {
                error = "Register a compatible profile first";
                return {};
            }
            if (!absolute_file(arguments[1]) || GetFileAttributesW(arguments[1].c_str()) != INVALID_FILE_ATTRIBUTES) {
                error = "Choose an absolute, new output WAV path; existing files are never overwritten";
                return {};
            }
            const auto from = arguments[0], to = arguments[1];
            auto anchor = std::shared_ptr<nk::Profile>(new nk::Profile(profile_), [](nk::Profile *profile) {
                SecureZeroMemory(profile, sizeof(*profile));
                delete profile;
            });
            start_job("process", [this, from, to, anchor] {
                JobResult result;
                WipeObject<nk::Profile> wipe_profile{*anchor};
                nk::AudioData data;
                WipeSamples wipe_input{data.samples};
                if (!nk::read_wav(from, data, result.error))
                    return result;
                if (double(data.samples.size()) / data.sample_rate > 600) {
                    result.error = "Offline WAV must be at most 10 minutes for this desktop MVP";
                    return result;
                }
                std::vector<float> mono;
                WipeSamples wipe_mono{mono};
                if (!resample_cancelable(data, mono)) {
                    result.message = "Offline processing cancelled; no output was written";
                    return result;
                }
                nk::Engine engine(nk::Config{}, *anchor);
                engine.reset(1);
                std::vector<float> output(mono.size() + nk::Window, 0);
                WipeSamples wipe_output{output};
                std::array<float, nk::Hop> input{}, hop{};
                WipeObject<decltype(input)> wipe_hop_input{input};
                WipeObject<decltype(hop)> wipe_hop_output{hop};
                nk::FrameResult frame_result{};
                const std::size_t frames = (mono.size() + nk::Hop - 1) / nk::Hop +
                                           (nk::Window - nk::Hop) / nk::Hop;
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    if (cancelled_.load()) {
                        result.message = "Offline processing cancelled; no output was written";
                        engine.reset(0);
                        return result;
                    }
                    input.fill(0);
                    for (std::size_t i = 0; i < nk::Hop && frame * nk::Hop + i < mono.size(); ++i)
                        input[i] = mono[frame * nk::Hop + i];
                    engine.process_hop(input, hop, frame_result);
                    if (!frame_result.valid) {
                        result.error = "MECAG reported an invalid frame; no output was written";
                        engine.reset(0);
                        return result;
                    }
                    for (std::size_t i = 0; i < nk::Hop && frame * nk::Hop + i < output.size(); ++i)
                        output[frame * nk::Hop + i] = hop[i];
                }
                engine.reset(0);
                TemporaryFile temporary{temporary_path(to)};
                if (!nk::write_wav(temporary.path, output.data() + nk::Window - nk::Hop,
                                   mono.size(), nk::SampleRate, result.error))
                    return result;
                if (cancelled_.load()) {
                    result.message = "Offline processing cancelled; no output was written";
                    return result;
                }
                // No REPLACE_EXISTING: protects input and any concurrently
                // created destination even after the initial existence check.
                if (!MoveFileExW(temporary.path.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    result.error = "Unable to commit new output file; existing files are never overwritten";
                    return result;
                }
                temporary.path.clear();
                result.ok = true;
                result.message = "Offline WAV complete; 320-sample algorithm alignment removed; quality is not certified";
                return result;
            });
            return status();
        }
        error = "Unknown command";
        return {};
    }
    std::string smoke() { return status(); }
    void fail_closed() noexcept {
        cancelled_.store(true);
        audio_.stop();
    }
};
void respond(const std::string &id, const std::string &result, const std::string &error) {
    std::cout << "{\"id\":" << quote(id) << ",\"ok\":" << (error.empty() ? "true" : "false");
    if (error.empty())
        std::cout << ",\"result\":" << result;
    else
        std::cout << ",\"error\":" << quote(error);
    std::cout << "}\n" << std::flush;
}
bool protocol_self_test() {
    const auto accepts = [](const std::string &line) { Request request; return parse(line, request); };
    std::wstring value;
    return accepts("NK1\t1\tstatus") && accepts("NK1\t99\tenrollFile\t5rWL6K+VLndhdg==") &&
           !accepts("NK1\t01\tstatus") && !accepts("NK1\t-1\tstatus") &&
           !accepts("NK1\t1\tenrollFile\tYQ=") && !accepts("NK1\t1\tenrollFile\tYR==") &&
           !accepts("NK1\t1\tenrollFile\tAA==") && !accepts("NK1\t1\tenrollFile\t/w==") &&
           !accepts("NK1\t1\tenrollFile\tY Q==") && !accepts("NK1\t1\tenrollFile\tYQ==AAAA") &&
           !accepts("NK1\t1\tstatus\t\t\t\t\t\t") && !accepts(std::string(MaximumLine + 1, 'x')) &&
           decode_argument("5rWL6K+VLndhdg==", value) && value == L"测试.wav" &&
           quote("a\n\"\\") == "\"a\\u000a\\\"\\\\\"";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    std::wstring profile_path = profile_location();
    bool smoke = false, self_test = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--profile" && i + 1 < argc && absolute_file(argv[i + 1]))
            profile_path = argv[++i];
        else if (argument == L"--smoke")
            smoke = true;
        else if (argument == L"--protocol-self-test")
            self_test = true;
        else {
            std::cerr << "Invalid Host argument; use --profile <absolute.nkp>, --smoke or --protocol-self-test\n";
            return 2;
        }
    }
    if (self_test) {
        const bool ok = protocol_self_test();
        respond("0", "{\"passed\":" + std::string(ok ? "true" : "false") + "}", "");
        return ok ? 0 : 3;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        std::cerr << "Unable to initialize Host COM control apartment\n";
        return 2;
    }
    int exit_code = 0;
    try {
        Host host(profile_path);
        if (smoke)
            respond("0", host.smoke(), "");
        else {
            std::string line;
            bool oversized = false, shutdown = false;
            while (!shutdown && read_line(line, oversized)) {
                Request request;
                if (oversized || !parse(line, request)) {
                    respond(request.id, "", "Malformed or oversized NK1 request");
                    continue;
                }
                try {
                    std::string error;
                    const auto result = host.execute(request, shutdown, error);
                    respond(request.id, result, error);
                } catch (const std::exception &exception) {
                    host.fail_closed();
                    respond(request.id, "", exception.what());
                } catch (...) {
                    host.fail_closed();
                    respond(request.id, "", "Native control operation failed");
                }
                if (!std::cout)
                    break;
            }
        }
        // Host destructs and stops/joins all native producers before COM exits.
    } catch (...) {
        std::cerr << "Host initialization failed\n";
        exit_code = 2;
    }
    CoUninitialize();
    return exit_code;
}
#endif
