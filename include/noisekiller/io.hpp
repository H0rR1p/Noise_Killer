#pragma once
#include "noisekiller/core.hpp"
#include <array>
#include <string>
#include <vector>

namespace nk {
struct AudioData {
    unsigned sample_rate = 0;
    unsigned original_channels = 0;
    std::vector<float> samples;
};
bool read_wav(const std::wstring& path, AudioData& audio, std::string& error);
bool write_wav(const std::wstring& path, const float* samples, std::size_t count,
               unsigned sample_rate, std::string& error);

// Causal 33-tap polyphase FIR; allocated once, no allocation during push.
class MonoResampler {
public:
    MonoResampler(unsigned input_rate = SampleRate, unsigned output_rate = SampleRate);
    void configure(unsigned input_rate, unsigned output_rate);
    void reset() noexcept;
    std::size_t push(float sample, float* outputs, std::size_t capacity) noexcept;
    unsigned delay_input_samples() const noexcept;
    bool faulted() const noexcept { return fault_; }
private:
    unsigned input_rate_ = SampleRate, output_rate_ = SampleRate;
    std::uint64_t input_count_ = 0, next_numerator_ = 0;
    bool fault_ = false;
    std::array<float, 64> history_{};
    std::array<std::array<float, 33>, 256> coefficients_{};
};
std::vector<float> resample(const AudioData& audio, unsigned target_rate = SampleRate);
}
