#pragma once
#include "noisekiller/core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <memory>
#include <string>

namespace nk {
namespace win {
constexpr unsigned RenderQueueHops = 4;
constexpr unsigned MaximumRenderFrames = 8192;
inline std::wstring routing_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}
// UI routing hints, NOT authentication. The actual endpoint data flow/state is
// checked again before WASAPI initialization. A renamed/unknown cable is not
// routed to a default speaker and requires a supported explicit selection.
inline bool known_cable_render(const std::wstring &name, const std::wstring &manufacturer) {
    const auto n = routing_lower(name), m = routing_lower(manufacturer);
    return n.find(L"cable") != std::wstring::npos && n.find(L"input") != std::wstring::npos &&
           (n.find(L"vb-audio") != std::wstring::npos || m.find(L"vb-audio") != std::wstring::npos);
}
inline bool virtual_capture_hint(const std::wstring &name, const std::wstring &manufacturer) {
    const auto n = routing_lower(name), m = routing_lower(manufacturer);
    return n.find(L"noisekiller") != std::wstring::npos || n.find(L"virtual") != std::wstring::npos ||
           n.find(L"虚拟") != std::wstring::npos || n.find(L"立体声混音") != std::wstring::npos ||
           n.find(L"环回") != std::wstring::npos || n.find(L"stereo mix") != std::wstring::npos ||
           n.find(L"what u hear") != std::wstring::npos || n.find(L"wave out mix") != std::wstring::npos ||
           n.find(L"voicemeeter") != std::wstring::npos || n.find(L"loopback") != std::wstring::npos ||
           (n.find(L"cable") != std::wstring::npos && n.find(L"output") != std::wstring::npos) ||
           n.find(L"vb-audio") != std::wstring::npos || m.find(L"vb-audio") != std::wstring::npos;
}
inline std::wstring cable_family_key(const std::wstring &name, const wchar_t *direction) {
    const auto n = routing_lower(name);
    const auto end = n.find(direction), begin = n.find(L"cable");
    if (end == std::wstring::npos || begin == std::wstring::npos || end <= begin)
        return {};
    auto key = n.substr(begin, end - begin);
    while (!key.empty() && std::iswspace(key.back()))
        key.pop_back();
    return key;
}
inline bool paired_cable_endpoints(const std::wstring &render_name, const std::wstring &render_manufacturer,
                                   const std::wstring &capture_name,
                                   const std::wstring &capture_manufacturer) {
    if (!known_cable_render(render_name, render_manufacturer))
        return false;
    const auto c = routing_lower(capture_name), m = routing_lower(capture_manufacturer);
    if (c.find(L"output") == std::wstring::npos ||
        (c.find(L"vb-audio") == std::wstring::npos && m.find(L"vb-audio") == std::wstring::npos))
        return false;
    const auto a = cable_family_key(render_name, L"input"), b = cable_family_key(capture_name, L"output");
    return !a.empty() && a == b;
}
enum class RenderQueueResult { Ok, Empty, Full, Invalid, OldEpoch };
class RenderHopQueue {
  public:
    // Control-path reset only: producer and consumer must be joined first.
    void reset(std::uint64_t epoch) noexcept {
        for (auto &slot : slots_)
            slot.fill(0);
        produced_.store(0);
        consumed_.store(0);
        epoch_.store(epoch);
    }
    RenderQueueResult push(const float *samples, std::size_t count, std::uint64_t epoch) noexcept {
        if (!epoch || epoch != epoch_.load(std::memory_order_acquire))
            return RenderQueueResult::OldEpoch;
        if (!samples || count != Hop)
            return RenderQueueResult::Invalid;
        for (std::size_t i = 0; i < Hop; ++i)
            if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 4)
                return RenderQueueResult::Invalid;
        const auto write = produced_.load(std::memory_order_relaxed),
                   read = consumed_.load(std::memory_order_acquire);
        if (write - read >= RenderQueueHops)
            return RenderQueueResult::Full;
        std::copy_n(samples, Hop, slots_[write % RenderQueueHops].begin());
        produced_.store(write + 1, std::memory_order_release);
        return RenderQueueResult::Ok;
    }
    RenderQueueResult pop(std::array<float, Hop> &samples, std::uint64_t epoch) noexcept {
        if (!epoch || epoch != epoch_.load(std::memory_order_acquire)) {
            samples.fill(0);
            return RenderQueueResult::OldEpoch;
        }
        const auto read = consumed_.load(std::memory_order_relaxed),
                   write = produced_.load(std::memory_order_acquire);
        if (read == write) {
            samples.fill(0);
            return RenderQueueResult::Empty;
        }
        samples = slots_[read % RenderQueueHops];
        consumed_.store(read + 1, std::memory_order_release);
        return RenderQueueResult::Ok;
    }
    unsigned queued_samples() const noexcept {
        const auto read = consumed_.load(std::memory_order_acquire),
                   write = produced_.load(std::memory_order_acquire);
        return static_cast<unsigned>(
                   std::min<std::uint64_t>(write >= read ? write - read : 0, RenderQueueHops)) *
               Hop;
    }

  private:
    std::array<std::array<float, Hop>, RenderQueueHops> slots_{};
    std::atomic<std::uint64_t> produced_{0}, consumed_{0}, epoch_{0};
};
// Smooth, limited asynchronous clock correction. Positive ppm consumes input
// faster; it never inserts/deletes whole PCM samples or grows the FIFO. These
// gains/bounds are engineering candidates, not measured device guarantees.
class RenderDriftController {
  public:
    void reset() noexcept {
        filtered_ = 2.0 * Hop;
        integral_ = ppm_ = 0;
        fault_ = false;
    }
    double update(unsigned queued_samples, double elapsed_seconds) noexcept {
        if (!std::isfinite(elapsed_seconds) || elapsed_seconds <= 0 || elapsed_seconds > 1 ||
            queued_samples > (RenderQueueHops + 1) * Hop) {
            fault_ = true;
            return ppm_;
        }
        const double dt = std::min(elapsed_seconds, .1);
        filtered_ += (queued_samples - filtered_) * dt / (.5 + dt);
        const double error = filtered_ - 2.0 * Hop;
        integral_ = std::clamp(integral_ + error * dt * .5, -400.0, 400.0);
        const double desired = std::clamp(error + integral_, -500.0, 500.0);
        ppm_ += std::clamp(desired - ppm_, -40.0 * dt, 40.0 * dt);
        return ppm_;
    }
    bool faulted() const noexcept { return fault_; }
    double ppm() const noexcept { return ppm_; }

  private:
    double filtered_ = 2.0 * Hop, integral_ = 0, ppm_ = 0;
    bool fault_ = false;
};
// Causal 33-tap FIR, fixed 256 phases and 64-sample history. Configuration is
// slow-path; push/set_ppm allocate nothing. Continuously changing the phase
// increment provides bounded asynchronous adaptation without reconfiguration.
class AsyncRenderResampler {
  public:
    bool configure(unsigned output_rate) noexcept {
        if (output_rate < 8000 || output_rate > 192000) {
            fault_ = true;
            return false;
        }
        output_rate_ = output_rate;
        constexpr double pi = 3.14159265358979323846;
        const double cutoff = .45 * std::min(1.0, double(output_rate) / SampleRate);
        for (std::size_t phase = 0; phase < coefficients_.size(); ++phase) {
            double sum = 0;
            for (std::size_t tap = 0; tap < 33; ++tap) {
                const double distance = double(tap) - 16 + double(phase) / coefficients_.size();
                const double window = std::abs(distance) <= 16 ? .54 + .46 * std::cos(pi * distance / 16) : 0;
                const double argument = 2 * pi * cutoff * distance;
                const double coefficient =
                    window * 2 * cutoff * (std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument);
                coefficients_[phase][tap] = float(coefficient);
                sum += coefficient;
            }
            if (!std::isfinite(sum) || std::abs(sum) < 1e-12) {
                fault_ = true;
                return false;
            }
            for (auto &coefficient : coefficients_[phase])
                coefficient = float(coefficient / sum);
        }
        reset();
        return true;
    }
    void reset() noexcept {
        history_.fill(0);
        input_count_ = 0;
        next_input_ = 0;
        ppm_ = 0;
        fault_ = false;
    }
    bool set_ppm(double ppm) noexcept {
        if (!std::isfinite(ppm) || std::abs(ppm) > 500) {
            fault_ = true;
            return false;
        }
        ppm_ = ppm;
        return true;
    }
    std::size_t push(float sample, float *outputs, std::size_t capacity) noexcept {
        if (fault_ || !output_rate_ || !outputs || !capacity || !std::isfinite(sample) ||
            std::abs(sample) > 4 || input_count_ > UINT64_C(1000000000000)) {
            fault_ = true;
            return 0;
        }
        const auto index = input_count_++;
        history_[index % history_.size()] = sample;
        std::size_t count = 0;
        while (next_input_ <= double(index)) {
            if (count == capacity) {
                fault_ = true;
                return 0;
            }
            const auto at = static_cast<std::uint64_t>(std::floor(next_input_));
            const auto phase =
                std::min<std::size_t>(255, static_cast<std::size_t>((next_input_ - double(at)) * 256));
            double value = 0;
            for (std::size_t tap = 0; tap < 33; ++tap)
                if (at >= tap && index - (at - tap) < history_.size())
                    value += history_[(at - tap) % history_.size()] * coefficients_[phase][tap];
            if (!std::isfinite(value)) {
                fault_ = true;
                return 0;
            }
            outputs[count++] = float(value);
            next_input_ += double(SampleRate) / output_rate_ * (1 + ppm_ * 1e-6);
        }
        return count;
    }
    bool faulted() const noexcept { return fault_; }
    unsigned delay_input_samples() const noexcept { return 16; }

  private:
    unsigned output_rate_ = 0;
    std::uint64_t input_count_ = 0;
    double next_input_ = 0, ppm_ = 0;
    bool fault_ = false;
    std::array<float, 64> history_{};
    std::array<std::array<float, 33>, 256> coefficients_{};
};
struct RenderFillResult {
    unsigned underflow_frames = 0;
    bool fault = false;
};
class RenderSamplePump {
  public:
    bool configure(unsigned rate, std::uint64_t epoch) noexcept {
        if (!epoch)
            return false;
        epoch_ = epoch;
        queue_.reset(epoch);
        current_.fill(0);
        pending_.fill(0);
        input_cursor_ = Hop;
        pending_count_ = pending_cursor_ = 0;
        return converter_.configure(rate);
    }
    void clear() noexcept {
        queue_.reset(0);
        current_.fill(0);
        pending_.fill(0);
        converter_.reset();
        epoch_ = 0;
        input_cursor_ = Hop;
        pending_count_ = pending_cursor_ = 0;
    }
    RenderQueueResult enqueue(const float *samples, std::size_t count, std::uint64_t epoch) noexcept {
        return queue_.push(samples, count, epoch);
    }
    // Consumer-only: input_cursor_ is render-thread state. The native adapter
    // publishes an atomic cached occupancy for UI; producer never calls this.
    unsigned queued_input_samples() const noexcept {
        return queue_.queued_samples() + static_cast<unsigned>(Hop - input_cursor_);
    }
    unsigned delay_input_samples() const noexcept { return converter_.delay_input_samples(); }
    RenderFillResult fill(float *output, unsigned frames, double ppm) noexcept {
        RenderFillResult result;
        if (!output || frames > MaximumRenderFrames || !epoch_ || !converter_.set_ppm(ppm)) {
            result.fault = true;
            return result;
        }
        for (unsigned frame = 0; frame < frames; ++frame) {
            bool available = true;
            while (pending_cursor_ == pending_count_) {
                if (input_cursor_ == Hop) {
                    const auto status = queue_.pop(current_, epoch_);
                    if (status == RenderQueueResult::Empty) {
                        available = false;
                        break;
                    }
                    if (status != RenderQueueResult::Ok) {
                        result.fault = true;
                        return result;
                    }
                    input_cursor_ = 0;
                }
                pending_count_ = converter_.push(current_[input_cursor_++], pending_.data(), pending_.size());
                pending_cursor_ = 0;
                if (converter_.faulted()) {
                    result.fault = true;
                    return result;
                }
            }
            if (available)
                output[frame] = pending_[pending_cursor_++];
            else {
                // The first Empty fixes this whole remaining render buffer to
                // zero. Do not consume a newly arriving producer hop mid-fill
                // and then erase its FIR/pending samples at the boundary.
                result.underflow_frames = frames - frame;
                std::fill(output + frame, output + frames, 0.0f);
                break;
            }
        }
        // There is no last-block replay across a real underrun. Reset only at
        // a discontinuity, never for a drift correction or ordinary hop edge.
        if (result.underflow_frames) {
            converter_.reset();
            pending_.fill(0);
            pending_cursor_ = pending_count_ = 0;
        }
        return result;
    }

  private:
    RenderHopQueue queue_;
    AsyncRenderResampler converter_;
    std::uint64_t epoch_ = 0;
    std::array<float, Hop> current_{};
    std::array<float, 16> pending_{};
    std::size_t input_cursor_ = Hop, pending_count_ = 0, pending_cursor_ = 0;
};
struct RenderSnapshot {
    bool running = false, fault = false;
    unsigned sample_rate = 0, channels = 0, period_frames = 0, buffer_frames = 0, padding_frames = 0;
    unsigned queued_input_samples = 0, queue_high_water = 0, resampler_delay_samples = 16;
    std::uint64_t underflow_events = 0, underflow_frames = 0, rendered_frames = 0, warmup_zero_frames = 0;
    double drift_ppm = 0, compute_us = 0, maximum_compute_us = 0;
    bool client3 = false;
    std::int64_t shared_fallback_hresult = 0;
    std::string error;
};
class WasapiRenderAdapter {
  public:
    WasapiRenderAdapter();
    ~WasapiRenderAdapter();
    WasapiRenderAdapter(const WasapiRenderAdapter &) = delete;
    WasapiRenderAdapter &operator=(const WasapiRenderAdapter &) = delete;
    bool start(const std::wstring &device_id, std::uint64_t epoch, std::string &error);
    bool enqueue(const float *samples, std::size_t count, std::uint64_t epoch) noexcept;
    bool healthy() const noexcept;
    // Control-only: join the sole enqueue producer before stop/clear. The
    // adapter joins its render consumer, then clears queue/resampler/epoch.
    void stop() noexcept;
    RenderSnapshot snapshot() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace win
} // namespace nk
