#include "noisekiller/core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Synthetic, gain-sensitive regression only. These generated harmonic sources
// are not human speech, and passing does not imply G1/G3 product acceptance.
// Thresholds were fixed before changing MECAG, not fitted to its output.
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr std::size_t Delay = nk::Window - nk::Hop;
constexpr double MinimumPresence = .70;
constexpr double MinimumTargetGain = .35;
constexpr double MinimumSirImprovementDb = 3.0;
constexpr double MaximumAbsentEnergyRatio = .01; // -20 dB, not amplitude dB.
unsigned checks = 0, failures = 0;
bool full_trace = false;

void check(bool value, const std::string& name) {
    ++checks; failures += !value;
    std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
}
double db(double power_ratio) {
    return 10.0 * std::log10(std::max(power_ratio, 1e-30));
}
// Exactly matches tests/core_tests.cpp for seed 12345 and phase_seed 0,
// including arithmetic and low-level noise; retains the original failure.
std::vector<float> fixture(unsigned seconds, double base_f0,
                           double formant_shift = 0, unsigned phase_seed = 0,
                           unsigned noise_seed = 12345,
                           double syllable_rate = 2.8, double syllable_phase = 0,
                           double pitch_modulation_rate = .7, double pitch_modulation_phase = 0) {
    std::vector<float> data(std::size_t(seconds) * nk::SampleRate);
    std::array<double, 29> harmonic_phase{};
    unsigned phase_random = phase_seed;
    if (phase_seed) {
        for (unsigned harmonic = 1; harmonic <= 28; ++harmonic) {
            phase_random = 1664525 * phase_random + 1013904223;
            harmonic_phase[harmonic] = 2 * Pi * double(phase_random) / 4294967295.0;
        }
    }
    double phase = 0;
    unsigned random = noise_seed;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const double time = double(i) / nk::SampleRate;
        const double f0 = base_f0 * (1 + 0.1 * std::sin(2 * Pi * time * pitch_modulation_rate + pitch_modulation_phase));
        phase += 2 * Pi * f0 / nk::SampleRate;
        const double syllable = 0.35 + 0.65 * std::pow(std::sin(Pi * time * syllable_rate + syllable_phase), 2);
        const double formant = 650 + 150 * std::sin(time * 4) + formant_shift;
        double sample = 0;
        for (unsigned harmonic = 1; harmonic <= 28; ++harmonic) {
            const double frequency = f0 * harmonic;
            const double amplitude = 0.2 / harmonic
                + std::exp(-std::pow((frequency - formant) / 220, 2))
                + 0.4 * std::exp(-std::pow((frequency - (1800 + formant_shift)) / 350, 2));
            sample += amplitude * std::sin(phase * harmonic + harmonic_phase[harmonic]);
        }
        random = 1664525 * random + 1013904223;
        data[i] = float(0.065 * syllable * sample
            + 0.0003 * (double(random) / 4294967295.0 - 0.5));
    }
    return data;
}
struct Span { std::size_t begin = 0, end = 0; };
struct Scenario {
    std::string name;
    std::vector<float> target, interferer, mixture;
    std::vector<Span> active, absent;
    bool target_only = false, interferer_only = false;
};
void build_mixture(Scenario& scenario) {
    scenario.mixture.resize(scenario.target.size());
    for (std::size_t i = 0; i < scenario.mixture.size(); ++i)
        scenario.mixture[i] = std::clamp(scenario.target[i] + scenario.interferer[i], -1.f, 1.f);
}
Scenario overlap(std::string name, double interferer_f0, float interferer_gain,
                 unsigned target_phase = 0, unsigned other_phase = 0,
                 double target_f0 = 165) {
    Scenario scenario;
    scenario.name = std::move(name);
    scenario.target = fixture(3, target_f0, 0, target_phase, target_phase ? 96831 : 12345);
    scenario.interferer = fixture(3, interferer_f0, 1000, other_phase, other_phase ? 38217 : 12345);
    for (auto& sample : scenario.interferer) sample *= interferer_gain;
    // Ignore only the fixed STFT startup window, not failed audible regions.
    scenario.active.push_back({nk::Window, scenario.target.size()});
    build_mixture(scenario);
    return scenario;
}
bool in_spans(std::size_t sample, const std::vector<Span>& spans) {
    for (const auto& span : spans) if (sample >= span.begin && sample < span.end) return true;
    return false;
}
bool window_active(std::size_t input_end, const std::vector<Span>& spans) {
    if (input_end < nk::Window) return false;
    for (const auto& span : spans)
        if (input_end - nk::Window >= span.begin && input_end <= span.end) return true;
    return false;
}
struct Projection {
    double target_gain = 0, interferer_gain = 0, correlation = 0;
    double determinant_ratio = 0, residual_energy = 0;
    bool identifiable = false;
};
Projection projection(const Scenario& scenario, const std::vector<float>& output,
                      bool use_output, const std::vector<Span>& spans) {
    double tt = 0, ii = 0, ti = 0, yt = 0, yi = 0, yy = 0;
    for (std::size_t s = 0; s < scenario.target.size(); ++s) {
        if (!in_spans(s, spans)) continue;
        const double target = scenario.target[s], interferer = scenario.interferer[s];
        const double value = use_output ? output[s + Delay] : scenario.mixture[s];
        tt += target * target; ii += interferer * interferer; ti += target * interferer;
        yt += value * target; yi += value * interferer; yy += value * value;
    }
    Projection result;
    if (scenario.target_only) {
        result.identifiable = tt > 1e-10;
        result.target_gain = tt > 0 ? yt / tt : 0;
        result.determinant_ratio = 1;
        result.residual_energy = std::max(0.0, yy - result.target_gain * yt);
        return result;
    }
    const double determinant = tt * ii - ti * ti;
    result.correlation = tt * ii > 0 ? ti / std::sqrt(tt * ii) : 0;
    result.determinant_ratio = tt * ii > 0 ? determinant / (tt * ii) : 0;
    result.identifiable = tt > 1e-10 && ii > 1e-10 && result.determinant_ratio > .90;
    if (result.identifiable) {
        result.target_gain = (yt * ii - yi * ti) / determinant;
        result.interferer_gain = (yi * tt - yt * ti) / determinant;
        result.residual_energy = std::max(0.0,
            yy - result.target_gain * yt - result.interferer_gain * yi);
    }
    return result;
}
double energy_ratio(const Scenario& scenario, const std::vector<float>& output,
                    const std::vector<Span>& spans) {
    double numerator = 0, denominator = 0;
    for (std::size_t s = 0; s < scenario.mixture.size(); ++s) {
        if (!in_spans(s, spans)) continue;
        numerator += double(output[s + Delay]) * output[s + Delay];
        denominator += double(scenario.mixture[s]) * scenario.mixture[s];
    }
    return denominator > 1e-10 ? numerator / denominator : std::numeric_limits<double>::infinity();
}
bool selected_trace_frame(std::size_t frame) {
    return full_trace || frame == 5 || frame == 30 || frame == 100
        || frame == 150 || frame == 200 || frame == 250;
}
void run_scenario(const nk::Profile& profile, const Scenario& scenario) {
    nk::Engine engine(nk::Config{}, profile);
    std::array<float, nk::Hop> input{}, output_hop{};
    std::vector<float> output;
    output.reserve(scenario.mixture.size() + nk::Window + nk::Hop);
    nk::FrameResult result;
    std::array<std::uint64_t, 4> ownership{};
    std::array<std::uint64_t, 8> reasons{};
    unsigned active_frames = 0, present_frames = 0, feedback = 0, committed = 0;
    unsigned valid_identity = 0, invalid_frames = 0, reset_frames = 0, capacity_frames = 0;
    std::vector<unsigned> span_active(scenario.active.size()), span_present(scenario.active.size());
    bool finite = true, bounded = true, mapped = true;
    const std::size_t total_hops = (scenario.mixture.size() + nk::Hop - 1) / nk::Hop;
    for (std::size_t frame = 0; frame < total_hops + nk::Window / nk::Hop; ++frame) {
        for (std::size_t n = 0; n < nk::Hop; ++n) {
            const auto at = frame * nk::Hop + n;
            input[n] = at < scenario.mixture.size() ? scenario.mixture[at] : 0;
        }
        engine.process_hop(input, output_hop, result);
        output.insert(output.end(), output_hop.begin(), output_hop.end());
        if (frame >= total_hops) continue;
        invalid_frames += !result.valid; reset_frames += result.state_reset;
        capacity_frames += result.budget_exhausted;
        feedback += result.feedback_accepted; committed += result.feedback_committed;
        bounded &= result.groups <= nk::MaxTracks && result.tracks <= nk::MaxTracks
            && result.beam_states <= nk::MaxBeam && result.basis_count <= nk::MaxBasis
            && result.solver_steps <= 3;
        mapped &= result.algorithm_delay_samples == Delay
            && result.output_begin_sample == std::int64_t(result.input_end_sample) - std::int64_t(nk::Window);
        for (float value : output_hop) finite &= std::isfinite(value) && std::abs(value) <= 1;
        if (window_active(result.input_end_sample, scenario.active)) {
            ++active_frames; present_frames += result.target_present;
        }
        if (result.input_end_sample >= nk::Window)
            for (std::size_t span = 0; span < scenario.active.size(); ++span)
                if (result.input_end_sample - nk::Window >= scenario.active[span].begin
                    && result.input_end_sample <= scenario.active[span].end) {
                    ++span_active[span]; span_present[span] += result.target_present;
                }
        for (unsigned group = 0; group < result.groups; ++group) {
            const auto reason = unsigned(result.identity[group].missing_reason);
            if (reason < reasons.size()) ++reasons[reason];
            ++ownership[unsigned(result.ownership[group])];
            valid_identity += result.identity[group].valid;
            if (selected_trace_frame(frame)) {
                std::cout << "CANDIDATE scenario=" << scenario.name << " frame=" << frame
                    << " group=" << group << " f0=" << result.grouping[group].f0
                    << " group_rel=" << result.grouping[group].reliability
                    << " identity_valid=" << result.identity[group].valid
                    << " identity_rel=" << result.identity[group].reliability
                    << " score=" << result.identity[group].relative_score
                    << " proto=" << result.identity[group].prototype
                    << " reason=" << reason << " owner=" << unsigned(result.ownership[group]) << '\n';
            }
        }
    }
    const double presence = active_frames ? double(present_frames) / active_frames : 0;
    const auto before = projection(scenario, output, false, scenario.active);
    const auto after = projection(scenario, output, true, scenario.active);
    const double sir_improvement = db((after.target_gain * after.target_gain + 1e-30)
        / (after.interferer_gain * after.interferer_gain + 1e-30))
        - db((before.target_gain * before.target_gain + 1e-30)
            / (before.interferer_gain * before.interferer_gain + 1e-30));
    const double absent_ratio = scenario.absent.empty() ? 0 : energy_ratio(scenario, output, scenario.absent);
    std::cout << "SUMMARY scenario=" << scenario.name << " active_frames=" << active_frames
        << " present_frames=" << present_frames << " presence=" << presence
        << " input_target_gain=" << before.target_gain << " input_interferer_gain=" << before.interferer_gain
        << " target_gain=" << after.target_gain << " interferer_gain=" << after.interferer_gain
        << " sir_improvement_db=" << sir_improvement << " correlation=" << after.correlation
        << " gram_determinant_ratio=" << after.determinant_ratio
        << " absent_output_energy_db=" << db(absent_ratio)
        << " identity_valid=" << valid_identity << " feedback=" << feedback << " committed=" << committed
        << " invalid=" << invalid_frames << " resets=" << reset_frames << " capacity=" << capacity_frames
        << " ownership_U_T_I_N=" << ownership[0] << ',' << ownership[1] << ',' << ownership[2] << ',' << ownership[3]
        << " identity_reasons=";
    for (std::size_t i = 0; i < reasons.size(); ++i) std::cout << (i ? "," : "") << reasons[i];
    std::cout << '\n';
    check(finite && bounded && mapped && invalid_frames == 0 && reset_frames == 0,
          scenario.name + "/finite_bounded_causal_without_reset");
    if (scenario.interferer_only) {
        check(absent_ratio <= MaximumAbsentEnergyRatio, scenario.name + "/absent_energy_at_least_20db_down");
        return;
    }
    check(before.identifiable && after.identifiable, scenario.name + "/groundtruth_projection_identifiable");
    check(presence >= (scenario.target_only ? .85 : MinimumPresence), scenario.name + "/target_presence");
    check(after.target_gain >= (scenario.target_only ? .60 : MinimumTargetGain), scenario.name + "/gain_sensitive_target_retention");
    if (!scenario.target_only)
        check(sir_improvement >= MinimumSirImprovementDb, scenario.name + "/sir_improvement_at_least_3db");
    if (!scenario.absent.empty())
        check(absent_ratio <= MaximumAbsentEnergyRatio, scenario.name + "/absence_not_stale_target_replay");
    if (scenario.active.size() > 1) {
        for (std::size_t span = 0; span < scenario.active.size(); ++span) {
            const std::vector<Span> isolated{scenario.active[span]};
            const auto input_projection = projection(scenario, output, false, isolated);
            const auto output_projection = projection(scenario, output, true, isolated);
            const double span_presence = span_active[span] ? double(span_present[span]) / span_active[span] : 0;
            const double span_sir = db((output_projection.target_gain * output_projection.target_gain + 1e-30)
                / (output_projection.interferer_gain * output_projection.interferer_gain + 1e-30))
                - db((input_projection.target_gain * input_projection.target_gain + 1e-30)
                    / (input_projection.interferer_gain * input_projection.interferer_gain + 1e-30));
            std::cout << "ACTIVE_SPAN scenario=" << scenario.name << " span=" << span
                << " active_frames=" << span_active[span] << " present_frames=" << span_present[span]
                << " presence=" << span_presence << " target_gain=" << output_projection.target_gain
                << " interferer_gain=" << output_projection.interferer_gain
                << " sir_improvement_db=" << span_sir
                << " gram_determinant_ratio=" << output_projection.determinant_ratio << '\n';
            const std::string prefix = scenario.name + "/span_" + std::to_string(span);
            check(span_presence >= MinimumPresence, prefix + "/target_presence");
            check(output_projection.identifiable && output_projection.target_gain >= MinimumTargetGain,
                  prefix + "/gain_sensitive_target_retention");
            check(span_sir >= MinimumSirImprovementDb, prefix + "/sir_improvement_at_least_3db");
        }
    }
}

int run() {
    std::cout << std::setprecision(8)
        << "MECAG_SYNTHETIC_REGRESSION algorithm=" << nk::AlgorithmVersion
        << " config=" << std::hex << nk::Config{}.fingerprint() << std::dec
        << " delay=" << Delay << " presence_min=.70 target_gain_min=.35 sir_improvement_min_db=3"
        << " single_presence_min=.85 single_gain_min=.60 absent_energy_max=.01"
        << " groundtruth_given_to_engine=false human_speech_acceptance=false\n";
    auto reference = fixture(12, 165);
    nk::Profile profile;
    std::string error;
    check(nk::enroll(reference.data(), reference.size(), profile, error) && profile.valid(),
          "enrollment/original_12_second_in_memory_fixture");
    if (!profile.valid()) { std::cerr << error << '\n'; return 1; }
    const auto anchor = profile.anchor_hash;
    std::vector<Scenario> scenarios;
    auto single = overlap("single_target", 270, 0);
    single.target_only = true; scenarios.push_back(std::move(single));
    auto absent = overlap("only_interferer", 270, .6f);
    std::fill(absent.target.begin(), absent.target.end(), 0.f);
    absent.active.clear(); absent.absent.push_back({nk::Window, absent.target.size()});
    absent.interferer_only = true; build_mixture(absent); scenarios.push_back(std::move(absent));
    auto same_f0_absent = overlap("only_same_f0_interferer", 165, .6f);
    std::fill(same_f0_absent.target.begin(), same_f0_absent.target.end(), 0.f);
    same_f0_absent.active.clear(); same_f0_absent.absent.push_back({nk::Window, same_f0_absent.target.size()});
    same_f0_absent.interferer_only = true;
    build_mixture(same_f0_absent); scenarios.push_back(std::move(same_f0_absent));
    scenarios.push_back(overlap("original_165_270_gain_060", 270, .6f));
    scenarios.push_back(overlap("independent_harmonic_phases", 270, .6f, 77317, 45233));
    scenarios.push_back(overlap("different_interferer_f0_305", 305, .6f, 0, 31657));
    scenarios.push_back(overlap("equal_source_levels", 270, 1.f));
    scenarios.push_back(overlap("louder_interferer", 270, 1.4f));
    auto independent_am = overlap("independent_source_AM", 270, .6f);
    independent_am.interferer = fixture(3, 270, 1000, 3127, 9237, 2.13, .63, 1.09, .37);
    for (auto& sample : independent_am.interferer) sample *= .6f;
    build_mixture(independent_am); scenarios.push_back(std::move(independent_am));
    auto lead = overlap("interferer_starts_first", 270, .6f, 0, 58421);
    const auto onset = std::size_t(.6 * nk::SampleRate);
    std::fill(lead.target.begin(), lead.target.begin() + onset, 0.f);
    lead.active = {{onset + nk::Window, lead.target.size()}};
    lead.absent = {{nk::Window, onset}};
    build_mixture(lead); scenarios.push_back(std::move(lead));
    auto recovery = overlap("target_absence_and_return", 270, .6f, 0, 62953);
    const auto away = std::size_t(.8 * nk::SampleRate), back = std::size_t(1.4 * nk::SampleRate);
    std::fill(recovery.target.begin() + away, recovery.target.begin() + back, 0.f);
    recovery.active = {{nk::Window, away}, {back + nk::Window, recovery.target.size()}};
    recovery.absent = {{away + std::size_t(.15 * nk::SampleRate), back}};
    build_mixture(recovery); scenarios.push_back(std::move(recovery));
    for (const auto& scenario : scenarios) run_scenario(profile, scenario);
    check(profile.anchor_hash == anchor && profile.valid(), "enrollment/anchor_not_mutated_by_scenarios");
    std::cout << "Tests=" << checks << " failures=" << failures
        << " (fixed synthetic regressions only; real voices, indistinguishable same-F0 sources,"
        << " virtual cable consumers, and end-to-end latency remain unaccepted)\n";
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--trace") full_trace = true;
    try { return run(); }
    catch (const std::exception& error) { std::cerr << "Regression exception: " << error.what() << '\n'; return 1; }
}
