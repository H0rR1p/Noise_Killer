#pragma once
#include "noisekiller/core.hpp"
#include "noisekiller/profile_store.hpp"
#include "noisekiller/render.hpp"
#include <memory>
#include <string>
#include <vector>

namespace nk {
namespace win {
struct Device {
    std::wstring id, name, manufacturer;
    bool virtual_microphone = false;
    bool virtual_cable = false;
};
bool enumerate_capture_devices(std::vector<Device> &devices, std::string &error);
bool enumerate_render_devices(std::vector<Device> &devices, std::string &error);
enum class SessionState { Stopped, Starting, Enrolling, Filtering, Bypass, EnrollmentReady, Fault };
struct AudioSnapshot {
    SessionState state = SessionState::Stopped;
    unsigned input_rate = 0, input_channels = 0, capture_period_frames = 0;
    std::uint64_t epoch = 0, captured_packets = 0, processed_hops = 0;
    std::uint64_t dropped_packets = 0, discontinuities = 0, deadline_misses = 0;
    unsigned enrollment_samples = 0, enrollment_goal = 0;
    unsigned queue_high_water = 0;
    float target_confidence = 0, residual_ratio = 0;
    double compute_us = 0, maximum_compute_us = 0, engine_compute_us = 0;
    RenderSnapshot output;
    std::string error;
};
// No capture is started by construction or enumeration. Every session requires
// an explicit user command. Physical capture owns a copy-only SPSC producer;
// decoding, resampling and MECAG live on another thread. A separate renderer
// consumes a bounded output queue into the explicitly selected virtual cable.
class AudioSession {
  public:
    AudioSession();
    ~AudioSession();
    AudioSession(const AudioSession &) = delete;
    AudioSession &operator=(const AudioSession &) = delete;
    bool start_enrollment(const std::wstring &device_id, unsigned seconds, std::string &error);
    bool start_live(const std::wstring &device_id, const std::wstring &render_device_id,
                    const Profile &profile, bool explicit_bypass, std::string &error);
    void stop() noexcept;
    AudioSnapshot snapshot() const;
    bool take_enrollment(std::vector<float> &samples, std::string &error);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace win
} // namespace nk
