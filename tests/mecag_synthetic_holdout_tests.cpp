#include "noisekiller/core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Frozen before conditional-reference MECAG results were inspected. Independent
// generated vowels, not real speech, not an enrollment prefix, and not a quality
// acceptance corpus. Truth sources are used ONLY by the evaluation below.
// Related design research: Hu/Wang, "Monaural Speech Separation" (NIPS 2002),
// https://papers.nips.cc/paper/2314-monaural-speech-separation.pdf . This test
// varies resolved harmonic, AM/onset and phase evidence; no paper code is copied.
namespace {
constexpr char Protocol[] = "synthetic-holdout-v1-2026-10-13";
constexpr double Pi = 3.14159265358979323846;
constexpr std::size_t Delay = nk::Window - nk::Hop;
constexpr unsigned EnrollmentSeed = 0x41C64E6D;
constexpr unsigned TargetSeed = 0x7F4A7C15;
constexpr unsigned ImpostorSeed = 0xA341316C;
constexpr unsigned SameF0Seed = 0xC8013EA4;
constexpr double MixPresence = .70, MixGain = .35, SirImprovementDb = 3;
constexpr double SoloPresence = .85, SoloGain = .60, AbsentPowerRatio = .01;
constexpr unsigned Duration = 4;
unsigned mandatory_checks = 0, mandatory_failures = 0;
unsigned exploratory_checks = 0, exploratory_failures = 0;
bool trace = false, protocol_only = false;

double db(double power) { return 10 * std::log10(std::max(power, 1e-30)); }
std::size_t at(double seconds) { return std::size_t(std::llround(seconds * nk::SampleRate)); }
unsigned next(unsigned& state) { state = state * 1664525u + 1013904223u; return state; }
double random01(unsigned& state) { return double(next(state)) / 4294967296.0; }
std::uint64_t samples_hash(const std::vector<float>& samples) {
    std::uint64_t hash = 14695981039346656037ull;
    for (float sample : samples) {
        std::uint32_t bits = 0; std::memcpy(&bits, &sample, sizeof(bits));
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (bits >> shift) & 255u; hash *= 1099511628211ull;
        }
    }
    return hash;
}
struct Span { std::size_t begin, end; };
struct Voice {
    unsigned seed;
    double base_f0, pitch_rate, pitch_phase, pitch_depth;
    double syllable_rate, syllable_phase, phone_duration;
    std::array<double, 3> formant_shift{};
    bool unregistered_vowel = false, pitch_crossing = false;
};
struct Phone { std::array<double, 3> center, width; };
// Multiple phonetic states, not a static formant with only sinusoidal wobble.
constexpr std::array<Phone, 4> Phones{{
    {{{500, 1450, 2520}}, {{170, 300, 430}}},
    {{{710, 1810, 2860}}, {{190, 320, 450}}},
    {{{365, 2130, 3210}}, {{150, 350, 460}}},
    {{{610, 1120, 2380}}, {{180, 280, 420}}}
}};
std::vector<float> generate(unsigned seconds, const Voice& voice) {
    std::vector<float> samples(std::size_t(seconds) * nk::SampleRate);
    std::array<double, 37> phases{};
    unsigned random = voice.seed;
    for (unsigned harmonic = 1; harmonic <= 36; ++harmonic) phases[harmonic] = 2 * Pi * random01(random);
    const auto phone_count = std::size_t(std::ceil(seconds / voice.phone_duration)) + 2;
    std::vector<unsigned> phones(phone_count);
    for (auto& phone : phones) phone = (next(random) >> 24) % Phones.size();
    double phase = 0, square = 0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double time = double(n) / nk::SampleRate;
        const double f0 = voice.pitch_crossing
            ? 240 - 110 * time / seconds + 6 * std::sin(2 * Pi * time * .43 + voice.pitch_phase)
            : voice.base_f0 * (1 + voice.pitch_depth * std::sin(2 * Pi * time * voice.pitch_rate + voice.pitch_phase)
                + .025 * std::sin(2 * Pi * time * (voice.pitch_rate * 2.17) + .31));
        phase += 2 * Pi * f0 / nk::SampleRate;
        const auto index = std::min(std::size_t(time / voice.phone_duration), phone_count - 2);
        const double fraction = time / voice.phone_duration - index;
        // Smooth phoneme transition in the final 80 ms of each phone.
        double blend = std::clamp((fraction - (1 - .08 / voice.phone_duration))
            / (.08 / voice.phone_duration), 0.0, 1.0);
        blend = blend * blend * (3 - 2 * blend);
        std::array<double, 3> center{}, width{};
        for (unsigned formant = 0; formant < 3; ++formant) {
            center[formant] = Phones[phones[index]].center[formant] * (1 - blend)
                + Phones[phones[index + 1]].center[formant] * blend + voice.formant_shift[formant];
            width[formant] = Phones[phones[index]].width[formant] * (1 - blend)
                + Phones[phones[index + 1]].width[formant] * blend;
            center[formant] += 18 * std::sin(2 * Pi * time * (.37 + .11 * formant) + .27 * formant);
            if (voice.unregistered_vowel) center[formant] += (formant == 0 ? 230 : -380)
                + 95 * std::sin(2 * Pi * time * .61 + .8 * formant);
        }
        const double amplitude = .25 + .75 * std::pow(std::sin(Pi * time * voice.syllable_rate
            + voice.syllable_phase), 2);
        double value = 0;
        for (unsigned harmonic = 1; harmonic <= 36; ++harmonic) {
            const double frequency = f0 * harmonic;
            if (frequency >= 7800) break;
            double envelope = .12 / std::pow(double(harmonic), 1.1);
            const double gains[3] = {1.0, .55, .22};
            for (unsigned formant = 0; formant < 3; ++formant)
                envelope += gains[formant] * std::exp(-std::pow((frequency - center[formant]) / width[formant], 2));
            value += envelope * std::sin(harmonic * phase + phases[harmonic]);
        }
        samples[n] = float(.055 * amplitude * value + .00008 * (random01(random) - .5));
        square += double(samples[n]) * samples[n];
    }
    // Equalize clean-source RMS before any case-specific mixer gain/onset/pause.
    // One fixed whole-source scalar, not truth-dependent algorithm post-gain.
    const double scale = .065 / std::sqrt(square / samples.size());
    for (auto& sample : samples) sample = float(sample * scale);
    return samples;
}
Voice enrollment_voice() { return {EnrollmentSeed, 178, .53, .13, .13, 2.41, .17, .43, {0, 0, 0}}; }
Voice target_voice() { return {TargetSeed, 170, .79, .41, .12, 2.07, .73, .37, {0, 0, 0}}; }
Voice impostor_voice() { return {ImpostorSeed, 275, .61, 1.17, .09, 1.73, 1.21, .49, {620, 820, 520}}; }

struct Case {
    std::string name;
    std::vector<float> target, interferer, mixture;
    std::vector<Span> active, absent;
    bool solo = false, no_target = false, report_only = false;
};
Case make_case(std::string name, double gain, const Voice& target_parameters = target_voice(),
               const Voice& other_parameters = impostor_voice()) {
    Case result;
    result.name = std::move(name);
    result.target = generate(Duration, target_parameters);
    result.interferer = generate(Duration, other_parameters);
    for (auto& sample : result.interferer) sample = float(sample * gain);
    result.active = {{nk::Window, result.target.size()}};
    return result;
}
void mix(Case& value) {
    value.mixture.resize(value.target.size());
    for (std::size_t n = 0; n < value.target.size(); ++n)
        value.mixture[n] = value.target[n] + value.interferer[n]; // Clipping is never hidden.
}
bool inside(std::size_t sample, const std::vector<Span>& spans) {
    for (const auto& span : spans) if (sample >= span.begin && sample < span.end) return true;
    return false;
}
bool complete_window(std::size_t end, const Span& span) {
    return end >= nk::Window && end - nk::Window >= span.begin && end <= span.end;
}
void check(bool value, const Case& scenario, const std::string& criterion, bool safety = false) {
    const bool exploratory = scenario.report_only && !safety;
    if (exploratory) { ++exploratory_checks; exploratory_failures += !value; }
    else { ++mandatory_checks; mandatory_failures += !value; }
    std::cout << (exploratory ? "EXPLORATORY_" : "MANDATORY_") << (value ? "PASS " : "FAIL ")
        << scenario.name << '/' << criterion << '\n';
}
struct Metric {
    double target_gain = 0, other_gain = 0, correlation = 0, gram_ratio = 0;
    double target_energy = 0, other_energy = 0, unexplained_output = 0;
    bool identifiable = false;
};
Metric projection(const Case& scenario, const std::vector<float>& output, bool processed,
                  const std::vector<Span>& spans) {
    double tt = 0, ii = 0, ti = 0, yt = 0, yi = 0, yy = 0;
    for (std::size_t n = 0; n < scenario.mixture.size(); ++n) if (inside(n, spans)) {
        const double t = scenario.target[n], i = scenario.interferer[n];
        const double y = processed ? output[n + Delay] : scenario.mixture[n];
        tt += t * t; ii += i * i; ti += t * i; yt += y * t; yi += y * i; yy += y * y;
    }
    Metric result; result.target_energy = tt; result.other_energy = ii;
    if (scenario.solo) {
        result.identifiable = tt > 1e-10; result.gram_ratio = 1;
        result.target_gain = tt > 0 ? yt / tt : 0;
    } else {
        const double determinant = tt * ii - ti * ti;
        result.correlation = tt * ii > 0 ? ti / std::sqrt(tt * ii) : 0;
        result.gram_ratio = tt * ii > 0 ? determinant / (tt * ii) : 0;
        result.identifiable = tt > 1e-10 && ii > 1e-10 && result.gram_ratio > .90;
        if (result.identifiable) {
            result.target_gain = (yt * ii - yi * ti) / determinant;
            result.other_gain = (yi * tt - yt * ti) / determinant;
        }
    }
    result.unexplained_output = std::max(0.0, yy - result.target_gain * yt - result.other_gain * yi);
    return result;
}
double sir_improvement(const Metric& input, const Metric& output) {
    return db((output.target_gain * output.target_gain + 1e-30) / (output.other_gain * output.other_gain + 1e-30))
        - db((input.target_gain * input.target_gain + 1e-30) / (input.other_gain * input.other_gain + 1e-30));
}
double absence_ratio(const Case& scenario, const std::vector<float>& output) {
    double input_energy = 0, output_energy = 0;
    for (std::size_t n = 0; n < scenario.mixture.size(); ++n) if (inside(n, scenario.absent)) {
        input_energy += double(scenario.mixture[n]) * scenario.mixture[n];
        output_energy += double(output[n + Delay]) * output[n + Delay];
    }
    return input_energy > 1e-10 ? output_energy / input_energy : std::numeric_limits<double>::infinity();
}
void evaluate(const nk::Profile& profile, const Case& scenario) {
    nk::Engine engine(nk::Config{}, profile);
    std::array<float, nk::Hop> input{}, output_hop{};
    std::vector<float> output; output.reserve(scenario.mixture.size() + nk::Window);
    std::vector<unsigned> active(scenario.active.size()), present(scenario.active.size());
    std::array<std::uint64_t, 4> owners{};
    std::array<std::uint64_t, 8> missing{};
    unsigned accepted = 0, committed = 0, false_target_frames = 0, invalid = 0, reset = 0;
    double max_us = 0;
    bool bounded = true, finite = true, mapping = true, no_input_clip = true;
    for (float value : scenario.mixture) no_input_clip &= std::isfinite(value) && std::abs(value) <= 1;
    const std::size_t hops = scenario.mixture.size() / nk::Hop;
    nk::FrameResult result;
    for (std::size_t frame = 0; frame < hops + nk::Window / nk::Hop; ++frame) {
        for (std::size_t n = 0; n < nk::Hop; ++n) {
            const auto index = frame * nk::Hop + n;
            input[n] = index < scenario.mixture.size() ? scenario.mixture[index] : 0;
        }
        engine.process_hop(input, output_hop, result);
        output.insert(output.end(), output_hop.begin(), output_hop.end());
        if (frame >= hops) continue;
        invalid += !result.valid; reset += result.state_reset;
        accepted += result.feedback_accepted; committed += result.feedback_committed;
        max_us = std::max(max_us, result.compute_us);
        bounded &= result.groups <= nk::MaxTracks && result.tracks <= nk::MaxTracks
            && result.basis_count <= nk::MaxBasis && result.beam_states <= nk::MaxBeam && result.solver_steps <= 3;
        mapping &= result.algorithm_delay_samples == Delay
            && result.output_begin_sample == std::int64_t(result.input_end_sample) - std::int64_t(nk::Window);
        for (float value : output_hop) finite &= std::isfinite(value) && std::abs(value) <= 1;
        for (std::size_t span = 0; span < scenario.active.size(); ++span)
            if (complete_window(result.input_end_sample, scenario.active[span])) {
                ++active[span]; present[span] += result.target_present;
            }
        for (const auto& span : scenario.absent)
            if (complete_window(result.input_end_sample, span)) false_target_frames += result.target_present;
        for (unsigned group = 0; group < result.groups; ++group) {
            ++owners[unsigned(result.ownership[group])];
            ++missing[unsigned(result.identity[group].missing_reason)];
            if (trace) std::cout << "HOLDOUT_CANDIDATE case=" << scenario.name << " frame=" << frame
                << " group=" << group << " f0=" << result.grouping[group].f0
                << " identity_valid=" << result.identity[group].valid
                << " identity_score=" << result.identity[group].relative_score
                << " identity_rel=" << result.identity[group].reliability
                << " features=" << result.identity[group].supported_features
                << " reason=" << unsigned(result.identity[group].missing_reason)
                << " owner=" << unsigned(result.ownership[group]) << '\n';
        }
    }
    unsigned total_active = 0, total_present = 0;
    for (std::size_t span = 0; span < active.size(); ++span) { total_active += active[span]; total_present += present[span]; }
    const double presence = total_active ? double(total_present) / total_active : 0;
    const auto before = projection(scenario, output, false, scenario.active);
    const auto after = projection(scenario, output, true, scenario.active);
    const double absent = scenario.absent.empty() ? 0 : absence_ratio(scenario, output);
    std::cout << "HOLDOUT_SUMMARY case=" << scenario.name << " report_only=" << scenario.report_only
        << " active_frames=" << total_active << " present_frames=" << total_present << " presence=" << presence
        << " input_target_gain=" << before.target_gain << " input_interferer_gain=" << before.other_gain
        << " target_gain=" << after.target_gain << " interferer_gain=" << after.other_gain
        << " sir_improvement_db=" << sir_improvement(before, after)
        << " gram_determinant_ratio=" << after.gram_ratio << " source_correlation=" << after.correlation
        << " unexplained_output_energy=" << after.unexplained_output
        << " absent_energy_db=" << db(absent) << " false_target_frames=" << false_target_frames
        << " feedback_accepted=" << accepted << " feedback_committed=" << committed
        << " invalid=" << invalid << " reset=" << reset << " max_compute_us=" << max_us
        << " ownership_U_T_I_N=" << owners[0] << ',' << owners[1] << ',' << owners[2] << ',' << owners[3]
        << " missing_reasons=";
    for (std::size_t reason = 0; reason < missing.size(); ++reason) std::cout << (reason ? "," : "") << missing[reason];
    std::cout << '\n';
    check(no_input_clip && finite && bounded && mapping && invalid == 0 && reset == 0,
        scenario, "finite_bounded_fixed_timeline_no_clip_or_reset", true);
    if (scenario.no_target) {
        check(absent <= AbsentPowerRatio, scenario, "target_absent_at_least_20db_energy_suppression");
        return;
    }
    check(before.identifiable && after.identifiable, scenario, "independent_groundtruth_gram_ratio_above_090");
    check(presence >= (scenario.solo ? SoloPresence : MixPresence), scenario, "target_presence");
    check(after.target_gain >= (scenario.solo ? SoloGain : MixGain), scenario, "gain_sensitive_target_retention");
    if (!scenario.solo) check(sir_improvement(before, after) >= SirImprovementDb, scenario, "sir_improvement_at_least_3db");
    if (!scenario.absent.empty()) check(absent <= AbsentPowerRatio, scenario, "absence_not_stale_target_replay");
    if (scenario.active.size() > 1) for (std::size_t span = 0; span < scenario.active.size(); ++span) {
        const std::vector<Span> selected{scenario.active[span]};
        const auto span_before = projection(scenario, output, false, selected);
        const auto span_after = projection(scenario, output, true, selected);
        const double span_presence = active[span] ? double(present[span]) / active[span] : 0;
        std::cout << "HOLDOUT_ACTIVE_SPAN case=" << scenario.name << " span=" << span
            << " frames=" << active[span] << " presence=" << span_presence
            << " target_gain=" << span_after.target_gain << " interferer_gain=" << span_after.other_gain
            << " sir_improvement_db=" << sir_improvement(span_before, span_after)
            << " gram_determinant_ratio=" << span_after.gram_ratio << '\n';
        const auto prefix = "span_" + std::to_string(span) + '/';
        check(span_presence >= MixPresence, scenario, prefix + "target_presence");
        check(span_after.identifiable && span_after.target_gain >= MixGain, scenario, prefix + "target_retention");
        check(sir_improvement(span_before, span_after) >= SirImprovementDb, scenario, prefix + "sir_improvement_3db");
    }
}
std::vector<Case> cases() {
    std::vector<Case> result;
    auto solo = make_case("dev_solo_new_phone_sequence", 0); solo.solo = true; result.push_back(std::move(solo));
    auto negative = make_case("dev_only_different_f0_impostor", .6);
    std::fill(negative.target.begin(), negative.target.end(), 0.f);
    negative.no_target = true; negative.active.clear(); negative.absent = {{nk::Window, negative.target.size()}};
    result.push_back(std::move(negative));
    auto same_parameters = target_voice(); same_parameters.seed = SameF0Seed;
    same_parameters.formant_shift = {620, 820, 520}; same_parameters.phone_duration = .47;
    same_parameters.syllable_rate = 1.89; same_parameters.syllable_phase = 1.07;
    auto same_f0 = make_case("dev_only_same_f0_impostor", .6, target_voice(), same_parameters);
    std::fill(same_f0.target.begin(), same_f0.target.end(), 0.f);
    same_f0.no_target = true; same_f0.active.clear(); same_f0.absent = {{nk::Window, same_f0.target.size()}};
    result.push_back(std::move(same_f0));
    result.push_back(make_case("dev_mix_gain_030", .3));
    result.push_back(make_case("dev_mix_gain_060", .6));
    result.push_back(make_case("dev_mix_gain_120", 1.2));
    auto onset = make_case("dev_impostor_starts_first", .6);
    const auto begin = at(.65);
    std::fill(onset.target.begin(), onset.target.begin() + begin, 0.f);
    onset.active = {{begin + nk::Window, onset.target.size()}}; onset.absent = {{nk::Window, begin}};
    result.push_back(std::move(onset));
    auto recovery = make_case("dev_target_pause_return", .6);
    const auto away = at(1.1), back = at(1.75);
    std::fill(recovery.target.begin() + away, recovery.target.begin() + back, 0.f);
    recovery.active = {{nk::Window, away}, {back + nk::Window, recovery.target.size()}};
    recovery.absent = {{away + at(.15), back}}; result.push_back(std::move(recovery));
    auto crossing_parameters = impostor_voice(); crossing_parameters.seed = 0xAD90777D;
    crossing_parameters.pitch_crossing = true;
    auto crossing = make_case("hard_holdout_crossing_f0", .6, target_voice(), crossing_parameters);
    crossing.report_only = true; result.push_back(std::move(crossing));
    auto outside_parameters = target_voice(); outside_parameters.seed = 0x9E3779B9;
    outside_parameters.base_f0 = 193; outside_parameters.unregistered_vowel = true;
    auto outside = make_case("hard_holdout_unregistered_vowel", .6, outside_parameters);
    outside.report_only = true; result.push_back(std::move(outside));
    auto unknown = make_case("hard_unknown_broadband_no_target", 0);
    std::fill(unknown.target.begin(), unknown.target.end(), 0.f);
    unsigned noise = 0x243F6A88;
    for (std::size_t n = 0; n < unknown.interferer.size(); ++n) {
        const double amplitude = .03 + .04 * std::pow(std::sin(2 * Pi * double(n) / nk::SampleRate * .41), 2);
        unknown.interferer[n] = float(amplitude * (2 * random01(noise) - 1));
    }
    unknown.no_target = true; unknown.report_only = true;
    unknown.active.clear(); unknown.absent = {{nk::Window, unknown.target.size()}};
    result.push_back(std::move(unknown));
    for (auto& value : result) mix(value);
    return result;
}
int run() {
    std::cout << std::setprecision(9) << "HOLDOUT_PROTOCOL name=" << Protocol
        << " algorithm_header=" << nk::AlgorithmVersion << " config=" << std::hex << nk::Config{}.fingerprint() << std::dec
        << " feature_schema=" << nk::FeatureSchema << " sample_rate=" << nk::SampleRate
        << " delay=" << Delay << " register_seconds=12 eval_seconds=" << Duration
        << " enroll_seed=" << EnrollmentSeed << " target_seed=" << TargetSeed
        << " impostor_seed=" << ImpostorSeed << " same_f0_impostor_seed=" << SameF0Seed
        << " mandatory_developer_cases=8 hard_report_only_cases=3"
        << " mix_presence_min=.70 mix_target_gain_min=.35 sir_improvement_min_db=3"
        << " solo_presence_min=.85 solo_gain_min=.60 absent_power_max=.01 gram_min=.90"
        << " gt_sent_to_engine=false human_speech_acceptance=false\n";
    auto reference = generate(12, enrollment_voice());
    auto scenarios = cases();
    std::cout << "HOLDOUT_AUDIO_HASH reference_fnv1a64=" << std::hex << samples_hash(reference) << std::dec << '\n';
    for (const auto& scenario : scenarios)
        std::cout << "HOLDOUT_AUDIO_HASH case=" << scenario.name << " report_only=" << scenario.report_only
            << " target_fnv1a64=" << std::hex << samples_hash(scenario.target)
            << " interferer_fnv1a64=" << samples_hash(scenario.interferer)
            << " mixture_fnv1a64=" << samples_hash(scenario.mixture) << std::dec << '\n';
    if (protocol_only) return 0;
    nk::Profile profile; std::string error;
    Case enrollment_control; enrollment_control.name = "enrollment_control";
    const bool enrolled = nk::enroll(reference.data(), reference.size(), profile, error) && profile.valid();
    check(enrolled, enrollment_control, "independent_12_second_source_in_memory", true);
    if (!enrolled) { std::cerr << "Enrollment rejected: " << error << '\n'; return 1; }
    const auto anchor = profile.anchor_hash;
    for (const auto& scenario : scenarios) evaluate(profile, scenario);
    nk::Engine silence_engine(nk::Config{}, profile);
    std::array<float, nk::Hop> zeros{}, output{}; nk::FrameResult frame;
    bool quiet = true;
    for (unsigned n = 0; n < 50; ++n) {
        silence_engine.process_hop(zeros, output, frame);
        quiet &= frame.valid && !frame.target_present && !frame.feedback_accepted;
        for (float value : output) quiet &= value == 0;
    }
    check(quiet, enrollment_control, "silence_exact_zero_no_fake_target", true);
    check(profile.valid() && profile.anchor_hash == anchor, enrollment_control, "enrollment_anchor_immutable", true);
    std::cout << "HOLDOUT_TOTAL mandatory_checks=" << mandatory_checks << " mandatory_failures=" << mandatory_failures
        << " exploratory_checks=" << exploratory_checks << " exploratory_failures=" << exploratory_failures
        << " exploratory_failure_is_not_quality_pass=true"
        << " synthetic_only=true human_quality_accepted=false cable_latency_accepted=false mvp_accepted=false\n";
    return mandatory_failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    for (int n = 1; n < argc; ++n) {
        if (std::string(argv[n]) == "--trace") trace = true;
        if (std::string(argv[n]) == "--protocol-only") protocol_only = true;
    }
    try { return run(); }
    catch (const std::exception& error) { std::cerr << "Holdout exception: " << error.what() << '\n'; return 1; }
}
