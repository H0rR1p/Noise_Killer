#include "noisekiller/core.hpp"
#include "noisekiller/io.hpp"
#include "noisekiller/profile_store.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
thread_local bool measure_allocations = false;
std::atomic<std::size_t> allocations{0};
constexpr double Pi = 3.14159265358979323846;
unsigned tests = 0, failures = 0;
void check(bool passed, const char* name) {
    ++tests; if (!passed) ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
}
std::vector<float> fixture(unsigned seconds, double base_f0, double formant_shift = 0) {
    std::vector<float> data(std::size_t(seconds) * nk::SampleRate);
    double phase = 0;
    unsigned random = 12345;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const double time = double(i) / nk::SampleRate;
        const double f0 = base_f0 * (1 + 0.1 * std::sin(2 * Pi * time * 0.7));
        phase += 2 * Pi * f0 / nk::SampleRate;
        const double syllable = 0.35 + 0.65 * std::pow(std::sin(Pi * time * 2.8), 2);
        const double formant = 650 + 150 * std::sin(time * 4) + formant_shift;
        double sample = 0;
        for (unsigned harmonic = 1; harmonic <= 28; ++harmonic) {
            const double frequency = f0 * harmonic;
            const double amplitude = 0.2 / harmonic + std::exp(-std::pow((frequency - formant) / 220, 2))
                + 0.4 * std::exp(-std::pow((frequency - (1800 + formant_shift)) / 350, 2));
            sample += amplitude * std::sin(phase * harmonic);
        }
        random = 1664525 * random + 1013904223;
        data[i] = float(0.065 * syllable * sample + 0.0003 * (double(random) / 4294967295.0 - 0.5));
    }
    return data;
}
void hop_at(const std::vector<float>& audio, std::size_t frame, std::array<float, nk::Hop>& input) {
    for (std::size_t i = 0; i < nk::Hop; ++i) {
        const auto at = frame * nk::Hop + i;
        input[i] = at < audio.size() ? audio[at] : 0;
    }
}
void test_resampling() {
    nk::MonoResampler unity;
    float converted[8]{};
    bool exact = true;
    for (int i = 0; i < 1234; ++i) {
        const float sample = float(std::sin(i * 0.03));
        exact = exact && unity.push(sample, converted, 8) == 1 && converted[0] == sample;
    }
    check(exact && unity.delay_input_samples() == 0, "resampling/unity_exact");
    double energy_low = 0, energy_high = 0;
    for (unsigned trial = 0; trial < 2; ++trial) {
        nk::MonoResampler sampler(48000, 16000);
        const double frequency = trial ? 12000 : 1000;
        std::size_t count = 0;
        for (std::size_t i = 0; i < 48000; ++i) {
            const auto produced = sampler.push(float(std::sin(2 * Pi * frequency * i / 48000)), converted, 8);
            count += produced;
            if (i > 200) for (std::size_t j = 0; j < produced; ++j) (trial ? energy_high : energy_low) += converted[j] * converted[j];
        }
        check(count == 16000 && !sampler.faulted() && sampler.delay_input_samples() == 16, trial ? "resampling/high_rate_count" : "resampling/low_rate_count");
    }
    check(energy_low > 1000 && energy_high / energy_low < 0.001, "resampling/anti_alias_12k_to_16k");
    nk::MonoResampler up(16000, 48000);
    check(up.push(0.1f, converted, 8) == 3, "resampling/upsample_count");
    check(up.push(std::numeric_limits<float>::quiet_NaN(), converted, 8) == 0 && up.faulted(), "resampling/nan_fail_closed");
    up.reset(); check(!up.faulted(), "resampling/reset");
}
void test_files(const nk::Profile& profile) {
    const auto root = std::filesystem::current_path() / "test-artifacts";
    std::filesystem::create_directories(root);
    const auto audio_path = root / "roundtrip.wav";
    const auto profile_path = root / "encrypted.nkp";
    auto audio = fixture(1, 180);
    std::string error;
    nk::AudioData read;
    bool passed = nk::write_wav(audio_path.wstring(), audio.data(), audio.size(), nk::SampleRate, error) && nk::read_wav(audio_path.wstring(), read, error);
    double difference = 0;
    if (passed && read.samples.size() == audio.size()) for (std::size_t i = 0; i < audio.size(); ++i) difference = std::max(difference, double(std::abs(audio[i] - read.samples[i])));
    else passed = false;
    check(passed && difference < 0.00007, "wav/pcm16_roundtrip");
    const auto invalid_path = root / "truncated.wav";
    { std::ofstream bad(invalid_path, std::ios::binary); bad.write("RIFF\xff\xff\xff\x7fWAVE", 12); }
    check(!nk::read_wav(invalid_path.wstring(), read, error), "wav/reject_truncated_riff");
    nk::Profile recovered;
    const bool saved = nk::win::save_profile(profile_path.wstring(), profile, error);
    check(saved, "profile/dpapi_save");
    if (!saved) std::cerr << "Profile save diagnostic: " << error << '\n';
    const bool loaded = nk::win::load_profile(profile_path.wstring(), recovered, error);
    check(loaded && recovered.anchor_hash == profile.anchor_hash && recovered.valid(), "profile/dpapi_roundtrip");
    if (!loaded) std::cerr << "Profile load diagnostic: " << error << '\n';
    { std::fstream file(profile_path, std::ios::in | std::ios::out | std::ios::binary); file.seekg(-1, std::ios::end); const int previous = file.get(); file.seekp(-1, std::ios::end); file.put(char(previous ^ 0x80)); }
    check(!nk::win::load_profile(profile_path.wstring(), recovered, error), "profile/reject_tamper");
    check(nk::win::delete_profile(profile_path.wstring(), error), "profile/delete");
    // Explicit synthetic-only CLI fixtures; never a recorded person's voice.
    const auto reference = fixture(12, 165);
    const auto interfering = fixture(3, 270, 1000);
    auto mixture = fixture(3, 165);
    for (std::size_t i = 0; i < mixture.size(); ++i) mixture[i] = std::clamp(mixture[i] + interfering[i] * 0.6f, -1.f, 1.f);
    check(nk::write_wav((root / "synthetic-reference.wav").wstring(), reference.data(), reference.size(), nk::SampleRate, error)
        && nk::write_wav((root / "synthetic-mixture.wav").wstring(), mixture.data(), mixture.size(), nk::SampleRate, error), "fixtures/explicit_synthetic_cli_inputs");
}
int run() {
    nk::Config config;
    check(config.valid(), "config/default_valid");
    config.max_tracks = 5; check(!config.valid(), "config/reject_track_overflow");
    config = nk::Config{}; config.solver_iterations = 4; check(!config.valid(), "config/reject_solver_overflow");
    config = nk::Config{}; config.lookahead = 1; check(!config.valid(), "config/reject_undeclared_future");
    config = nk::Config{};
    nk::Profile profile;
    std::string error;
    auto reference = fixture(12, 165);
    check(!nk::enroll(reference.data(), nk::SampleRate, profile, error), "enroll/reject_short_reference");
    std::vector<float> silence(reference.size());
    check(!nk::enroll(silence.data(), silence.size(), profile, error), "enroll/reject_silence");
    auto nan_reference = reference; nan_reference[200] = std::numeric_limits<float>::quiet_NaN();
    check(!nk::enroll(nan_reference.data(), nan_reference.size(), profile, error), "enroll/reject_nan");
    const bool enrolled = nk::enroll(reference.data(), reference.size(), profile, error);
    check(enrolled && profile.valid() && profile.prototype_count > 1, "enroll/synthetic_multistate_profile");
    if (!enrolled) { std::cerr << "Enrollment fixture rejected: " << error << '\n'; return 1; }
    const auto anchor_hash = profile.anchor_hash;
    nk::Engine engine(config, profile);
    std::array<float, nk::Hop> input{}, output{};
    nk::FrameResult result;
    auto signal = fixture(3, 180);
    signal[1000] += 0.2f;
    std::vector<float> rebuilt;
    for (std::size_t frame = 0; frame < signal.size() / nk::Hop + 4; ++frame) {
        hop_at(signal, frame, input); engine.process_unity_hop(input, output);
        rebuilt.insert(rebuilt.end(), output.begin(), output.end());
    }
    double unity_error = 0;
    const std::size_t delay = nk::Window - nk::Hop;
    for (std::size_t i = 0; i < signal.size(); ++i) unity_error = std::max(unity_error, double(std::abs(signal[i] - rebuilt[i + delay])));
    check(unity_error < 0.0001, "stft/unit_mask_sample_mapping");
    engine.reset(10);
    bool bounded = true, finite = true, valid = true;
    std::uint64_t feedback = 0, committed_feedback = 0;
    bool feedback_delayed = true, mapped_samples = true, objective_descends = true;
    bool bounded_identity_diagnostics = true;
    allocations = 0;
    for (std::size_t frame = 0; frame < 500; ++frame) {
        hop_at(reference, frame, input);
        measure_allocations = true; engine.process_hop(input, output, result); measure_allocations = false;
        bounded = bounded && result.tracks <= nk::MaxTracks && result.groups <= nk::MaxTracks && result.basis_count <= nk::MaxBasis && result.beam_states <= nk::MaxBeam && result.solver_steps <= 3 && result.partial_families <= config.max_tracks && result.ambiguous_peaks <= 40;
        valid = valid && result.valid;
        for (float sample : output) finite = finite && std::isfinite(sample) && std::abs(sample) <= 1;
        for (std::size_t bin = 0; bin < nk::Bins; ++bin) {
            finite = finite && std::isfinite(result.gate[bin]) && result.gate[bin] >= 0 && result.gate[bin] <= 1;
            for (float power : {result.target_power[bin], result.interference_power[bin], result.noise_power[bin], result.unknown_power[bin]}) finite = finite && std::isfinite(power) && power >= 0;
        }
        feedback += result.feedback_accepted;
        committed_feedback += result.feedback_committed;
        if (frame == 0) feedback_delayed &= result.feedback_committed == 0;
        mapped_samples &= result.output_begin_sample == std::int64_t(result.input_end_sample) - std::int64_t(nk::Window)
            && result.output_end_sample - result.output_begin_sample == std::int64_t(nk::Hop);
        objective_descends &= std::isfinite(result.solver_objective_before) && std::isfinite(result.solver_objective_after)
            && result.solver_objective_after <= result.solver_objective_before + 1e-10;
        for (unsigned group = 0; group < result.groups; ++group) {
            const auto& identity = result.identity[group];
            unsigned supported_count = 0;
            bounded_identity_diagnostics &= (identity.supported_feature_mask >> nk::Features) == 0;
            for (unsigned feature = 0; feature < nk::Features; ++feature) {
                const bool supported = (identity.supported_feature_mask & (1u << feature)) != 0;
                supported_count += supported;
                if (!supported) continue;
                bounded_identity_diagnostics &= std::isfinite(identity.observed_features[feature])
                    && std::isfinite(identity.conditioned_mean[feature])
                    && std::isfinite(identity.conditioned_variance[feature]) && identity.conditioned_variance[feature] > 0
                    && std::isfinite(identity.feature_log_ratio[feature])
                    && std::isfinite(identity.observation_variance[feature]) && identity.observation_variance[feature] >= 0;
            }
            bounded_identity_diagnostics &= supported_count == identity.supported_features;
        }
    }
    check(allocations.load() == 0, "worker/no_heap_allocations");
    check(bounded, "worker/candidate_beam_basis_iteration_caps");
    check(valid && finite, "worker/finite_contributions_gate_output");
    check(profile.anchor_hash == anchor_hash && result.anchor_hash == anchor_hash, "feedback/immutable_enrollment_anchor");
    check(feedback > 0, "feedback/qualified_next_frame_update_exercised");
    check(committed_feedback > 0 && feedback_delayed, "feedback/committed_only_in_later_frame");
    check(mapped_samples, "stft/explicit_signed_sample_mapping");
    check(objective_descends, "solver/weighted_history_objective_nonincrease");
    check(bounded_identity_diagnostics, "identity/bounded_finite_explicit_supported_feature_diagnostics");
    input.fill(0); input[42] = std::numeric_limits<float>::quiet_NaN();
    engine.process_hop(input, output, result);
    check(!result.valid && result.state_reset && std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }), "fault/nan_zero_current_output_and_clear_state");
    input.fill(0);
    bool no_stale_state = true;
    for (unsigned frame = 0; frame < 5; ++frame) {
        engine.process_hop(input, output, result);
        no_stale_state &= result.tracks == 0 && result.feedback_committed == 0;
        for (float value : output) no_stale_state &= value == 0;
    }
    check(no_stale_state, "fault/no_stale_tracking_feedback_or_ola_after_nan");
    engine.reset(20); input.fill(0);
    bool clean = true;
    for (unsigned i = 0; i < 20; ++i) { engine.process_hop(input, output, result); for (float value : output) clean = clean && value == 0; }
    check(clean && result.epoch == 20, "epoch/reset_clears_ola_and_old_samples");
    nk::Ablation ablation; ablation.identity = false; engine.reset(30); engine.set_ablation(ablation);
    bool no_fake_identity = true;
    for (unsigned i = 0; i < 100; ++i) { hop_at(reference, i, input); engine.process_hop(input, output, result); no_fake_identity = no_fake_identity && !result.target_present; }
    check(no_fake_identity, "responsibilities/tracking_cannot_replace_disabled_identity");
    nk::Engine first(config, profile), second(config, profile);
    std::array<float, nk::Hop> other{};
    nk::FrameResult other_result;
    bool prefix_equal = true;
    for (unsigned i = 0; i < 200; ++i) {
        hop_at(reference, i, input); first.process_hop(input, output, result); second.process_hop(input, other, other_result);
        prefix_equal = prefix_equal && output == other && result.target_power == other_result.target_power && result.ownership == other_result.ownership;
    }
    check(prefix_equal, "causality/identical_published_prefix");
    for (unsigned mode = 0; mode < 7; ++mode) {
        nk::Ablation disabled;
        switch (mode) {
        case 0: disabled.grouping = false; break;
        case 1: disabled.tracking = false; break;
        case 2: disabled.competition = false; break;
        case 3: disabled.contribution = false; break;
        case 4: disabled.feedback = false; break;
        case 5: disabled.scheduling = false; break;
        default: disabled.reliability = false; break;
        }
        nk::Engine ablated(config, profile); ablated.set_ablation(disabled);
        bool semantics = true, exercised = false;
        for (unsigned frame = 0; frame < 150; ++frame) {
            hop_at(reference, frame, input); ablated.process_hop(input, output, result);
            exercised |= result.groups > 0;
            for (unsigned group = 0; group < result.groups; ++group) {
                if (mode == 0) semantics &= !result.grouping[group].valid && result.grouping[group].missing_reason == nk::MissingReason::DisabledForAblation;
                if (mode == 1) semantics &= !result.association[group].valid && result.association[group].missing_reason == nk::MissingReason::DisabledForAblation;
                if (mode == 6 && result.identity[group].valid) semantics &= result.identity[group].reliability == 1;
            }
            if (mode == 2) semantics &= result.beam_states <= 1;
            if (mode == 3) semantics &= result.basis_count == 0 && result.solver_steps == 0;
            if (mode == 4) semantics &= result.feedback_accepted == 0 && result.feedback_committed == 0;
            if (mode == 5) semantics &= result.solver_steps == config.solver_iterations;
        }
        const char* names[] = {"ablation/grouping_contract", "ablation/tracking_contract", "ablation/independent_ownership", "ablation/direct_contribution", "ablation/feedback_off", "ablation/fixed_solver_budget", "ablation/reliability_off"};
        check(semantics && exercised, names[mode]);
    }
    nk::Config small = config;
    small.max_tracks = small.max_aliases = small.beam_width = small.solver_iterations = small.history_hops = 1;
    small.max_basis = 2;
    nk::Engine limited(small, profile);
    bool respects_caps = true;
    for (unsigned frame = 0; frame < 100; ++frame) {
        hop_at(reference, frame, input); limited.process_hop(input, output, result);
        respects_caps &= result.groups <= 1 && result.tracks <= 1 && result.beam_states <= 1 && result.basis_count <= 2 && result.solver_steps <= 1;
    }
    check(respects_caps, "config/nondefault_runtime_caps");
    auto corrupted = profile; corrupted.prototypes[0].mean[0] += .1f;
    check(!corrupted.valid(), "profile/reject_modified_anchor");
    corrupted = profile; ++corrupted.feature_schema;
    check(!corrupted.valid(), "profile/reject_feature_version");
    corrupted = profile; corrupted.feature_schema = 1;
    check(!corrupted.valid(), "profile/reject_legacy_feature_schema_one");
    corrupted = profile; corrupted.feature_schema = 2;
    check(!corrupted.valid(), "profile/reject_legacy_feature_schema_two");
    check(profile.feature_schema == nk::FeatureSchema && profile.feature_schema > 1,
          "profile/enrollment_declares_current_feature_protocol");
    engine.reset(99); input.fill(0);
    for (unsigned frame = 0; frame < 100; ++frame) engine.process_hop(input, output, result);
    check(result.tracks == 0 && !result.target_present && result.output_rms == 0, "tracking/silence_no_false_target");
    test_resampling(); test_files(profile);
    std::ofstream report("contract-test-results.json", std::ios::binary);
    report << "{\n  \"tests\": " << tests << ",\n  \"failures\": " << failures
           << ",\n  \"synthetic_only\": true,\n  \"quality_acceptance\": false,\n  \"driver_acceptance\": false,\n  \"mvp_accepted\": false\n}\n";
    std::cout << "Tests=" << tests << " failures=" << failures << " (synthetic contracts only; G1/G3 quality and driver acceptance pending)\n";
    return failures ? 1 : 0;
}
}
void* operator new(std::size_t size) {
    if (measure_allocations) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
int main() {
    try { return run(); }
    catch (const std::exception& exception) { std::cerr << "Test exception: " << exception.what() << '\n'; return 1; }
}
