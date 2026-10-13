#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nk {
inline constexpr char AlgorithmVersion[] = "0.2.4-mecag-g1-partial-harmonic-hypotheses";
constexpr std::uint32_t FeatureSchema = 3;
constexpr std::size_t SampleRate = 16000;
constexpr std::size_t Hop = 160;
constexpr std::size_t Window = 480;
constexpr std::size_t FftSize = 512;
constexpr std::size_t Bins = FftSize / 2 + 1;
constexpr std::size_t MaxTracks = 4;
constexpr std::size_t MaxAliases = 3;
constexpr std::size_t MaxBeam = 8;
constexpr std::size_t MaxBasis = 16;
constexpr std::size_t MaxPrototypes = 8;
constexpr std::size_t Features = 12;

enum class Ownership : std::uint8_t { Unknown, Target, Interference, Noise };
enum class MissingReason : std::uint8_t {
    None, InsufficientSupport, LowEnergy, NoPeriodicity, MixedObservation,
    NoHistory, DisabledForAblation, NumericFault
};
struct Config {
    unsigned schema = 1;
    unsigned sample_rate = SampleRate;
    unsigned hop = Hop;
    unsigned window = Window;
    unsigned fft_size = FftSize;
    unsigned lookahead = 0;
    unsigned max_tracks = MaxTracks;
    unsigned max_aliases = MaxAliases;
    unsigned beam_width = MaxBeam;
    unsigned max_basis = MaxBasis;
    unsigned solver_iterations = 3;
    unsigned history_hops = 12;
    float identity_margin = 0.0f;
    float minimum_rms = 0.003f;
    float maximum_gain = 1.0f;
    unsigned missed_track_hops = 10;
    bool valid() const noexcept;
    std::uint64_t fingerprint() const noexcept;
};
struct Prototype {
    // Feature schema 3: normalized absolute-Hz power ENVELOPE, not a raw
    // harmonic comb. Schema 1/2 profiles require independent re-enrollment.
    std::array<float, Bins> power_basis{};
    std::array<float, Features> mean{};
    std::array<float, Features> variance{};
    bool voiced = false;
};
struct Profile {
    std::uint32_t schema = 1;
    std::uint32_t feature_schema = FeatureSchema;
    std::uint32_t prototype_count = 0;
    std::uint32_t accepted_frames = 0;
    std::uint64_t anchor_hash = 0;
    float accepted_seconds = 0;
    std::array<Prototype, MaxPrototypes> prototypes{};
    bool valid() const noexcept;
};
struct GroupAffinity {
    std::uint32_t group_id = 0;
    float f0 = 0, energy = 0;
    float harmonic = 0, onset = 0, modulation = 0;
    float reliability = 0;
    bool valid = false, voiced = false;
    MissingReason missing_reason = MissingReason::InsufficientSupport;
    unsigned observation_age_hops = 0;
    std::array<float, Bins> support{};
};
struct IdentityMatch {
    float relative_score = 0, reliability = 0;
    unsigned prototype = 0, supported_features = 0;
    bool valid = false;
    MissingReason missing_reason = MissingReason::InsufficientSupport;
    unsigned observation_age_hops = 0;
    // Bounded diagnostics; never PCM. Export detailed vectors only in an
    // explicitly selected local diagnostic session, not default application logs.
    std::uint32_t supported_feature_mask = 0;
    std::array<float, Features> observed_features{}, conditioned_mean{}, conditioned_variance{};
    std::array<float, Features> feature_log_ratio{}, observation_variance{};
};
struct TrackAssociation {
    std::uint64_t track_id = 0;
    float transition_cost = 0, reliability = 0;
    bool born = false, paused = false, valid = false;
    MissingReason missing_reason = MissingReason::NoHistory;
    unsigned observation_age_hops = 0;
};
struct Ablation {
    bool grouping = true, identity = true, tracking = true;
    bool reliability = true, competition = true, contribution = true;
    bool feedback = true, scheduling = true;
};
struct FrameResult {
    std::uint64_t epoch = 0, frame = 0, input_end_sample = 0;
    std::uint64_t anchor_hash = 0;
    std::uint64_t config_hash = 0;
    unsigned groups = 0, tracks = 0, basis_count = 0;
    unsigned partial_families = 0, ambiguous_peaks = 0;
    unsigned beam_states = 0, solver_steps = 0;
    unsigned feedback_accepted = 0, feedback_rejected = 0, feedback_committed = 0;
    unsigned algorithm_delay_samples = Window - Hop;
    bool valid = false, target_present = false, budget_exhausted = false, state_reset = false;
    bool solver_finite = false;
    float target_confidence = 0, residual_ratio = 0, output_rms = 0;
    float signed_residual_sum = 0, absolute_residual_sum = 0;
    double solver_objective_before = 0, solver_objective_after = 0;
    std::int64_t output_begin_sample = 0, output_end_sample = 0;
    double compute_us = 0;
    std::array<GroupAffinity, MaxTracks> grouping{};
    std::array<IdentityMatch, MaxTracks> identity{};
    std::array<TrackAssociation, MaxTracks> association{};
    std::array<Ownership, MaxTracks> ownership{};
    std::array<float, Bins> target_power{}, interference_power{}, noise_power{}, unknown_power{}, gate{};
};

// Slow-channel operation; never invoked from the audio worker.
bool enroll(const float* mono, std::size_t samples, Profile& result, std::string& error);

class Engine {
public:
    Engine(const Config& config, const Profile& profile);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void reset(std::uint64_t epoch) noexcept;
    void set_ablation(const Ablation& ablation) noexcept;
    void process_hop(const std::array<float, Hop>& input,
                     std::array<float, Hop>& output, FrameResult& result) noexcept;
    // Diagnostic unity-mask path through the identical STFT/OLA timeline.
    void process_unity_hop(const std::array<float, Hop>& input,
                          std::array<float, Hop>& output) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
