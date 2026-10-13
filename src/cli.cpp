#include "noisekiller/core.hpp"
#include "noisekiller/io.hpp"
#include "noisekiller/profile_store.hpp"
#include "noisekiller/windows.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

namespace {
std::string utf8(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "[unprintable endpoint]";
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr)) return "[unprintable endpoint]";
    return result;
#else
    return std::filesystem::path(value).u8string();
#endif
}
void usage() {
    std::cout << "NoiseKiller engineering build (not an accepted G3 MVP)\n"
        "  NoiseKiller.Cli enroll reference.wav profile.nkp\n"
        "  NoiseKiller.Cli process profile.nkp input.wav output.wav [diagnostics.json]\n"
        "  NoiseKiller.Cli benchmark profile.nkp [seconds: 1-10800]\n"
        "  NoiseKiller.Cli devices  (read-only capture/render inventory; never starts audio)\n"
        "Profiles are current-user DPAPI protected. Existing output files are never overwritten.\n"
        "Offline processing/compute benchmarks do not validate the virtual microphone or voice quality.\n";
}
bool exists(const std::wstring& path) { return std::filesystem::exists(std::filesystem::path(path)); }
double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, std::size_t(std::ceil(q * values.size()) - 1))];
}
int run(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::wstring mode = argv[1];
    std::string error;
    if (mode == L"devices" && argc == 2) {
        std::vector<nk::win::Device> capture, render;
        if (!nk::win::enumerate_capture_devices(capture, error) || !nk::win::enumerate_render_devices(render, error)) {
            std::cerr << error << '\n'; return 1;
        }
        std::cout << "CAPTURE (physical inputs or virtual recording endpoints; no stream started):\n";
        for (const auto& device : capture) std::cout << "  " << utf8(device.name) << "\n    " << utf8(device.id) << '\n';
        std::cout << "RENDER (NoiseKiller cable output must be a playback endpoint, not a recording endpoint):\n";
        for (const auto& device : render) std::cout << "  " << utf8(device.name) << "\n    " << utf8(device.id) << '\n';
        std::cout << "VB-CABLE example: write playback CABLE Input; target app selects recording CABLE Output.\n";
        return 0;
    }
    if (mode == L"enroll" && argc == 4) {
        if (exists(argv[3])) { std::cerr << "Profile exists; delete or select a new destination.\n"; return 2; }
        nk::AudioData audio;
        if (!nk::read_wav(argv[2], audio, error)) { std::cerr << error << '\n'; return 1; }
        auto pcm = nk::resample(audio);
        nk::Profile profile;
        if (!nk::enroll(pcm.data(), pcm.size(), profile, error) || !nk::win::save_profile(argv[3], profile, error)) {
            std::cerr << error << '\n'; return 1;
        }
        std::cout << "Encrypted reference saved; prototypes=" << profile.prototype_count
                  << ", accepted_seconds=" << profile.accepted_seconds << ". Quality remains unvalidated.\n";
        return 0;
    }
    if ((mode == L"process" && (argc == 5 || argc == 6)) || (mode == L"benchmark" && (argc == 3 || argc == 4))) {
        nk::Profile profile;
        if (!nk::win::load_profile(argv[2], profile, error)) { std::cerr << error << '\n'; return 1; }
        const bool benchmark = mode == L"benchmark";
        std::vector<float> pcm, output;
        unsigned seconds = 30;
        if (!benchmark) {
            if (exists(argv[4]) || (argc == 6 && exists(argv[5]))) { std::cerr << "Output destination already exists.\n"; return 2; }
            if (argc == 6 && std::filesystem::path(argv[4]).lexically_normal() == std::filesystem::path(argv[5]).lexically_normal()) {
                std::cerr << "Audio and report destinations must differ.\n"; return 2;
            }
            nk::AudioData audio;
            if (!nk::read_wav(argv[3], audio, error)) { std::cerr << error << '\n'; return 1; }
            pcm = nk::resample(audio); output.resize(((pcm.size() + nk::Hop - 1) / nk::Hop) * nk::Hop);
        } else if (argc == 4) {
            const std::wstring value = argv[3];
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) { std::cerr << "Invalid seconds.\n"; return 2; }
            const auto parsed = std::stoul(value);
            if (parsed < 1 || parsed > 10800) { std::cerr << "Seconds must be 1-10800.\n"; return 2; }
            seconds = unsigned(parsed);
        }
        const std::size_t frames = benchmark ? std::size_t(seconds) * 100 : (pcm.size() + nk::Hop - 1) / nk::Hop;
        nk::Engine engine(nk::Config{}, profile);
        std::array<float, nk::Hop> input{}, processed{};
        nk::FrameResult result;
        std::vector<double> durations; durations.reserve(frames);
        std::uint64_t feedback = 0, invalid = 0, target = 0, misses = 0, longest_misses = 0, consecutive_misses = 0;
        std::array<double, nk::MaxTracks> phases{};
        unsigned observed_groups = 0, observed_tracks = 0, observed_basis = 0, observed_beam = 0, observed_steps = 0;
        unsigned capped_frames = 0;
        std::array<std::uint64_t, 8> identity_reasons{};
        std::array<std::uint64_t, 4> ownership_counts{};
        std::uint64_t low_reliability = 0, committed = 0, state_resets = 0, partial_families = 0, ambiguous_peaks = 0;
        unsigned observed_partial_families = 0;
        unsigned random = 0x4e4b;
        const auto total_start = std::chrono::steady_clock::now();
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t i = 0; i < nk::Hop; ++i) {
                const std::size_t at = frame * nk::Hop + i;
                if (benchmark) {
                    random = 1664525 * random + 1013904223;
                    double mixture = 0;
                    for (unsigned source = 0; source < nk::MaxTracks; ++source) {
                        const double f0 = 110 + 51 * source + 8 * std::sin(double(at) / (6100 + source * 370));
                        phases[source] += 6.283185307179586 * f0 / nk::SampleRate;
                        const double envelope = .55 + .45 * std::pow(std::sin(double(at) / (3700 + source * 900)), 2);
                        for (unsigned harmonic = 1; harmonic <= 16; ++harmonic)
                            mixture += .05 * envelope * std::sin(harmonic * phases[source]) / std::sqrt(double(harmonic));
                    }
                    input[i] = float(mixture + .012 * (double(random) / 4294967295.0 - 0.5));
                } else input[i] = at < pcm.size() ? pcm[at] : 0;
            }
            const auto begin = std::chrono::steady_clock::now();
            engine.process_hop(input, processed, result);
            const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
            durations.push_back(elapsed); feedback += result.feedback_accepted; target += result.target_present; invalid += !result.valid;
            observed_groups = std::max(observed_groups, result.groups); observed_tracks = std::max(observed_tracks, result.tracks);
            observed_basis = std::max(observed_basis, result.basis_count); observed_beam = std::max(observed_beam, result.beam_states);
            observed_steps = std::max(observed_steps, result.solver_steps); capped_frames += result.budget_exhausted;
            committed += result.feedback_committed; state_resets += result.state_reset;
            partial_families += result.partial_families; ambiguous_peaks += result.ambiguous_peaks;
            observed_partial_families = std::max(observed_partial_families, result.partial_families);
            for (unsigned group = 0; group < result.groups; ++group) {
                const auto reason = std::size_t(result.identity[group].missing_reason);
                if (reason < identity_reasons.size()) ++identity_reasons[reason];
                const auto label = std::size_t(result.ownership[group]);
                if (label < ownership_counts.size()) ++ownership_counts[label];
                low_reliability += result.identity[group].valid && result.identity[group].reliability < .35f;
            }
            if (elapsed >= 10000) { ++misses; longest_misses = std::max(longest_misses, ++consecutive_misses); } else consecutive_misses = 0;
            if (!benchmark) for (std::size_t i = 0; i < nk::Hop && frame * nk::Hop + i < output.size(); ++i) output[frame * nk::Hop + i] = processed[i];
        }
        // A file sink finishes the declared OLA tail; it is not a live latency measurement.
        if (!benchmark) {
            const unsigned delay = result.algorithm_delay_samples;
            if (delay > nk::FftSize * 4) throw std::runtime_error("Invalid declared algorithm delay");
            std::vector<float> tail;
            tail.reserve(delay + nk::Hop);
            input.fill(0);
            for (unsigned i = 0; i < (delay + nk::Hop - 1) / nk::Hop; ++i) {
                engine.process_hop(input, processed, result);
                tail.insert(tail.end(), processed.begin(), processed.end());
            }
            output.insert(output.end(), tail.begin(), tail.end());
            if (output.size() < std::size_t(delay) + pcm.size()) throw std::runtime_error("Insufficient mature output");
            std::vector<float> aligned(output.begin() + delay, output.begin() + delay + pcm.size());
            if (!nk::write_wav(argv[4], aligned.data(), aligned.size(), nk::SampleRate, error)) { std::cerr << error << '\n'; return 1; }
        }
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - total_start).count();
        double compute_sum = 0; for (double time : durations) compute_sum += time;
        const double p99 = percentile(durations, 0.99);
        std::cout << "frames=" << frames << " compute_p99_us=" << p99 << " algorithm_RTF=" << compute_sum / (frames * 10000.0)
                  << " misses=" << misses << " longest_misses=" << longest_misses << " invalid=" << invalid
                  << " feedback=" << feedback << " wall_seconds=" << total << '\n';
        std::cout << "observed_groups=" << observed_groups << " tracks=" << observed_tracks << " basis=" << observed_basis
                  << " beam=" << observed_beam << " solver_steps=" << observed_steps << " capped_frames=" << capped_frames
                  << " (algorithm-only synthetic stress; not complete worker/game/driver acceptance)\n";
        std::cout << "ownership_U/T/I/N=" << ownership_counts[0] << '/' << ownership_counts[1] << '/' << ownership_counts[2] << '/' << ownership_counts[3]
                  << " identity_mixed=" << identity_reasons[std::size_t(nk::MissingReason::MixedObservation)]
                  << " identity_low_reliability=" << low_reliability << " feedback_committed=" << committed << " state_resets=" << state_resets << '\n';
        std::cout << "partial_unknown_families=" << partial_families << " max_partial_unknown_families=" << observed_partial_families
                  << " ambiguous_peaks=" << ambiguous_peaks << " (bounded model-relative ambiguity, not physical clean-source proof)\n";
        if (!benchmark && argc == 6) {
            std::ofstream report(std::filesystem::path(argv[5]), std::ios::binary);
            report << "{\n  \"kind\": \"offline_algorithm_diagnostic\",\n  \"mvp_accepted\": false,\n  \"frames\": " << frames
                << ",\n  \"algorithm_version\": \"" << nk::AlgorithmVersion << "\",\n  \"config_hash\": \"" << std::hex << nk::Config{}.fingerprint() << std::dec << "\""
                << ",\n  \"profile_feature_schema\": " << profile.feature_schema
                << ",\n  \"profile_anchor_hash\": \"" << std::hex << profile.anchor_hash << std::dec << "\""
                << ",\n  \"algorithm_compute_p99_us\": " << p99 << ",\n  \"algorithm_rtf\": " << compute_sum / (frames * 10000.0)
                << ",\n  \"misses\": " << misses << ",\n  \"longest_misses\": " << longest_misses
                << ",\n  \"invalid_frames\": " << invalid << ",\n  \"target_present_frames\": " << target
                << ",\n  \"feedback_accepted\": " << feedback
                << ",\n  \"feedback_committed\": " << committed << ",\n  \"state_resets\": " << state_resets
                << ",\n  \"ownership_counts_U_T_I_N\": [" << ownership_counts[0] << ',' << ownership_counts[1] << ',' << ownership_counts[2] << ',' << ownership_counts[3] << ']'
                << ",\n  \"identity_missing_reason_counts\": [";
            for (std::size_t i = 0; i < identity_reasons.size(); ++i) { if (i) report << ','; report << identity_reasons[i]; }
            report << ']' << ",\n  \"identity_low_reliability_candidates\": " << low_reliability
                << ",\n  \"max_observed_groups\": " << observed_groups << ",\n  \"max_observed_tracks\": " << observed_tracks
                << ",\n  \"max_observed_basis\": " << observed_basis << ",\n  \"max_observed_beam\": " << observed_beam
                << ",\n  \"max_observed_solver_steps\": " << observed_steps << ",\n  \"capacity_reached_frames\": " << capped_frames
                << ",\n  \"partial_unknown_families\": " << partial_families
                << ",\n  \"max_partial_unknown_families\": " << observed_partial_families
                << ",\n  \"ambiguous_peaks\": " << ambiguous_peaks
                << ",\n  \"config\": {\"sample_rate\":16000, \"hop_samples\":160, \"window_samples\":480, \"fft_size\":512, \"lookahead\":0}"
                << ",\n  \"D_add_ms\": null,\n  \"quality_acceptance\": \"not_measured\"\n}\n";
            if (!report) { std::cerr << "Could not write diagnostic report.\n"; return 1; }
        }
        return invalid ? 1 : 0;
    }
    usage(); return 2;
}
}

int wmain(int argc, wchar_t** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& exception) { std::cerr << "Error: " << exception.what() << '\n'; return 1; }
}
