#include "noisekiller/driver_model.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool good, const char* message) {
    if (!good) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
using nk::driver_test::TransportModel;
using nk::driver_test::Peer;
using nk::driver_test::Result;
constexpr Peer Owner{100, 12345, 700, 2};
NK_CONFIGURE config(std::uint64_t epoch = 1) {
    NK_CONFIGURE request{};
    request.header = {sizeof(request), NK_DRIVER_VERSION, NK_OP_CONFIGURE, epoch};
    request.sample_rate = NK_DRIVER_RATE;
    request.channels = NK_DRIVER_CHANNELS;
    request.bits = NK_DRIVER_BITS;
    request.hop_samples = NK_DRIVER_HOP;
    request.max_queue_hops = NK_DRIVER_QUEUE_HOPS;
    return request;
}
NK_SUBMIT block(std::uint64_t sequence = 0, std::uint64_t epoch = 1,
                std::uint64_t captured = 0, std::int16_t value = 1000) {
    NK_SUBMIT request{};
    request.header = {sizeof(request), NK_DRIVER_VERSION, NK_OP_SUBMIT, epoch};
    request.sequence = sequence;
    request.first_sample_index = sequence * NK_DRIVER_HOP;
    request.capture_qpc = captured;
    request.sample_count = NK_DRIVER_HOP;
    std::fill_n(request.pcm, NK_DRIVER_HOP, value);
    return request;
}
void start(TransportModel& model) {
    expect(model.grant_from_test_authority(Owner, 0) == Result::Ok, "test authority grants owner");
    expect(model.configure(Owner, config(), 0) == Result::Ok, "exact supported format configures");
}
void wire_and_owner_tests() {
    TransportModel model(1000000);
    start(model);
    expect(model.grant_from_test_authority(Owner, 0) == Result::Busy, "only one owner");
    Peer forged = Owner; ++forged.process;
    expect(model.submit(forged, block(), sizeof(NK_SUBMIT), 0) == Result::Unauthorized, "matching-session other process cannot write");
    forged = Owner; ++forged.creation_time;
    expect(model.submit(forged, block(), sizeof(NK_SUBMIT), 0) == Result::Unauthorized, "reused process ID cannot write");
    forged = Owner; ++forged.handle;
    expect(model.submit(forged, block(), sizeof(NK_SUBMIT), 0) == Result::Unauthorized, "different file-object handle cannot write");
    forged = Owner; ++forged.session;
    expect(model.submit(forged, block(), sizeof(NK_SUBMIT), 0) == Result::Unauthorized, "other login session cannot write");
    auto m = block();
    expect(model.submit(Owner, m, sizeof(m) - 1, 0) == Result::InvalidMessage, "truncated request rejects");
    expect(model.submit(Owner, m, sizeof(m) + 1, 0) == Result::InvalidMessage, "oversize request rejects");
    ++m.header.version;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::InvalidMessage, "wrong protocol version rejects");
    m = block(); m.sample_count = UINT32_MAX;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::InvalidMessage, "unbounded sample count rejects");
    m = block(); m.reserved = 1;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::InvalidMessage, "reserved flags reject");
    m = block(); m.header.epoch = 2;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::OldEpoch, "unconfigured epoch rejects");
    m = block(); m.sequence = 2;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::WrongSequence, "missing sequence rejects");
    m = block(); m.first_sample_index = UINT64_MAX;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::Discontinuous, "overflowing position rejects");
    m = block(); m.capture_qpc = 1;
    expect(model.submit(Owner, m, sizeof(m), 0) == Result::Discontinuous, "future capture clock rejects");
    expect(model.submit(Owner, block(), sizeof(m), 0) == Result::Ok, "valid block is accepted");
    expect(model.submit(Owner, block(), sizeof(m), 0) == Result::WrongSequence, "duplicate block cannot replay");
    expect(model.stats().accepted_blocks == 1 && model.stats().rejected_blocks == 14, "fixed counters record admission/rejection");
}
void clock_and_multiconsumer_tests() {
    TransportModel model(1000000);
    start(model);
    auto a = model.open_consumer(), b = model.open_consumer();
    std::array<std::int16_t, NK_DRIVER_HOP> ya{}, yb{};
    expect(model.submit(Owner, block(), sizeof(NK_SUBMIT), 0) == Result::Ok, "first queued hop");
    expect(model.stats().endpoint_sample_position == 0, "submit never advances endpoint clock");
    expect(model.advance_clock(10000) == Result::Ok, "clock advances without consumers");
    expect(model.read(a, ya.data(), ya.size()) == NK_DRIVER_HOP, "first consumer sees elapsed zero prefill");
    expect(std::all_of(ya.begin(), ya.end(), [](auto v) { return v == 0; }), "prefill is silence, not test tone");
    expect(model.read(b, yb.data(), yb.size()) == NK_DRIVER_HOP, "second cursor independently sees same prefill");
    expect(model.advance_clock(20000) == Result::Ok, "clock continues independently");
    expect(model.read(a, ya.data(), ya.size()) == NK_DRIVER_HOP, "consumer A gets submitted hop");
    expect(model.read(b, yb.data(), yb.size()) == NK_DRIVER_HOP, "consumer B does not compete for queue index");
    expect(ya == yb && ya.front() == 1000, "multi consumer samples are identical");
    model.close_consumer(a);
    expect(model.advance_clock(30000) == Result::Ok, "one app closing does not stop clock");
    expect(model.read(b, yb.data(), yb.size()) == NK_DRIVER_HOP, "other app continues");
    expect(std::all_of(yb.begin(), yb.end(), [](auto v) { return v == 0; }), "underrun zero fills instead of repeating last block");
    expect(model.stats().active_consumers == 1, "consumer count updates independently");
    expect(model.advance_clock(29000) == Result::BadClock, "QPC regression rejected");
}
void bounded_queue_and_epoch_tests() {
    TransportModel model(1000000);
    start(model);
    for (std::uint64_t i = 0; i < 3; ++i)
        expect(model.submit(Owner, block(i), sizeof(NK_SUBMIT), 0) == Result::Ok, "bounded lead admits hop");
    expect(model.submit(Owner, block(3), sizeof(NK_SUBMIT), 0) == Result::Full, "queue cannot grow beyond clock horizon");
    expect(model.stats().queued_samples == 3 * NK_DRIVER_HOP, "queued PCM bounded");
    expect(model.stop(Owner) == Result::Ok, "stop invalidates old PCM");
    expect(model.stats().queued_samples == 0 && model.stats().header.epoch == 0, "stop clears memory timeline and epoch");
    expect(model.configure(Owner, config(), 0) == Result::OldEpoch, "same owner cannot reuse old epoch");
    expect(model.configure(Owner, config(2), 0) == Result::Ok, "new epoch restarts safely");
    expect(model.submit(Owner, block(0, 1), sizeof(NK_SUBMIT), 0) == Result::OldEpoch, "old in-flight message cannot restore writing");
    model.disconnect(Owner);
    expect(!model.stats().owner_active, "process/handle cleanup revokes producer");
    expect(model.submit(Owner, block(0, 2), sizeof(NK_SUBMIT), 0) == Result::Unauthorized, "closed owner cannot write");
    Peer replacement = Owner; ++replacement.creation_time; ++replacement.handle;
    expect(model.grant_from_test_authority(replacement, 10000) == Result::Ok, "fresh owner can be admitted after disconnect");
    expect(model.configure(replacement, config(), 10000) == Result::Ok, "fresh file-object scopes fresh epoch counter");
    expect(model.submit(Owner, block(), sizeof(NK_SUBMIT), 10000) == Result::Unauthorized, "old process cannot exploit new epoch");
}
void watchdog_and_late_tests() {
    TransportModel model(1000000);
    start(model);
    auto c = model.open_consumer();
    expect(model.submit(Owner, block(), sizeof(NK_SUBMIT), 0) == Result::Ok, "initial block before hang");
    expect(model.advance_clock(100000) == Result::Ok, "clock continues through producer hang");
    expect(model.stats().watchdog_stops == 1 && !model.stats().owner_active && model.stats().queued_samples == 0,
        "watchdog checks real sample progress, revokes owner, clears queued PCM");
    std::array<std::int16_t, NK_DRIVER_HOP> output{};
    expect(model.read(c, output.data(), output.size()) == NK_DRIVER_HOP, "hung producer still allows capture progress");
    expect(std::all_of(output.begin(), output.end(), [](auto v) { return v == 0; }), "watchdog has no stale tail replay");
    expect(model.stats().consumer_overruns == 1, "lagging consumer has bounded history rather than indefinite backlog");
    TransportModel late(1000000);
    start(late);
    expect(late.submit(Owner, block(), sizeof(NK_SUBMIT), 20000) == Result::Late, "late packet is never replayed at a later endpoint time");
    TransportModel invalid(0);
    expect(invalid.grant_from_test_authority(Owner, 0) == Result::BadClock, "invalid clock cannot begin session");
}
void fractional_clock_tests() {
    TransportModel model(1000003);
    expect(model.advance_clock(0) == Result::Ok, "initialize fractional QPC");
    for (std::uint64_t i = 1; i <= 1000003; i += 1000)
        expect(model.advance_clock(i) == Result::Ok, "fractional sample clock remains finite");
    expect(model.advance_clock(1000003) == Result::Ok, "whole second after fractional ticks");
    expect(model.stats().endpoint_sample_position == NK_DRIVER_RATE, "fractional remainder prevents cumulative clock drift");
}
}
int main() {
    wire_and_owner_tests(); clock_and_multiconsumer_tests();
    bounded_queue_and_epoch_tests(); watchdog_and_late_tests(); fractional_clock_tests();
    if (failures) { std::cerr << failures << " driver model checks failed\n"; return 1; }
    std::cout << "DriverContract v1 user-mode MODEL tests passed; no WaveRT endpoint/driver validation performed.\n";
    return 0;
}
