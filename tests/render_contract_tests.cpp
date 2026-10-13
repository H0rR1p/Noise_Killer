#include "noisekiller/render.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace {
thread_local bool measure_allocations = false;
std::atomic<unsigned> allocations{0};
unsigned tests = 0, failures = 0;
void check(bool passed, const char *name) {
    ++tests;
    if (!passed)
        ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
}
using namespace nk::win;
void routing_tests() {
    check(known_cable_render(L"CABLE Input (VB-Audio Virtual Cable)", L""),
          "VB-CABLE playback input is a supported routing hint");
    check(!known_cable_render(L"CABLE Output (VB-Audio Virtual Cable)", L"VB-Audio"),
          "Recording output is never selected as playback input");
    check(!known_cable_render(L"Speakers (Realtek Audio)", L"Realtek"),
          "Ordinary speaker is not a default virtual route");
    check(!known_cable_render(L"Speakers", L"VB-Audio"),
          "Manufacturer alone cannot turn speakers into a supported cable");
    check(virtual_capture_hint(L"CABLE Output (VB-Audio Virtual Cable)", L""),
          "Cable recording endpoint is excluded from physical capture");
    check(virtual_capture_hint(L"NoiseKiller Microphone", L""),
          "Historical self-built microphone is also excluded from capture");
    check(virtual_capture_hint(L"麦克风阵列 (网易虚拟音频设备)", L"网易"),
          "Localized virtual microphone is excluded from physical inputs");
    check(virtual_capture_hint(L"立体声混音 (Realtek(R) Audio)", L"Realtek") &&
              virtual_capture_hint(L"Stereo Mix (Realtek Audio)", L"Realtek") &&
              virtual_capture_hint(L"What U Hear", L"Creative"),
          "Localized and English system loopback inputs are not physical microphone choices");
    check(!virtual_capture_hint(L"Microphone (USB Audio Device)", L"USB Audio"),
          "Ordinary physical input remains selectable");
    check(paired_cable_endpoints(L"CABLE Input (VB-Audio Virtual Cable)", L"",
                                 L"CABLE Output (VB-Audio Virtual Cable)", L""),
          "Standard cable playback/recording naming pair is recognized");
    check(!paired_cable_endpoints(L"CABLE-A Input (VB-Audio Cable A)", L"VB-Audio",
                                  L"CABLE-B Output (VB-Audio Cable B)", L"VB-Audio"),
          "Different cable families do not get paired");
    check(!paired_cable_endpoints(L"CABLE Input (VB-Audio Virtual Cable)", L"", L"Microphone", L""),
          "Physical microphone cannot substitute for a missing cable output");
}
void queue_tests() {
    RenderHopQueue queue;
    std::array<float, nk::Hop> block{}, out{};
    block.fill(.1f);
    check(queue.push(block.data(), block.size(), 1) == RenderQueueResult::OldEpoch,
          "Inactive queue refuses producer before configuration");
    queue.reset(7);
    check(queue.pop(out, 7) == RenderQueueResult::Empty &&
              std::all_of(out.begin(), out.end(), [](float v) { return v == 0; }),
          "Empty FIFO returns zero, never the previous block");
    check(queue.push(block.data(), block.size() - 1, 7) == RenderQueueResult::Invalid,
          "Partial or oversized producer hop is rejected");
    check(queue.push(block.data(), block.size(), 6) == RenderQueueResult::OldEpoch,
          "Old epoch cannot write current render queue");
    auto invalid = block;
    invalid[3] = std::numeric_limits<float>::quiet_NaN();
    check(queue.push(invalid.data(), invalid.size(), 7) == RenderQueueResult::Invalid,
          "NaN does not enter output FIFO");
    bool fill = true;
    for (unsigned i = 0; i < RenderQueueHops; ++i) {
        block.fill(float(i + 1) / 10);
        fill &= queue.push(block.data(), block.size(), 7) == RenderQueueResult::Ok;
    }
    check(fill && queue.queued_samples() == RenderQueueHops * nk::Hop,
          "Output FIFO has a fixed four-hop capacity");
    check(queue.push(block.data(), block.size(), 7) == RenderQueueResult::Full,
          "Full FIFO refuses current block instead of overwriting consumer audio");
    bool ordered = true;
    for (unsigned i = 0; i < RenderQueueHops; ++i)
        ordered &= queue.pop(out, 7) == RenderQueueResult::Ok &&
                   std::all_of(out.begin(), out.end(), [i](float v) { return v == float(i + 1) / 10; });
    check(ordered, "FIFO preserves all admitted complete hops in order");
    queue.reset(8);
    check(queue.pop(out, 8) == RenderQueueResult::Empty && queue.queued_samples() == 0,
          "New epoch clears old PCM and queue indices");
}
void pump_tests() {
    RenderSamplePump a, b;
    std::array<float, nk::Hop> source{};
    for (unsigned i = 0; i < nk::Hop; ++i)
        source[i] = float(std::sin(i * .05) * .2);
    check(a.configure(48000, 1) && b.configure(48000, 1),
          "Production sample pump configures 16 kHz to 48 kHz causally");
    for (unsigned i = 0; i < 4; ++i) {
        a.enqueue(source.data(), source.size(), 1);
        b.enqueue(source.data(), source.size(), 1);
    }
    std::array<float, 480> whole{}, split{};
    const auto all = a.fill(whole.data(), 480, 0);
    const auto first = b.fill(split.data(), 240, 0), second = b.fill(split.data() + 240, 240, 0);
    check(!all.fault && !all.underflow_frames && !first.underflow_frames && !second.underflow_frames &&
              whole == split,
          "Partial native render buffers preserve the identical resampler/queue timeline");
    check(a.delay_input_samples() == 16, "FIR group delay is declared as sixteen 16-kHz input samples");
    RenderSamplePump empty;
    empty.configure(48000, 2);
    std::array<float, 480> silence{};
    silence.fill(1);
    const auto missing = empty.fill(silence.data(), static_cast<unsigned>(silence.size()), 0);
    check(missing.underflow_frames == 480 && !missing.fault &&
              std::all_of(silence.begin(), silence.end(), [](float v) { return v == 0; }),
          "Consumer clock underrun fills the entire native buffer with zero");
    source.fill(.3f);
    empty.enqueue(source.data(), source.size(), 2);
    empty.enqueue(source.data(), source.size(), 2);
    const auto resumed = empty.fill(silence.data(), static_cast<unsigned>(silence.size()), 0);
    RenderSamplePump fresh_positive;
    fresh_positive.configure(48000, 3);
    fresh_positive.enqueue(source.data(), source.size(), 3);
    fresh_positive.enqueue(source.data(), source.size(), 3);
    std::array<float, 480> positive_reference{};
    const auto reference_positive = fresh_positive.fill(positive_reference.data(), 480, 0);
    std::cout << "SYNTHETIC_UNDERRUN_RESUME first=" << silence.front() << " last=" << silence.back()
              << " underflow=" << resumed.underflow_frames
              << " fresh_equal=" << (silence == positive_reference) << '\n';
    check(!resumed.fault && !resumed.underflow_frames && !reference_positive.fault &&
              !reference_positive.underflow_frames && silence == positive_reference &&
              std::abs(silence.back() - .3f) < .005f,
          "New hop after underrun resumes from cleared FIR history, without old tail replay");
    std::array<float, 1200> tail{};
    const auto shortfall = empty.fill(tail.data(), static_cast<unsigned>(tail.size()), 0);
    source.fill(-.2f);
    empty.enqueue(source.data(), source.size(), 2);
    empty.enqueue(source.data(), source.size(), 2);
    const auto next = empty.fill(silence.data(), static_cast<unsigned>(silence.size()), 0);
    RenderSamplePump fresh_negative;
    fresh_negative.configure(48000, 4);
    fresh_negative.enqueue(source.data(), source.size(), 4);
    fresh_negative.enqueue(source.data(), source.size(), 4);
    std::array<float, 480> negative_reference{};
    const auto reference_negative = fresh_negative.fill(negative_reference.data(), 480, 0);
    std::cout << "SYNTHETIC_NEXT_CALLBACK first=" << silence.front() << " last=" << silence.back()
              << " previous_underflow=" << shortfall.underflow_frames
              << " underflow=" << next.underflow_frames << " fresh_equal=" << (silence == negative_reference)
              << '\n';
    check(shortfall.underflow_frames > 0 && !next.fault && !next.underflow_frames &&
              !reference_negative.fault && !reference_negative.underflow_frames &&
              silence == negative_reference && silence.back() < -.19f,
          "First Empty commits remaining buffer to zero; a later callback consumes the new hop intact");
    empty.clear();
    silence.fill(0);
    check(empty.enqueue(source.data(), source.size(), 2) == RenderQueueResult::OldEpoch &&
              empty.fill(silence.data(), static_cast<unsigned>(silence.size()), 0).fault,
          "Stop clears epoch and prevents retained producer or converter reuse");
    RenderSamplePump invalid;
    check(!invalid.configure(7999, 3) && !invalid.configure(192001, 3) && !invalid.configure(48000, 0),
          "Unsupported endpoint rate or zero epoch is rejected");
}
std::size_t conversion_count(unsigned rate, double ppm) {
    AsyncRenderResampler converter;
    converter.configure(rate);
    converter.set_ppm(ppm);
    std::array<float, 16> output{};
    std::size_t count = 0;
    for (unsigned i = 0; i < nk::SampleRate; ++i)
        count += converter.push(.2f, output.data(), output.size());
    return count;
}
double converted_tone_rms(double frequency) {
    AsyncRenderResampler converter;
    converter.configure(8000);
    std::array<float, 16> output{};
    double energy = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < 8000; ++i) {
        const auto n =
            converter.push(float(.4 * std::sin(2 * 3.14159265358979323846 * frequency * i / nk::SampleRate)),
                           output.data(), output.size());
        if (i > 128)
            for (std::size_t j = 0; j < n; ++j) {
                energy += double(output[j]) * output[j];
                ++count;
            }
    }
    return std::sqrt(energy / count);
}
void resampling_and_drift_tests() {
    for (unsigned rate : {8000u, 16000u, 44100u, 48000u, 96000u, 192000u}) {
        const auto count = conversion_count(rate, 0);
        check(count <= rate + 1 && count + 16 >= rate,
              "Supported endpoint rate keeps bounded one-second sample accounting");
    }
    const auto base = conversion_count(48000, 0), fast = conversion_count(48000, 200),
               slow = conversion_count(48000, -200);
    check(fast < base && slow > base && base - fast < 16 && slow - base < 16,
          "Smooth phase-increment correction changes fractional rate, not complete PCM blocks");
    check(converted_tone_rms(1000) > .2 && converted_tone_rms(6000) < .02,
          "Causal FIR preserves in-band tone and attenuates out-of-band alias before 8-kHz output");
    RenderDriftController drift;
    bool bounded = true;
    double previous = 0;
    for (unsigned i = 0; i < 30000; ++i) {
        const double ppm = drift.update(640, .01);
        bounded &= std::isfinite(ppm) && std::abs(ppm) <= 500 && std::abs(ppm - previous) <= .400001;
        previous = ppm;
    }
    check(bounded && drift.ppm() > 0, "Drift controller has finite 500-ppm bounds and 40-ppm/second slew");
    drift.reset();
    check(drift.ppm() == 0 && !drift.faulted(), "New epoch clears correction/integrator state");
    drift.update(900, .01);
    check(drift.faulted(), "Occupancy outside fixed capacity is a drift fault, not unlimited buffering");
    drift.reset();
    drift.update(100, 2);
    check(drift.faulted(), "Large clock/scheduling gap is rejected rather than hidden as drift");
    AsyncRenderResampler converter;
    converter.configure(48000);
    check(!converter.set_ppm(501) && converter.faulted(), "Out-of-policy resampler correction fails closed");
    converter.configure(48000);
    std::array<float, 16> out{};
    converter.push(std::numeric_limits<float>::infinity(), out.data(), out.size());
    check(converter.faulted(), "Nonfinite renderer input faults before native conversion");
}
void bounded_hot_path_tests() {
    RenderSamplePump pump;
    pump.configure(48000, 10);
    std::array<float, nk::Hop> source{};
    source.fill(.1f);
    std::array<float, 480> output{};
    pump.enqueue(source.data(), source.size(), 10);
    pump.enqueue(source.data(), source.size(), 10);
    RenderDriftController drift;
    bool valid = true;
    allocations = 0;
    measure_allocations = true;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto ppm = drift.update(pump.queued_input_samples(), .01);
        const auto result = pump.fill(output.data(), static_cast<unsigned>(output.size()), ppm);
        valid &= !result.fault;
        valid &= pump.enqueue(source.data(), source.size(), 10) == RenderQueueResult::Ok;
    }
    measure_allocations = false;
    check(valid && allocations.load() == 0, "Production enqueue/FIR/drift/fill hot path allocates no memory");
    RenderHopQueue queue;
    queue.reset(11);
    std::atomic<bool> abort{false}, ordered{true};
    std::atomic<unsigned> received{0};
    constexpr unsigned goal = 10000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::thread producer([&] {
        std::array<float, nk::Hop> data{};
        for (unsigned i = 0; i < goal && !abort.load(); ++i) {
            data.fill(float(i % 100) / 100);
            while (queue.push(data.data(), data.size(), 11) == RenderQueueResult::Full) {
                if (abort.load() || std::chrono::steady_clock::now() > deadline) {
                    abort = true;
                    return;
                }
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&] {
        std::array<float, nk::Hop> data{};
        for (unsigned i = 0; i < goal && !abort.load();) {
            const auto result = queue.pop(data, 11);
            if (result == RenderQueueResult::Empty) {
                if (std::chrono::steady_clock::now() > deadline) {
                    abort = true;
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (result != RenderQueueResult::Ok ||
                !std::all_of(data.begin(), data.end(), [i](float v) { return v == float(i % 100) / 100; })) {
                ordered = false;
                abort = true;
                return;
            }
            ++i;
            ++received;
        }
    });
    producer.join();
    consumer.join();
    check(!abort.load() && ordered.load() && received.load() == goal,
          "Actual fixed SPSC queue preserves ten thousand concurrent producer/consumer hops");
}
} // namespace
void *operator new(std::size_t size) {
    if (measure_allocations)
        ++allocations;
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
int main() {
    routing_tests();
    queue_tests();
    pump_tests();
    resampling_and_drift_tests();
    bounded_hot_path_tests();
    std::cout << tests << " production render-helper checks, " << failures
              << " failures. No WASAPI stream, virtual cable installation, real speech or consumer "
                 "acceptance was exercised.\n";
    return failures ? 1 : 0;
}
