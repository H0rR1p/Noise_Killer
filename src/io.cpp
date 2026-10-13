#include "noisekiller/io.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace nk {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr std::uint64_t MaxWaveBytes = 256ULL * 1024 * 1024;
unsigned u16(const unsigned char* p) { return p[0] | (unsigned(p[1]) << 8); }
std::uint32_t u32(const unsigned char* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
void put16(std::ostream& stream, unsigned v) {
    const char b[] = {char(v), char(v >> 8)}; stream.write(b, 2);
}
void put32(std::ostream& stream, std::uint32_t v) {
    const char b[] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; stream.write(b, 4);
}
}

bool read_wav(const std::wstring& path, AudioData& audio, std::string& error) {
    audio = AudioData{};
    error.clear();
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) { error = "Cannot open WAV"; return false; }
    stream.seekg(0, std::ios::end);
    const auto file_end = stream.tellg();
    if (file_end < 12 || std::uint64_t(file_end) > MaxWaveBytes) { error = "WAV size out of range (256 MiB maximum)"; return false; }
    stream.seekg(0);
    unsigned char header[12]{};
    stream.read(reinterpret_cast<char*>(header), 12);
    if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) { error = "Only RIFF/WAVE is supported"; return false; }
    const std::uint64_t end = std::uint64_t(u32(header + 4)) + 8;
    if (end > std::uint64_t(file_end) || end < 12) { error = "Truncated RIFF"; return false; }
    unsigned format = 0, channels = 0, rate = 0, bits = 0, block = 0, byte_rate = 0;
    std::uint64_t data_offset = 0;
    std::uint32_t data_size = 0;
    bool have_format = false, have_data = false;
    for (std::uint64_t pos = 12; pos < end;) {
        if (end - pos < 8) { error = "Truncated chunk header"; return false; }
        stream.seekg(std::streamoff(pos));
        unsigned char chunk[8]{};
        stream.read(reinterpret_cast<char*>(chunk), 8);
        const auto size = u32(chunk + 4);
        const std::uint64_t next = pos + 8 + size + (size & 1);
        if (next > end || next < pos) { error = "Truncated chunk or padding"; return false; }
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (have_format || size < 16 || size > 128) { error = "Invalid or duplicate fmt chunk"; return false; }
            unsigned char fmt[128]{};
            stream.read(reinterpret_cast<char*>(fmt), size);
            format = u16(fmt); channels = u16(fmt + 2); rate = u32(fmt + 4);
            byte_rate = u32(fmt + 8); block = u16(fmt + 12); bits = u16(fmt + 14);
            if (format == 0xfffe) {
                const unsigned char suffix[] = {0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                if (size < 40 || u16(fmt + 16) < 22 || std::memcmp(fmt + 28, suffix, 12)) { error = "Unsupported extensible subformat"; return false; }
                format = u32(fmt + 24);
                if (u16(fmt + 18) != bits) { error = "Packed valid-bit extensible PCM is not supported"; return false; }
            }
            have_format = true;
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (have_data) { error = "Duplicate data chunk"; return false; }
            have_data = true; data_offset = pos + 8; data_size = size;
        }
        pos = next;
    }
    if (!have_format || !have_data || channels == 0 || channels > 8 || rate < 8000 || rate > 192000 ||
        !((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)) ||
        block != channels * (bits / 8) || byte_rate != rate * block || data_size == 0 || data_size % block) {
        error = "Invalid format: expected PCM16/24/32 or float32, 8-192 kHz, 1-8 channels"; return false;
    }
    const std::size_t frames = data_size / block;
    audio.samples.resize(frames);
    audio.sample_rate = rate; audio.original_channels = channels;
    stream.seekg(std::streamoff(data_offset));
    unsigned char frame[32]{};
    for (std::size_t i = 0; i < frames; ++i) {
        stream.read(reinterpret_cast<char*>(frame), block);
        if (!stream) { audio = AudioData{}; error = "Truncated samples"; return false; }
        double mono = 0;
        for (unsigned c = 0; c < channels; ++c) {
            const auto* p = frame + c * (bits / 8);
            double value = 0;
            if (format == 3) {
                float sample; std::memcpy(&sample, p, 4); value = sample;
            } else if (bits == 16) {
                const int signed_value = int(u16(p)) - ((p[1] & 0x80) ? 65536 : 0);
                value = signed_value / 32768.0;
            } else if (bits == 24) {
                const int raw = p[0] | (int(p[1]) << 8) | (int(p[2]) << 16);
                value = (raw - ((p[2] & 0x80) ? 16777216 : 0)) / 8388608.0;
            } else {
                const std::uint32_t raw = u32(p);
                const std::int64_t signed_value = std::int64_t(raw) - ((raw & 0x80000000U) ? 4294967296LL : 0);
                value = double(signed_value) / 2147483648.0;
            }
            if (!std::isfinite(value) || std::abs(value) > 4) { audio = AudioData{}; error = "Non-finite or invalid audio sample"; return false; }
            mono += value;
        }
        audio.samples[i] = float(mono / channels);
    }
    return true;
}

bool write_wav(const std::wstring& path, const float* samples, std::size_t count, unsigned rate, std::string& error) {
    error.clear();
    if (!samples || rate < 8000 || rate > 192000 || count > (MaxWaveBytes - 44) / 2) { error = "Invalid WAV output arguments"; return false; }
    for (std::size_t i = 0; i < count; ++i) if (!std::isfinite(samples[i])) { error = "Non-finite output"; return false; }
    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!stream) { error = "Cannot create output WAV"; return false; }
    stream.write("RIFF", 4); put32(stream, std::uint32_t(36 + count * 2)); stream.write("WAVEfmt ", 8);
    put32(stream, 16); put16(stream, 1); put16(stream, 1); put32(stream, rate);
    put32(stream, rate * 2); put16(stream, 2); put16(stream, 16);
    stream.write("data", 4); put32(stream, std::uint32_t(count * 2));
    for (std::size_t i = 0; i < count; ++i) {
        const float sample = std::max(-1.0f, std::min(1.0f, samples[i]));
        const int q = int(std::round(sample < 0 ? sample * 32768 : sample * 32767));
        put16(stream, unsigned(q) & 0xffff);
    }
    if (!stream) { error = "Failed writing WAV"; return false; }
    return true;
}

MonoResampler::MonoResampler(unsigned input_rate, unsigned output_rate) { configure(input_rate, output_rate); }
void MonoResampler::configure(unsigned input_rate, unsigned output_rate) {
    if (input_rate < 8000 || input_rate > 192000 || output_rate < 8000 || output_rate > 192000 ||
        output_rate > input_rate * 8) throw std::invalid_argument("Resampler rate out of range");
    input_rate_ = input_rate; output_rate_ = output_rate;
    const double cutoff = 0.45 * std::min(1.0, double(output_rate) / input_rate);
    for (std::size_t phase = 0; phase < coefficients_.size(); ++phase) {
        double sum = 0;
        for (std::size_t tap = 0; tap < 33; ++tap) {
            const double distance = double(tap) - 16 + double(phase) / coefficients_.size();
            const double window = std::abs(distance) <= 16 ? 0.54 + 0.46 * std::cos(Pi * distance / 16) : 0;
            const double argument = 2 * Pi * cutoff * distance;
            const double coefficient = window * 2 * cutoff * (std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument);
            coefficients_[phase][tap] = float(coefficient); sum += coefficient;
        }
        for (float& coefficient : coefficients_[phase]) coefficient = float(coefficient / sum);
    }
    reset();
}
void MonoResampler::reset() noexcept { history_.fill(0); input_count_ = 0; next_numerator_ = 0; fault_ = false; }
unsigned MonoResampler::delay_input_samples() const noexcept { return input_rate_ == output_rate_ ? 0 : 16; }
std::size_t MonoResampler::push(float sample, float* outputs, std::size_t capacity) noexcept {
    if (!outputs || capacity == 0 || !std::isfinite(sample) || std::abs(sample) > 4 || fault_) { fault_ = true; return 0; }
    if (input_rate_ == output_rate_) { outputs[0] = sample; ++input_count_; return 1; }
    const std::uint64_t index = input_count_++;
    history_[index % history_.size()] = sample;
    std::size_t count = 0;
    while (next_numerator_ / output_rate_ <= index) {
        if (count == capacity) { fault_ = true; return 0; }
        const auto at = next_numerator_ / output_rate_;
        const auto phase = std::size_t((next_numerator_ % output_rate_) * coefficients_.size() / output_rate_);
        float value = 0;
        for (std::size_t tap = 0; tap < 33; ++tap) {
            if (at >= tap && index - (at - tap) < history_.size()) value += history_[(at - tap) % history_.size()] * coefficients_[phase][tap];
        }
        outputs[count++] = value;
        next_numerator_ += input_rate_;
    }
    return count;
}
std::vector<float> resample(const AudioData& audio, unsigned target_rate) {
    MonoResampler converter(audio.sample_rate, target_rate);
    std::vector<float> result;
    result.reserve(audio.samples.size() * std::size_t(target_rate) / audio.sample_rate + 8);
    std::array<float, 8> output{};
    for (float sample : audio.samples) {
        const auto count = converter.push(sample, output.data(), output.size());
        if (converter.faulted()) throw std::runtime_error("Resampler failed");
        result.insert(result.end(), output.begin(), output.begin() + count);
    }
    return result;
}
}
