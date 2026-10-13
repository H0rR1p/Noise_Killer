#include "noisekiller/windows.hpp"
#include "noisekiller/render.hpp"
#ifdef _WIN32
#include <windows.h>
#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
unsigned tests = 0, failures = 0, skipped = 0;
void check(bool passed, const char *name) {
    ++tests;
    if (!passed)
        ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
}
void skip(const char *name) {
    ++skipped;
    std::cout << "SKIP " << name << '\n';
}
bool stopped_without_capture(const nk::win::AudioSession &session) {
    const auto snapshot = session.snapshot();
    return snapshot.state == nk::win::SessionState::Stopped && snapshot.captured_packets == 0 &&
           snapshot.processed_hops == 0 && snapshot.enrollment_samples == 0;
}
void audio_initialization_and_rejection() {
    nk::win::AudioSession session;
    check(stopped_without_capture(session), "AudioSession construction does not capture or process PCM");
    const auto initial = session.snapshot();
    check(initial.input_rate == 0 && initial.input_channels == 0 && initial.capture_period_frames == 0,
          "Construction does not initialize a physical capture stream");
    check(initial.dropped_packets == 0 && initial.discontinuities == 0 && initial.deadline_misses == 0 &&
              initial.queue_high_water == 0 && initial.compute_us == 0 && initial.engine_compute_us == 0,
          "Construction contains no hidden worker activity");

    // These requests are rejected before device selection. Never supply a
    // real endpoint ID: this test is not permission to capture ambient audio.
    for (unsigned seconds : {0u, 9u, 31u, std::numeric_limits<unsigned>::max()}) {
        std::string error;
        check(!session.start_enrollment(L"", seconds, error) && !error.empty(),
              "Invalid enrollment duration is rejected with a reason");
        check(stopped_without_capture(session), "Rejected duration leaves capture stopped with no PCM");
    }
    std::string error;
    check(!session.start_enrollment(L"", 15, error) && !error.empty(),
          "Empty physical device ID is rejected before WASAPI Start");
    check(stopped_without_capture(session), "Read-only enumeration cannot start enrollment implicitly");

    const nk::Profile unregistered{};
    check(!session.start_live(L"", L"", unregistered, false, error) && !error.empty(),
          "Unregistered filtering fails before microphone or driver activation");
    check(stopped_without_capture(session), "Failed unregistered filtering remains stopped and empty");
    check(!session.start_live(L"", L"", unregistered, true, error) && !error.empty(),
          "Unregistered explicit bypass cannot evade the profile/start contract");
    check(stopped_without_capture(session), "Rejected bypass does not capture raw microphone audio");

    std::vector<float> recording;
    check(!session.take_enrollment(recording, error) && !error.empty() && recording.empty(),
          "Incomplete enrollment cannot expose nonexistent PCM");
    session.stop();
    const auto stopped = session.snapshot();
    session.stop();
    check(stopped_without_capture(session), "Repeated stop is safe and retains no captured PCM");
    check(stopped.epoch != 0 && session.snapshot().epoch != 0,
          "Stop establishes a nonzero invalidated session epoch without starting a stream");
}
void unstarted_render_adapter() {
    nk::win::WasapiRenderAdapter render;
    std::string error;
    const auto initial = render.snapshot();
    check(!initial.running && !initial.fault && initial.rendered_frames == 0 && initial.sample_rate == 0,
          "Render adapter construction does not open a stream or default speaker");
    check(!render.start(L"", 1, error) && !error.empty() && !render.healthy(),
          "Missing explicit playback selection is rejected before WASAPI Start");
    std::array<float, nk::Hop> silence{};
    check(!render.enqueue(silence.data(), silence.size(), 1) && render.snapshot().rendered_frames == 0,
          "Unstarted adapter cannot queue or submit even a zero block");
    render.stop();
    render.stop();
    check(!render.healthy() && !render.snapshot().running, "Repeated render stop remains fail-closed");
}
void read_only_route_inventory() {
    std::string error;
    std::vector<nk::win::Device> devices;
    if (nk::win::enumerate_render_devices(devices, error)) {
        bool hints = true;
        for (const auto &device : devices)
            hints &= device.virtual_cable == nk::win::known_cable_render(device.name, device.manufacturer);
        check(hints, "Read-only render inventory marks supported cable hints without opening a stream");
    } else
        skip(
            "Audio service not available for read-only inventory; no microphone or renderer start attempted");
}
} // namespace
int main() {
    audio_initialization_and_rejection();
    unstarted_render_adapter();
    read_only_route_inventory();
    std::cout << tests << " Windows control-path checks, " << failures << " failures, " << skipped
              << " environmental scenarios skipped. No microphone/render stream was started; no driver "
                 "installed; no PCM captured or submitted. This is not actual virtual-cable "
                 "quality, real-time, pairing, or consumer acceptance.\n";
    return failures ? 1 : 0;
}
#endif
