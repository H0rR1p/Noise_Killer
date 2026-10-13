#include "noisekiller/core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

// Independent, bounded engineering implementation of the v0.4 mechanisms.
// The referenced libraries are design references, not linked or copied code.
// In particular, BroadReference below is NOT a trained population UBM.
namespace nk {
namespace {
constexpr float Pi = 3.14159265358979323846f;
constexpr float Eps = 1.0e-12f;
constexpr unsigned History = 12;
constexpr unsigned PeakCap = 40;
constexpr unsigned PitchCap = 192;
// Positive engineering candidate, not a human-speech calibrated value.
// Coefficients are power activations of L1-normalized columns.
constexpr float HistoryRegularization = 0.0001f;
using Spectrum = std::array<float, Bins>;
using FeatureVector = std::array<float, Features>;
using ComplexBuffer = std::array<std::complex<float>, FftSize>;
constexpr std::array<float, Features> VarianceFloor = {
    .20f, .20f, .20f, .20f, .20f, .20f, .20f, .20f,
    .0025f, .20f, .06f, .25f
};
constexpr std::array<float, Features> BroadVariance = {
    4.f, 4.f, 4.f, 4.f, 4.f, 4.f, 4.f, 4.f, .12f, 4.f, .9f, 4.f
};
constexpr std::array<float, Features> FeatureWeight = {
    1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, .7f, .5f, .15f, .7f
};
constexpr std::array<float, 9> BandEdges = {0, 250, 500, 750, 1000, 1500, 2500, 4000, 8001};

float unit(float x) noexcept { return std::max(0.f, std::min(1.f, x)); }
float bounded(float x, float lo, float hi) noexcept { return std::max(lo, std::min(hi, x)); }
float sum(const Spectrum& x) noexcept { float v = 0; for (float a : x) v += a; return v; }
float logistic(float x) noexcept { return 1.f / (1.f + std::exp(-bounded(x, -20.f, 20.f))); }
float softplus(float x) noexcept { x = bounded(x, -20.f, 20.f); return std::log1p(std::exp(x)); }
float dot(const Spectrum& a, const Spectrum& b) noexcept {
    float s = 0; for (std::size_t k = 0; k < Bins; ++k) s += a[k] * b[k]; return s;
}
float normalize(Spectrum& x) noexcept {
    const float total = sum(x);
    if (total > Eps) for (float& v : x) v /= total;
    return total;
}
float cosine(const Spectrum& a, const Spectrum& b) noexcept {
    return unit(dot(a, b) / std::sqrt(dot(a, a) * dot(b, b) + Eps * Eps));
}
float cents(float a, float b) noexcept {
    if (a <= 0 || b <= 0) return 0;
    return std::abs(1200.f * std::log2(a / b));
}
bool aliases(float a, float b) noexcept {
    return a > 0 && b > 0 && std::min({cents(a, b), cents(a * 2, b), cents(a, b * 2)}) < 75;
}

struct Fft {
    std::array<unsigned, FftSize> reversed{};
    std::array<std::complex<float>, FftSize / 2> roots{};
    Fft() noexcept {
        for (unsigned i = 0; i < FftSize; ++i) {
            unsigned v = i, r = 0;
            for (unsigned b = 0; b < 9; ++b) { r = (r << 1) | (v & 1); v >>= 1; }
            reversed[i] = r;
        }
        for (unsigned i = 0; i < FftSize / 2; ++i) {
            const float phase = -2 * Pi * float(i) / float(FftSize);
            roots[i] = {std::cos(phase), std::sin(phase)};
        }
    }
    void run(ComplexBuffer& x, bool inverse) const noexcept {
        for (unsigned i = 0; i < FftSize; ++i) if (reversed[i] > i) std::swap(x[i], x[reversed[i]]);
        for (unsigned width = 2; width <= FftSize; width <<= 1) {
            const unsigned half = width / 2, stride = unsigned(FftSize) / width;
            for (unsigned base = 0; base < FftSize; base += width) {
                for (unsigned j = 0; j < half; ++j) {
                    auto w = roots[j * stride]; if (inverse) w = std::conj(w);
                    const auto a = x[base + j], b = w * x[base + j + half];
                    x[base + j] = a + b; x[base + j + half] = a - b;
                }
            }
        }
        if (inverse) for (auto& v : x) v /= float(FftSize);
    }
};

struct Frontend {
    Fft fft;
    std::array<float, Window> window{}, samples{}, ola{}, norm{};
    ComplexBuffer spectrum{}, synthesis{};
    Spectrum power{};
    float rms = 0, flatness = 0, total = 0;
    Frontend() noexcept {
        // Half-sample periodic square-root Hann avoids a zero denominator at
        // startup. The actual accumulated w^2, not a hard-coded COLA factor,
        // normalizes startup, steady state and tail consistently.
        for (std::size_t i = 0; i < Window; ++i)
            window[i] = std::sqrt(.5f - .5f * std::cos(2 * Pi * (float(i) + .5f) / float(Window)));
    }
    void clear() noexcept {
        samples.fill(0); ola.fill(0); norm.fill(0); spectrum.fill({0, 0});
        synthesis.fill({0, 0}); power.fill(0); rms = flatness = total = 0;
    }
    bool analyze(const std::array<float, Hop>& input) noexcept {
        for (float v : input) if (!std::isfinite(v) || std::abs(v) > 1.001f) return false;
        for (std::size_t i = 0; i < Window - Hop; ++i) samples[i] = samples[i + Hop];
        std::copy(input.begin(), input.end(), samples.begin() + Window - Hop);
        spectrum.fill({0, 0});
        double energy = 0;
        for (std::size_t i = 0; i < Window; ++i) {
            energy += double(samples[i]) * samples[i]; spectrum[i] = {samples[i] * window[i], 0};
        }
        rms = float(std::sqrt(energy / Window)); fft.run(spectrum, false);
        total = 0; float logs = 0;
        for (std::size_t k = 0; k < Bins; ++k) {
            power[k] = std::norm(spectrum[k]) / float(FftSize * FftSize);
            total += power[k]; if (k > 1) logs += std::log(power[k] + Eps);
        }
        flatness = unit(std::exp(logs / float(Bins - 2)) / ((total - power[0] - power[1]) / float(Bins - 2) + Eps));
        return std::isfinite(total);
    }
    void reconstruct(const Spectrum& gate, std::array<float, Hop>& output) noexcept {
        synthesis.fill({0, 0});
        for (std::size_t k = 0; k < Bins; ++k) synthesis[k] = spectrum[k] * gate[k];
        for (std::size_t k = 1; k < FftSize / 2; ++k) synthesis[FftSize - k] = std::conj(synthesis[k]);
        fft.run(synthesis, true);
        for (std::size_t i = 0; i < Window; ++i) {
            ola[i] += synthesis[i].real() * window[i]; norm[i] += window[i] * window[i];
        }
        for (std::size_t i = 0; i < Hop; ++i) {
            const float v = norm[i] > Eps ? ola[i] / norm[i] : 0;
            output[i] = std::isfinite(v) ? bounded(v, -1.f, 1.f) : 0;
        }
        for (std::size_t i = 0; i < Window - Hop; ++i) { ola[i] = ola[i + Hop]; norm[i] = norm[i + Hop]; }
        std::fill(ola.begin() + Window - Hop, ola.end(), 0.f);
        std::fill(norm.begin() + Window - Hop, norm.end(), 0.f);
    }
};

struct Peak { float frequency = 0, power = 0; unsigned bin = 0; float height = 0; };
struct Pitch { float f0 = 0, score = 0, harmonic = 0; unsigned hits = 0; };
struct Analyzer {
    std::array<Peak, PeakCap> peaks{};
    std::array<Pitch, PitchCap> pitches{};
    std::array<Pitch, PitchCap> partial_pitches{};
    unsigned peak_count = 0, pitch_count = 0, partial_pitch_count = 0;
    bool saturated = false;
    void find_peaks(const Spectrum& power) noexcept {
        peak_count = pitch_count = partial_pitch_count = 0; saturated = false;
        float maximum = 0; for (float x : power) maximum = std::max(maximum, x);
        for (unsigned k = 2; k + 1 < Bins; ++k) {
            if (power[k] < maximum * .004f || power[k] <= power[k - 1] || power[k] < power[k + 1]) continue;
            const float a = std::log(power[k - 1] + Eps), b = std::log(power[k] + Eps), c = std::log(power[k + 1] + Eps);
            const float delta = bounded(.5f * (a - c) / (a - 2 * b + c + Eps), -.5f, .5f);
            const float height = std::exp(b - .25f * (a - c) * delta);
            const Peak p{(float(k) + delta) * float(SampleRate) / float(FftSize), power[k], k, height};
            unsigned at = peak_count;
            if (at == PeakCap) { saturated = true; if (p.power <= peaks[at - 1].power) continue; --at; }
            else ++peak_count;
            while (at > 0 && peaks[at - 1].power < p.power) { peaks[at] = peaks[at - 1]; --at; }
            peaks[at] = p;
        }
    }
    Pitch evaluate(float f0) const noexcept {
        Pitch p; p.f0 = f0; float covered = 0, fundamental = 0, weighted = 0, available = 0;
        std::array<bool, 64> seen{};
        for (unsigned j = 0; j < peak_count; ++j) {
            const auto& peak = peaks[j]; available += peak.power;
            const unsigned harmonic = unsigned(std::lround(peak.frequency / f0));
            if (harmonic == 0 || harmonic >= seen.size()) continue;
            const float tolerance = std::max(13.f, .035f * f0);
            const float d = std::abs(peak.frequency - float(harmonic) * f0);
            if (d >= tolerance) continue;
            const float agreement = 1 - d / tolerance;
            covered += agreement * peak.power;
            weighted += agreement * peak.power / std::sqrt(float(harmonic));
            if (harmonic == 1) fundamental += peak.power;
            if (!seen[harmonic]) { ++p.hits; seen[harmonic] = true; }
        }
        p.harmonic = unit(covered / (available + Eps));
        const float fundamental_present = unit(fundamental / (available * .08f + Eps));
        p.score = weighted * (.42f + .58f * fundamental_present) * std::min(1.f, float(p.hits) / 3.f);
        if (p.hits < 2) p.score = 0;
        return p;
    }
    void find_pitches() noexcept {
        for (unsigned j = 0; j < peak_count; ++j) {
            for (unsigned h = 1; h <= 8; ++h) {
                const float f0 = peaks[j].frequency / float(h);
                if (f0 < 70 || f0 > 420) continue;
                bool duplicate = false;
                for (unsigned i = 0; i < pitch_count; ++i) if (std::abs(pitches[i].f0 - f0) < 2.5f) { duplicate = true; break; }
                if (duplicate) continue;
                // Mixture energy share is not harmonic reliability: a quiet
                // but coherent family must remain eligible for inhibition.
                const Pitch p = evaluate(f0); if (p.score <= 0 || p.hits < 3) continue;
                if (pitch_count == PitchCap) { saturated = true; continue; }
                pitches[pitch_count++] = p;
            }
        }
        std::sort(pitches.begin(), pitches.begin() + pitch_count,
                  [](const Pitch& a, const Pitch& b) { return a.score > b.score; });
    }
    void find_partial_pitches() noexcept {
        // Preserve the complete-source and registration proposal sequence.
        // High-order-only explanations have less independent low-order
        // evidence; they can expose ambiguity as Unknown, but cannot create
        // or replace a speaker candidate, alias, enrollment state or track.
        // The evidence model accepts orders 1..63, so retaining only the first
        // eight here made real high-order residual explanations unreachable.
        partial_pitch_count = pitch_count;
        for (unsigned p = 0; p < pitch_count; ++p) partial_pitches[p] = pitches[p];
        for (unsigned j = 0; j < peak_count; ++j) {
            const unsigned highest = std::min(63u, unsigned(peaks[j].frequency / 70.f));
            for (unsigned h = 9; h <= highest; ++h) {
                const float f0 = peaks[j].frequency / float(h);
                if (f0 < 70 || f0 > 420) continue;
                bool duplicate = false;
                for (unsigned p = 0; p < partial_pitch_count; ++p)
                    if (std::abs(partial_pitches[p].f0 - f0) < 2.5f) { duplicate = true; break; }
                if (duplicate) continue;
                const Pitch p = evaluate(f0);
                if (p.score <= 0 || p.hits < 3) continue;
                if (partial_pitch_count == PitchCap) { saturated = true; continue; }
                partial_pitches[partial_pitch_count++] = p;
            }
        }
    }
};

float peak_agreement(float f0, const Peak& peak) noexcept {
    if (f0 <= 0) return 0;
    const unsigned h = unsigned(std::lround(peak.frequency / f0));
    if (!h || h >= 64) return 0;
    return unit(1.f - std::abs(peak.frequency - float(h) * f0) / std::max(13.f, .035f * f0));
}

void actual_peak_support(const Analyzer& analyzer, float f0, Spectrum& support,
                         std::array<bool, PeakCap>& membership) noexcept {
    support.fill(0); membership.fill(false);
    for (unsigned p = 0; p < analyzer.peak_count; ++p) {
        if (peak_agreement(f0, analyzer.peaks[p]) <= 0) continue;
        membership[p] = true;
        const float center = analyzer.peaks[p].frequency * float(FftSize) / float(SampleRate);
        for (int k = std::max(1, int(std::floor(center - 2.2f)));
             k <= std::min(int(Bins) - 1, int(std::ceil(center + 2.2f))); ++k) {
            const float distance = std::abs(float(k) - center);
            // Cover the observed line's main lobe; never paint a periodic
            // comb through unobserved bins and call it an independent source.
            const float weight = distance <= 1.2f ? 1.f : unit((2.2f - distance) / 1.f);
            support[std::size_t(k)] = std::max(support[std::size_t(k)], weight);
        }
    }
}

void smooth(const Spectrum& power, Spectrum& envelope) noexcept {
    for (std::size_t k = 0; k < Bins; ++k) {
        float v = 0, norm = 0;
        for (int d = -5; d <= 5; ++d) {
            const int j = int(k) + d; if (j < 0 || j >= int(Bins)) continue;
            const float w = 6.f - float(std::abs(d)); v += w * power[std::size_t(j)]; norm += w;
        }
        envelope[k] = v / norm;
    }
}

void source_envelope(const Spectrum& power, float f0, bool voiced, const Analyzer& analyzer,
                     const std::array<bool, PeakCap>* membership, Spectrum& envelope) noexcept {
    if (!voiced || f0 < 60 || f0 > 840) { smooth(power, envelope); normalize(envelope); return; }
    std::array<float, 128> frequency{}, amplitude{}; unsigned count = 0;
    float maximum = 0;
    for (unsigned p = 0; p < analyzer.peak_count; ++p)
        if (!membership || (*membership)[p]) maximum = std::max(maximum, analyzer.peaks[p].height);
    for (unsigned harmonic = 1; harmonic < frequency.size() && float(harmonic) * f0 < 7800; ++harmonic) {
        unsigned selected = analyzer.peak_count; float height = 0;
        for (unsigned p = 0; p < analyzer.peak_count; ++p) {
            if (membership && !(*membership)[p]) continue;
            const auto& peak = analyzer.peaks[p];
            if (unsigned(std::lround(peak.frequency / f0)) != harmonic || peak_agreement(f0, peak) <= 0) continue;
            if (peak.height > height) { height = peak.height; selected = p; }
        }
        if (selected == analyzer.peak_count || height < maximum * .0001f) continue;
        // Reuse the original window's measured peak location/height. Applying
        // a partial identity mask can remove one side lobe; fitting a new
        // parabola to those artificial zeros would move/reject a real peak.
        const float hz = analyzer.peaks[selected].frequency;
        if (count && hz <= frequency[count - 1]) continue;
        frequency[count] = hz; amplitude[count] = height; ++count;
    }
    envelope.fill(0); if (count < 2) return;
    unsigned right = 1;
    constexpr float ObservedLobeHz = 2.2f * float(SampleRate) / float(FftSize);
    for (std::size_t k = 1; k < Bins; ++k) {
        const float hz = float(k) * float(SampleRate) / float(FftSize);
        // Endpoint centers are NOT the edges of the observed FFT line. Retain
        // only their actually observed finite main-lobe neighborhood; zero
        // outside it. No spectrum is extrapolated into unobserved far bands.
        if (hz < frequency[0]) {
            if (frequency[0] - hz <= ObservedLobeHz) envelope[k] = amplitude[0];
            continue;
        }
        if (hz > frequency[count - 1]) {
            if (hz - frequency[count - 1] <= ObservedLobeHz) envelope[k] = amplitude[count - 1];
            continue;
        }
        while (right < count && frequency[right] < hz) ++right;
        if (right >= count) continue;
        const float interval = frequency[right] - frequency[right - 1];
        if (interval > 3.1f * f0) {
            if (hz - frequency[right - 1] <= ObservedLobeHz) envelope[k] = amplitude[right - 1];
            else if (frequency[right] - hz <= ObservedLobeHz) envelope[k] = amplitude[right];
            continue;
        }
        const float fraction = unit((hz - frequency[right - 1]) / (interval + Eps));
        envelope[k] = amplitude[right - 1] * (1 - fraction) + amplitude[right] * fraction;
    }
    normalize(envelope);
}

unsigned features(const Spectrum& power, float f0, bool voiced, FeatureVector& out) noexcept {
    // Schema 3 input is already a source envelope. This function applies no
    // additional smoothing: both sides use the same envelope*observation-mask
    // operator and the reference never acquires a second filtering pass.
    const Spectrum& envelope = power;
    std::array<float, 8> bands{}; const float total = sum(power);
    float centroid = 0, logs = 0; unsigned nonzero = 0, occupied = 0;
    for (std::size_t k = 1; k < Bins; ++k) {
        const float hz = float(k) * float(SampleRate) / float(FftSize);
        for (unsigned b = 0; b < 8; ++b) if (hz >= BandEdges[b] && hz < BandEdges[b + 1]) { bands[b] += envelope[k]; break; }
        centroid += float(k) / float(Bins - 1) * envelope[k];
        if (envelope[k] > total * .0001f) { logs += std::log(envelope[k] + Eps); ++nonzero; }
    }
    for (unsigned b = 0; b < 8; ++b) {
        out[b] = std::log(std::max(bands[b] / (total + Eps), 1.0e-6f));
        if (bands[b] > total * .002f) ++occupied;
    }
    out[8] = centroid / (total + Eps);
    // Texture of occupied source-support bins, rather than the whole mixture.
    out[9] = nonzero ? bounded(logs / float(nonzero) - std::log(total / float(nonzero) + Eps), -12, 0) : -12;
    out[10] = voiced && f0 > 0 ? std::log(f0 / 200.f) : 0;
    out[11] = bounded(std::log((bands[0] + bands[1] + Eps) / (bands[5] + bands[6] + bands[7] + Eps)), -10, 10);
    return occupied;
}

struct Candidate {
    GroupAffinity group{};
    Spectrum power{}, envelope{};
    FeatureVector feature{};
    unsigned occupied = 0;
    Spectrum observation_support{}, identity_support{}, identity_power{};
    std::array<bool, PeakCap> peak_membership{}, identity_membership{};
    unsigned independent_peaks = 0;
    float exclusive_fraction = 0;
    int slot = -1;
    std::array<float, MaxAliases> alias_f0{}, alias_cost{};
    unsigned alias_count = 1;
};
struct Track {
    std::uint64_t id = 0;
    bool active = false, voiced = false, trusted = false;
    unsigned age = 0, misses = 0, trusted_age = 0;
    std::uint64_t trusted_frame = 0;
    float f0 = 0, velocity = 0, energy = 0, coefficient = 0;
    Spectrum envelope{}, trusted_spectrum{};
    Ownership previous = Ownership::Unknown;
    std::array<float, MaxAliases> alias_f0{};
    unsigned alias_count = 0;
};
struct Feedback {
    bool valid = false;
    std::uint64_t epoch = 0, frame = 0, input_end = 0, track = 0;
    float f0 = 0, velocity = 0, energy = 0, coefficient = 0;
    Spectrum spectrum{};
};
struct Beam {
    float cost = 0;
    std::array<Ownership, MaxTracks> owner{};
    std::array<unsigned, MaxTracks> alias{};
};

IdentityMatch match(const Candidate& candidate, const Profile& profile,
                    const std::array<Spectrum, MaxPrototypes>& reference_power, const Analyzer& analyzer, bool reliability) noexcept {
    IdentityMatch result; float best = -std::numeric_limits<float>::infinity();
    if (candidate.group.energy <= Eps) { result.missing_reason = MissingReason::LowEnergy; return result; }
    // The overlap guard is retained on information actually used for identity.
    // A mixture-wide comb collision is not evidence that ALL of a source's
    // independent observed lines are contaminated. Shared/unresolved lines
    // under the current finite peak model are excluded, never relabelled as
    // clean speech; hidden same-F0 sources remain a physical limitation.
    if (candidate.exclusive_fraction < .45f || (candidate.group.voiced && candidate.independent_peaks < 3)) {
        result.missing_reason = MissingReason::MixedObservation; return result;
    }
    Spectrum observation_envelope{};
    source_envelope(candidate.identity_power, candidate.group.f0, candidate.group.voiced,
                    analyzer, &candidate.identity_membership, observation_envelope);
    for (std::size_t k = 0; k < Bins; ++k) observation_envelope[k] *= candidate.identity_support[k];
    if (normalize(observation_envelope) <= Eps) { result.missing_reason = MissingReason::InsufficientSupport; return result; }
    FeatureVector observation{}; features(observation_envelope, candidate.group.f0, candidate.group.voiced, observation);
    std::array<float, 8> exposure{};
    for (std::size_t k = 1; k < Bins; ++k) {
        const float hz = float(k) * float(SampleRate) / float(FftSize);
        for (unsigned band = 0; band < 8; ++band)
            if (hz >= BandEdges[band] && hz < BandEdges[band + 1]) { exposure[band] += candidate.identity_support[k]; break; }
    }
    unsigned observed_bands = 0;
    for (float e : exposure) observed_bands += e >= .40f;
    if (observed_bands < 3) { result.missing_reason = MissingReason::InsufficientSupport; return result; }
    unsigned best_supported = 0, best_bands = 0;
    for (unsigned p = 0; p < profile.prototype_count; ++p) {
        const auto& prototype = profile.prototypes[p];
        if (prototype.voiced != candidate.group.voiced) continue;
        Spectrum conditioned_reference{};
        for (std::size_t k = 0; k < Bins; ++k)
            conditioned_reference[k] = reference_power[p][k] * candidate.identity_support[k];
        if (normalize(conditioned_reference) <= Eps) continue;
        FeatureVector reference{}; features(conditioned_reference, candidate.group.f0, candidate.group.voiced, reference);
        // Pitch distribution remains registered data, not an identity label.
        reference[10] = prototype.mean[10];
        std::array<bool, 8> observable{}; unsigned supported_bands = 0;
        for (unsigned band = 0; band < 8; ++band) {
            // Geometric mask exposure is not sufficient signal information.
            // A log-floor from a near-empty/noise-floor OBSERVATION is missing,
            // not a giant negative identity measurement. Qualification never
            // depends on how well the reference agrees: a strong observation
            // where the anchor predicts little is legitimate negative evidence.
            observable[band] = exposure[band] >= .40f && observation[band] > std::log(.002f);
            supported_bands += observable[band];
        }
        if (supported_bands < 3) continue;
        float score = 0, weights = 0;
        unsigned supported = 0;
        std::uint32_t feature_mask = 0; FeatureVector likelihoods{};
        for (unsigned f = 0; f < Features; ++f) {
            if (f < 8 && !observable[f]) continue;
            // Spectral texture of missing harmonic lines is not observable.
            // It must not be filled with zero or counted as a valid feature.
            if (f == 9) continue;
            if (f == 10 && !candidate.group.voiced) continue;
            if (f == 11 && ((!observable[0] && !observable[1]) ||
                (!observable[5] && !observable[6] && !observable[7]))) continue;
            const float v = prototype.variance[f], broad = std::max(BroadVariance[f], v * 5);
            const float delta = observation[f] - reference[f];
            // Per-feature log density ratio to a wider, same-center rejection
            // reference. This is an engineering outlier model, not a population
            // background model, calibrated probability, or ALIZE checkpoint.
            const float llr = .5f * std::log(broad / v) - .5f * delta * delta * (1.f / v - 1.f / broad);
            score += FeatureWeight[f] * llr; weights += FeatureWeight[f];
            likelihoods[f] = llr; feature_mask |= std::uint32_t(1) << f;
            ++supported;
        }
        if (supported < 5 || weights <= 0) continue;
        score /= weights;
        if (score > best) {
            best = score; result.prototype = p; best_supported = supported; best_bands = supported_bands;
            result.supported_feature_mask = feature_mask; result.observed_features = observation;
            result.conditioned_mean = reference; result.conditioned_variance = prototype.variance;
            result.feature_log_ratio = likelihoods;
        }
    }
    if (!std::isfinite(best)) return result;
    result.valid = true; result.missing_reason = MissingReason::None;
    result.relative_score = bounded(best, -20, 20);
    result.supported_features = best_supported;
    result.reliability = reliability ? unit(candidate.group.reliability *
        (.65f + .35f * unit(float(best_bands) / 5)) * std::sqrt(candidate.exclusive_fraction)) : 1.f;
    return result;
}

float feature_distance(const FeatureVector& a, const FeatureVector& b) noexcept {
    float d = 0;
    for (unsigned f = 0; f < Features; ++f) d += FeatureWeight[f] * (a[f] - b[f]) * (a[f] - b[f]) / BroadVariance[f];
    return d / float(Features);
}
std::uint64_t profile_hash(const Profile& profile) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&hash](const void* ptr, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(ptr);
        for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    };
    add(&profile.feature_schema, sizeof(profile.feature_schema));
    add(&profile.prototype_count, sizeof(profile.prototype_count));
    for (unsigned i = 0; i < profile.prototype_count; ++i) {
        const auto& p = profile.prototypes[i];
        add(p.power_basis.data(), sizeof(float) * Bins); add(p.mean.data(), sizeof(float) * Features);
        add(p.variance.data(), sizeof(float) * Features); const unsigned char state = p.voiced ? 1 : 0; add(&state, 1);
    }
    return hash ? hash : 1;
}
}

bool Config::valid() const noexcept {
    return schema == 1 && sample_rate == SampleRate && hop == Hop && window == Window && fft_size == FftSize && lookahead == 0 &&
        max_tracks > 0 && max_tracks <= MaxTracks && max_aliases > 0 && max_aliases <= MaxAliases &&
        beam_width > 0 && beam_width <= MaxBeam && max_basis >= 2 && max_basis <= MaxBasis &&
        solver_iterations > 0 && solver_iterations <= 3 && history_hops > 0 && history_hops <= History &&
        missed_track_hops > 0 && missed_track_hops <= History && std::isfinite(identity_margin) && std::abs(identity_margin) <= 10 &&
        std::isfinite(minimum_rms) && minimum_rms > 0 && minimum_rms < .2f &&
        std::isfinite(maximum_gain) && maximum_gain > 0 && maximum_gain <= 1;
}

std::uint64_t Config::fingerprint() const noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto integer = [&hash](std::uint32_t value) noexcept {
        for (unsigned byte = 0; byte < 4; ++byte) { hash ^= (value >> (byte * 8)) & 255u; hash *= 1099511628211ull; }
    };
    const auto real = [&integer](float value) noexcept {
        std::uint32_t bits = 0; static_assert(sizeof(bits) == sizeof(value), "32-bit IEEE float configuration");
        std::memcpy(&bits, &value, sizeof(bits)); integer(bits);
    };
    for (const char c : AlgorithmVersion) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
    integer(schema); integer(sample_rate); integer(hop); integer(window); integer(fft_size); integer(lookahead);
    integer(max_tracks); integer(max_aliases); integer(beam_width); integer(max_basis); integer(solver_iterations);
    integer(history_hops); real(identity_margin); real(minimum_rms); real(maximum_gain); integer(missed_track_hops);
    return hash ? hash : 1;
}

bool Profile::valid() const noexcept {
    if (schema != 1 || feature_schema != FeatureSchema || prototype_count == 0 || prototype_count > MaxPrototypes ||
        anchor_hash == 0 || accepted_frames < 300 || !std::isfinite(accepted_seconds) || accepted_seconds < 3 || accepted_seconds > 30.01f ||
        std::abs(accepted_seconds - float(accepted_frames) * float(Hop) / float(SampleRate)) > .02f) return false;
    for (unsigned p = 0; p < prototype_count; ++p) {
        const auto& prototype = prototypes[p]; float total = 0;
        for (float v : prototype.power_basis) { if (!std::isfinite(v) || v < 0 || v > 1.001f) return false; total += v; }
        if (std::abs(total - 1) > .02f) return false;
        for (unsigned f = 0; f < Features; ++f)
            if (!std::isfinite(prototype.mean[f]) || std::abs(prototype.mean[f]) > 50 ||
                !std::isfinite(prototype.variance[f]) || prototype.variance[f] <= 0 || prototype.variance[f] > 1000) return false;
    }
    return anchor_hash == profile_hash(*this);
}

bool enroll(const float* mono, std::size_t samples, Profile& result, std::string& error) {
    result = Profile{}; error.clear();
    if (!mono || samples < 10 * SampleRate || samples > 30 * SampleRate) { error = "Enrollment requires 10-30 seconds of 16 kHz mono reference."; return false; }
    std::size_t clipped = 0; double energy = 0;
    for (std::size_t i = 0; i < samples; ++i) {
        if (!std::isfinite(mono[i]) || std::abs(mono[i]) > 1.001f) { error = "Reference contains non-finite or out-of-range PCM."; return false; }
        clipped += std::abs(mono[i]) >= .995f; energy += double(mono[i]) * mono[i];
    }
    if (clipped > samples / 200) { error = "Reference is clipped; reduce microphone gain and record again."; return false; }
    if (std::sqrt(energy / samples) < .004) { error = "Reference is too quiet or silent."; return false; }
    struct Record { Spectrum power{}; FeatureVector feature{}; bool voiced = false; };
    std::vector<Record> records; records.reserve(samples / Hop);
    Frontend frontend; Analyzer analyzer;
    std::array<float, Hop> input{}; unsigned voiced_count = 0, mixed = 0, noisy = 0;
    for (std::size_t first = 0; first + Hop <= samples; first += Hop) {
        std::copy(mono + first, mono + first + Hop, input.begin()); frontend.analyze(input);
        if (first < Window - Hop || frontend.rms < .004f) continue;
        analyzer.find_peaks(frontend.power); analyzer.find_pitches();
        Record record; std::array<bool, PeakCap> members{};
        const Pitch pitch = analyzer.pitch_count ? analyzer.pitches[0] : Pitch{};
        record.voiced = pitch.hits >= 2 && pitch.harmonic > .48f && frontend.flatness < .35f;
        if (record.voiced) {
            Spectrum support{};
            actual_peak_support(analyzer, pitch.f0, support, members);
            for (std::size_t k = 0; k < Bins; ++k) record.power[k] = frontend.power[k] * support[k];
            for (unsigned j = 1; j < analyzer.pitch_count; ++j) {
                const auto& alternative = analyzer.pitches[j];
                if (!aliases(pitch.f0, alternative.f0) && alternative.score > pitch.score * .35f && alternative.hits >= 3) {
                    unsigned independent = 0; float separate = 0, available = 0;
                    for (unsigned p = 0; p < analyzer.peak_count; ++p) {
                        available += analyzer.peaks[p].power;
                        if (!members[p] && peak_agreement(alternative.f0, analyzer.peaks[p]) > 0) {
                            ++independent; separate += analyzer.peaks[p].power;
                        }
                    }
                    if (independent >= 3 && separate > available * .20f) { ++mixed; break; }
                }
            }
            ++voiced_count;
        } else {
            if (frontend.flatness > .60f) { ++noisy; continue; }
            record.power = frontend.power;
        }
        if (normalize(record.power) <= Eps) continue;
        Spectrum envelope{}; source_envelope(record.power, pitch.f0, record.voiced, analyzer, &members, envelope);
        if (normalize(envelope) <= Eps) continue;
        record.power = envelope;
        features(record.power, pitch.f0, record.voiced, record.feature); records.push_back(record);
    }
    if (records.size() < 300 || voiced_count < 100) { error = "Insufficient usable speech: record normal speech alone for at least 10 seconds."; return false; }
    if (mixed * 10 > voiced_count) { error = "Reference has persistent competing harmonic families; record alone and retry."; return false; }
    if (noisy > records.size()) { error = "Reference appears dominated by broadband noise; use a quieter location."; return false; }
    // Deterministic state-conditioned, farthest-point initialized k-means.
    // Slow-channel vectors and iterations are deliberately outside process_hop.
    std::vector<unsigned> assignment(records.size());
    unsigned next_prototype = 0;
    for (unsigned state = 0; state < 2; ++state) {
        const bool voiced = state == 0; std::vector<unsigned> selected;
        for (unsigned i = 0; i < records.size(); ++i) if (records[i].voiced == voiced) selected.push_back(i);
        if (selected.empty()) continue;
        const unsigned capacity = std::min<unsigned>(voiced ? 6 : 2, unsigned(selected.size() / 30 + 1));
        std::array<FeatureVector, MaxPrototypes> centers{}; centers[0] = records[selected[0]].feature;
        unsigned count = 1;
        for (; count < capacity; ++count) {
            float largest = 0; unsigned farthest = selected[0];
            for (unsigned r : selected) {
                float nearest = std::numeric_limits<float>::infinity();
                for (unsigned c = 0; c < count; ++c) nearest = std::min(nearest, feature_distance(records[r].feature, centers[c]));
                if (nearest > largest) { largest = nearest; farthest = r; }
            }
            if (largest < .005f) break; centers[count] = records[farthest].feature;
        }
        for (unsigned iteration = 0; iteration < 12; ++iteration) {
            std::array<FeatureVector, MaxPrototypes> sums{}; std::array<unsigned, MaxPrototypes> sizes{};
            for (unsigned r : selected) {
                float best = std::numeric_limits<float>::infinity(); unsigned at = 0;
                for (unsigned c = 0; c < count; ++c) {
                    const float d = feature_distance(records[r].feature, centers[c]); if (d < best) { best = d; at = c; }
                }
                assignment[r] = at; ++sizes[at]; for (unsigned f = 0; f < Features; ++f) sums[at][f] += records[r].feature[f];
            }
            for (unsigned c = 0; c < count; ++c) if (sizes[c])
                for (unsigned f = 0; f < Features; ++f) centers[c][f] = sums[c][f] / float(sizes[c]);
        }
        for (unsigned c = 0; c < count; ++c) {
            Prototype prototype; unsigned size = 0; prototype.voiced = voiced; prototype.mean = centers[c];
            for (unsigned r : selected) if (assignment[r] == c) {
                ++size;
                for (std::size_t k = 0; k < Bins; ++k) prototype.power_basis[k] += records[r].power[k];
                for (unsigned f = 0; f < Features; ++f) {
                    const float d = records[r].feature[f] - prototype.mean[f]; prototype.variance[f] += d * d;
                }
            }
            if (!size) continue;
            normalize(prototype.power_basis);
            for (unsigned f = 0; f < Features; ++f) prototype.variance[f] = std::max(VarianceFloor[f], prototype.variance[f] / float(size));
            result.prototypes[next_prototype++] = prototype;
        }
    }
    result.prototype_count = next_prototype; result.accepted_frames = unsigned(records.size());
    result.accepted_seconds = float(records.size()) * float(Hop) / float(SampleRate); result.anchor_hash = profile_hash(result);
    if (!result.valid()) { result = Profile{}; error = "Unable to construct a finite compatible reference profile."; return false; }
    return true;
}

struct Engine::State {
    const Config config;
    const Profile profile;
    Ablation ablation{};
    Frontend frontend;
    Analyzer analyzer;
    std::uint64_t epoch = 1, frame = 0, next_track = 1;
    unsigned history_count = 0, history_write = 0;
    std::array<Spectrum, History> history{};
    std::array<Track, MaxTracks> tracks{};
    std::array<Feedback, MaxTracks> pending{};
    std::array<Candidate, MaxTracks> candidates{};
    std::array<std::array<Candidate, MaxAliases>, MaxTracks> variants{};
    std::array<std::array<IdentityMatch, MaxAliases>, MaxTracks> variant_identity{};
    std::array<bool, PeakCap> ambiguous_peak{};
    Spectrum unresolved_support{};
    std::array<Spectrum, MaxPrototypes> reference_power{}, reference_envelopes{};
    Spectrum claimed{}, noise{}, prediction{}, unit_gate{}, solve_weight{};
    std::array<Spectrum, MaxBasis> dictionary{};
    std::array<Ownership, MaxBasis> basis_owner{};
    std::array<std::uint64_t, MaxBasis> basis_track{};
    std::array<float, MaxBasis> coefficient{};
    std::array<float, MaxBasis> coefficient_prior{}, next_coefficient{};
    std::array<unsigned, MaxBasis> basis_kind{};
    std::array<Spectrum, MaxBasis> cached_dictionary{};
    std::array<float, MaxBasis> cached_coefficient{};
    std::array<Ownership, MaxBasis> cached_owner{};
    std::array<std::uint64_t, MaxBasis> cached_track{};
    std::array<unsigned, MaxBasis> cached_kind{};
    unsigned cached_count = 0;
    std::uint64_t cached_frame = 0;
    std::array<Beam, MaxBeam> beam{}, next_beam{};
    unsigned candidate_count = 0, basis_count = 0;
    float prior_total = 0;
    State(const Config& c, const Profile& p) : config(c), profile(p) {
        unit_gate.fill(1);
        for (unsigned i = 0; i < p.prototype_count; ++i) {
            // Schema 3 averages per-frame absolute-Hz source envelopes, not
            // raw combs smeared across varying F0s. Both identity operands are
            // conditioned once by the actual independent observation mask.
            reference_power[i] = p.prototypes[i].power_basis;
            reference_envelopes[i] = reference_power[i];
        }
        noise.fill(1.0e-8f);
    }
    void clear(std::uint64_t new_epoch) noexcept {
        epoch = new_epoch; frame = 0; next_track = 1; history_count = history_write = 0;
        frontend.clear(); tracks = {}; pending = {}; candidates = {}; variants = {}; variant_identity = {}; history = {};
        claimed.fill(0); noise.fill(1.0e-8f); prediction.fill(0); coefficient.fill(0);
        coefficient_prior.fill(0); cached_dictionary = {}; cached_coefficient.fill(0); cached_count = 0; cached_frame = 0;
        ambiguous_peak.fill(false); unresolved_support.fill(0);
        candidate_count = basis_count = 0; prior_total = 0;
    }
    void invalidate_audio_state() noexcept {
        // Recoverable internal faults preserve the continuous input/output
        // positions, but invalidate ALL acoustic history. The caller receives
        // state_reset and must establish a new transport epoch before restart.
        const auto saved_frame = frame, saved_track_id = next_track;
        clear(epoch); frame = saved_frame; next_track = saved_track_id;
    }
    unsigned commit_previous_feedback() noexcept {
        unsigned committed = 0;
        for (auto& track : tracks) if (track.active && track.trusted) {
            ++track.trusted_age;
            if (track.trusted_age > config.history_hops) {
                track.trusted = false; track.coefficient = 0; track.velocity = 0; track.trusted_spectrum.fill(0);
            }
        }
        for (auto& feedback : pending) {
            if (feedback.valid && ablation.feedback && ablation.identity && ablation.tracking &&
                feedback.epoch == epoch && feedback.frame + 1 == frame &&
                feedback.input_end + Hop == frame * Hop) {
                for (auto& track : tracks) if (track.active && track.id == feedback.track && track.misses == 0) {
                    if (feedback.f0 > 0 && track.f0 > 0) track.velocity = bounded(.75f * track.velocity + .25f * feedback.velocity, -12, 12);
                    track.trusted_spectrum = feedback.spectrum; track.trusted = true;
                    track.trusted_age = 0;
                    track.trusted_frame = feedback.frame;
                    track.coefficient = feedback.coefficient;
                    ++committed;
                    break;
                }
            }
            feedback.valid = false;
        }
        return committed;
    }
    const Spectrum& previous(unsigned age = 0) const noexcept {
        return history[(history_write + History - 1 - age) % History];
    }
    void affinity(Candidate& candidate) noexcept {
        auto& group = candidate.group;
        float weight = 0, flux_mean = 0, flux_square = 0, modulation = 0;
        std::array<float, History> aggregate{};
        const unsigned count = std::min(history_count, config.history_hops);
        for (unsigned age = 0; age < count; ++age) {
            float power = 0;
            for (std::size_t k = 0; k < Bins; ++k) power += previous(age)[k] * group.support[k];
            aggregate[age] = std::sqrt(power + Eps);
        }
        for (unsigned p = 0; p < analyzer.peak_count; ++p) {
            const unsigned bin = analyzer.peaks[p].bin; const float w = frontend.power[bin] * group.support[bin];
            if (w <= frontend.total * .0001f) continue;
            const float old = count ? previous()[bin] : 0;
            const float flux = (frontend.power[bin] - old) / (frontend.power[bin] + old + Eps);
            weight += w; flux_mean += w * flux; flux_square += w * flux * flux;
            float correlation = .5f;
            if (count >= 4) {
                float ma = 0, mb = 0;
                for (unsigned age = 0; age < count; ++age) { ma += aggregate[age]; mb += std::sqrt(previous(age)[bin] + Eps); }
                ma /= float(count); mb /= float(count); float covariance = 0, va = 0, vb = 0;
                for (unsigned age = 0; age < count; ++age) {
                    const float a = aggregate[age] - ma, b = std::sqrt(previous(age)[bin] + Eps) - mb;
                    covariance += a * b; va += a * a; vb += b * b;
                }
                if (va * vb > 1.0e-16f) correlation = unit(.5f + .5f * covariance / std::sqrt(va * vb));
            }
            modulation += w * correlation;
        }
        if (weight > Eps) {
            flux_mean /= weight; flux_square /= weight;
            group.onset = unit(1.f - 2.f * std::sqrt(std::max(0.f, flux_square - flux_mean * flux_mean)));
            group.modulation = unit(modulation / weight);
        } else { group.onset = .5f; group.modulation = .5f; }
        // Onset and AM constrain grouping, not reference identity. Low-F0
        // frames are discounted because zero padding does not add cycles.
        const float cycles = group.voiced ? unit(group.f0 * float(Window) / float(SampleRate) / 3) : 1;
        group.reliability = unit((group.voiced ? group.harmonic * (.55f + .45f * cycles) : .70f) *
                                 (.70f + .15f * group.onset + .15f * group.modulation));
        if (!ablation.reliability) group.reliability = 1;
    }
    void observe_candidate(Candidate& candidate) noexcept {
        auto& group = candidate.group;
        for (std::size_t k = 0; k < Bins; ++k) candidate.power[k] = frontend.power[k] * group.support[k];
        group.energy = sum(candidate.power); group.valid = group.energy > Eps;
        group.missing_reason = group.valid ? MissingReason::None : MissingReason::LowEnergy;
        normalize(candidate.power); source_envelope(candidate.power, group.f0, group.voiced,
            analyzer, ablation.grouping ? &candidate.peak_membership : nullptr, candidate.envelope);
        candidate.occupied = features(candidate.envelope, group.f0, group.voiced, candidate.feature);
        affinity(candidate);
    }
    void refine_membership(Candidate& candidate) noexcept {
        // Historical common onset and AM affect component MEMBERSHIP, not
        // merely a post-hoc identity reliability. No future samples are read.
        const unsigned count = std::min(history_count, config.history_hops);
        if (count < 2) return;
        std::array<float, History> group_amplitude{};
        float current = 0, old = 0;
        for (std::size_t k = 1; k < Bins; ++k) {
            current += frontend.power[k] * candidate.group.support[k]; old += previous()[k] * candidate.group.support[k];
        }
        const float group_flux = (current - old) / (current + old + Eps);
        float group_mean = 0;
        for (unsigned age = 0; age < count; ++age) {
            float power = 0;
            for (std::size_t k = 1; k < Bins; ++k) power += previous(age)[k] * candidate.group.support[k];
            group_amplitude[age] = std::sqrt(power + Eps); group_mean += group_amplitude[age];
        }
        group_mean /= float(count);
        for (std::size_t k = 1; k < Bins; ++k) if (candidate.group.support[k] > 0) {
            const float bin_flux = (frontend.power[k] - previous()[k]) / (frontend.power[k] + previous()[k] + Eps);
            const float onset_agreement = unit(1 - .5f * std::abs(bin_flux - group_flux));
            float modulation = .5f;
            if (count >= 4) {
                float bin_mean = 0;
                for (unsigned age = 0; age < count; ++age) bin_mean += std::sqrt(previous(age)[k] + Eps);
                bin_mean /= float(count); float covariance = 0, va = 0, vb = 0;
                for (unsigned age = 0; age < count; ++age) {
                    const float a = group_amplitude[age] - group_mean, b = std::sqrt(previous(age)[k] + Eps) - bin_mean;
                    covariance += a * b; va += a * a; vb += b * b;
                }
                if (va * vb > 1.0e-16f) modulation = unit(.5f + .5f * covariance / std::sqrt(va * vb));
            }
            candidate.group.support[k] *= .55f + .25f * onset_agreement + .20f * modulation;
        }
    }
    void finish_candidate(Candidate& candidate, unsigned id) noexcept {
        auto& group = candidate.group; group.group_id = id;
        candidate.observation_support = group.support;
        if (ablation.grouping) refine_membership(candidate);
        observe_candidate(candidate);
        candidate.alias_f0[0] = group.f0; candidate.alias_cost[0] = 0; candidate.alias_count = 1;
        if (group.voiced && config.max_aliases >= 2 && group.f0 / 2 >= 60) {
            candidate.alias_f0[candidate.alias_count] = group.f0 / 2; candidate.alias_cost[candidate.alias_count++] = .30f;
        }
        if (group.voiced && candidate.alias_count < config.max_aliases && group.f0 * 2 <= 840) {
            candidate.alias_f0[candidate.alias_count] = group.f0 * 2; candidate.alias_cost[candidate.alias_count++] = .40f;
        }
    }
    void group_sources(FrameResult& result) noexcept {
        candidates = {}; candidate_count = 0; claimed.fill(0); ambiguous_peak.fill(false); unresolved_support.fill(0);
        analyzer.find_peaks(frontend.power);
        if (ablation.scheduling && frontend.rms < config.minimum_rms) return;
        analyzer.find_pitches(); result.budget_exhausted |= analyzer.saturated;
        if (!ablation.grouping) {
            auto& c = candidates[0]; c.group.support.fill(1); c.group.support[0] = 0;
            for (unsigned p = 0; p < analyzer.peak_count; ++p) c.peak_membership[p] = true;
            c.group.f0 = analyzer.pitch_count ? analyzer.pitches[0].f0 : 0;
            c.group.voiced = analyzer.pitch_count && analyzer.pitches[0].harmonic > .45f;
            finish_candidate(c, 1); c.group.harmonic = c.group.onset = c.group.modulation = 0;
            c.group.reliability = ablation.reliability ? .65f : 1; candidate_count = c.group.valid ? 1 : 0;
            claimed = c.group.support; return;
        }
        std::array<bool, PeakCap> explained{};
        float peak_energy = 0;
        for (unsigned p = 0; p < analyzer.peak_count; ++p) peak_energy += analyzer.peaks[p].power;
        for (unsigned iteration = 0; iteration <= config.max_tracks; ++iteration) {
            unsigned best = analyzer.pitch_count; float best_gain = 0;
            for (unsigned p = 0; p < analyzer.pitch_count; ++p) {
                const auto& pitch = analyzer.pitches[p]; bool same_family = false;
                for (unsigned g = 0; g < candidate_count; ++g)
                    if (aliases(pitch.f0, candidates[g].group.f0)) same_family = true;
                if (same_family) continue;
                unsigned novel_hits = 0, lower_hits = 0; float novel_energy = 0, gain = 0;
                std::array<bool, 9> lower_harmonics{};
                for (unsigned k = 0; k < analyzer.peak_count; ++k) {
                    const float agreement = peak_agreement(pitch.f0, analyzer.peaks[k]);
                    if (agreement <= 0) continue;
                    const unsigned harmonic = unsigned(std::lround(analyzer.peaks[k].frequency / pitch.f0));
                    if (harmonic < lower_harmonics.size() && !lower_harmonics[harmonic]) {
                        lower_harmonics[harmonic] = true; ++lower_hits;
                    }
                    if (explained[k]) continue;
                    ++novel_hits; novel_energy += analyzer.peaks[k].power;
                    gain += agreement * std::sqrt(analyzer.peaks[k].power) / std::sqrt(float(harmonic));
                }
                // Binary observed-peak inhibition is separate from soft output
                // membership. The same explained peak cannot supply "novel"
                // energy to an infinity of subharmonic/partial aliases.
                if (novel_hits < 3 || lower_hits < 2 || novel_energy < peak_energy * .065f) continue;
                gain *= (.30f + .70f * float(lower_hits) / 8.f) * unit(float(novel_hits) / 5.f);
                if (gain > best_gain) { best = p; best_gain = gain; }
            }
            if (best == analyzer.pitch_count) break;
            if (candidate_count >= config.max_tracks) { result.budget_exhausted = true; break; }
            const auto& pitch = analyzer.pitches[best]; auto& c = candidates[candidate_count];
            c.group.f0 = pitch.f0; c.group.voiced = true;
            actual_peak_support(analyzer, pitch.f0, c.group.support, c.peak_membership);
            float agreement_sum = 0, assigned_energy = 0;
            for (unsigned p = 0; p < analyzer.peak_count; ++p) if (c.peak_membership[p]) {
                assigned_energy += analyzer.peaks[p].power;
                agreement_sum += analyzer.peaks[p].power * peak_agreement(pitch.f0, analyzer.peaks[p]);
                explained[p] = true;
            }
            c.group.harmonic = unit(agreement_sum / (assigned_energy + Eps));
            // The hard structural coverage used for residual membership is the
            // observed peak footprint BEFORE AM/onset softness is applied.
            for (std::size_t k = 0; k < Bins; ++k) claimed[k] = std::max(claimed[k], c.group.support[k]);
            finish_candidate(c, candidate_count + 1); ++candidate_count;
        }
        // A family with two independent unexplained peaks is not promoted to
        // a speaker/track (the three-peak source gate is unchanged). It is a
        // bounded PARTIAL unknown explanation: it can expose ambiguity in an
        // otherwise apparently exclusive line of an accepted source.
        analyzer.find_partial_pitches(); result.budget_exhausted |= analyzer.saturated;
        std::array<float, MaxTracks> partial_f0{}; unsigned partial_count = 0;
        auto residual_explained = explained;
        for (unsigned iteration = 0; iteration < config.max_tracks; ++iteration) {
            unsigned best = analyzer.partial_pitch_count; float best_gain = 0;
            for (unsigned p = 0; p < analyzer.partial_pitch_count; ++p) {
                const float f0 = analyzer.partial_pitches[p].f0; bool duplicate = false;
                for (unsigned g = 0; g < candidate_count; ++g) duplicate |= aliases(f0, candidates[g].group.f0);
                for (unsigned j = 0; j < partial_count; ++j) duplicate |= aliases(f0, partial_f0[j]);
                if (duplicate) continue;
                unsigned independent = 0; float energy = 0, gain = 0;
                for (unsigned peak = 0; peak < analyzer.peak_count; ++peak) {
                    if (residual_explained[peak]) continue;
                    const float agreement = peak_agreement(f0, analyzer.peaks[peak]); if (agreement <= 0) continue;
                    ++independent; energy += analyzer.peaks[peak].power;
                    gain += agreement * std::sqrt(analyzer.peaks[peak].power);
                }
                if (independent < 2 || energy < peak_energy * .02f) continue;
                if (gain > best_gain) { best = p; best_gain = gain; }
            }
            if (best == analyzer.partial_pitch_count) break;
            const float f0 = analyzer.partial_pitches[best].f0; partial_f0[partial_count++] = f0;
            Spectrum support{}; std::array<bool, PeakCap> membership{};
            actual_peak_support(analyzer, f0, support, membership);
            for (unsigned p = 0; p < analyzer.peak_count; ++p) if (membership[p]) {
                if (explained[p]) ambiguous_peak[p] = true;
                residual_explained[p] = true;
            }
            for (std::size_t k = 0; k < Bins; ++k) unresolved_support[k] = std::max(unresolved_support[k], support[k]);
        }
        // One observed atom claimed by two accepted families is also U in the
        // current single-channel approximation; knowing one identity does not
        // magically make a shared mixture component a clean target component.
        for (unsigned p = 0; p < analyzer.peak_count; ++p) {
            unsigned owners = 0;
            for (unsigned g = 0; g < candidate_count; ++g) owners += candidates[g].peak_membership[p];
            if (owners < 2) continue;
            ambiguous_peak[p] = true;
            const float center = analyzer.peaks[p].frequency * float(FftSize) / float(SampleRate);
            for (int k = std::max(1, int(std::floor(center - 2.2f)));
                 k <= std::min(int(Bins) - 1, int(std::ceil(center + 2.2f))); ++k) {
                const float distance = std::abs(float(k) - center);
                unresolved_support[std::size_t(k)] = std::max(unresolved_support[std::size_t(k)],
                    distance <= 1.2f ? 1.f : unit(2.2f - distance));
            }
        }
        result.partial_families = partial_count;
        for (bool ambiguous : ambiguous_peak) result.ambiguous_peaks += ambiguous;
        float residual = 0, high = 0;
        for (std::size_t k = 1; k < Bins; ++k) {
            const float p = frontend.power[k] * (1 - claimed[k]); residual += p; if (k >= 32) high += p;
        }
        // Unvoiced structures are represented without inventing an F0. Their
        // identity must still be supported by registered unvoiced statistics.
        if (residual > frontend.total * .12f && (candidate_count == 0 || high > residual * .40f)) {
            if (candidate_count < config.max_tracks) {
                auto& c = candidates[candidate_count];
                for (std::size_t k = 1; k < Bins; ++k) c.group.support[k] = 1 - claimed[k];
                c.group.voiced = false; finish_candidate(c, candidate_count + 1); ++candidate_count;
            } else result.budget_exhausted = true;
        }
        if (!candidate_count && frontend.rms >= config.minimum_rms) {
            auto& c = candidates[0]; c.group.support.fill(1); c.group.support[0] = 0;
            finish_candidate(c, 1); candidate_count = 1;
        }
    }
    void observe_aliases(FrameResult& result) noexcept {
        variants = {}; variant_identity = {};
        for (unsigned g = 0; g < candidate_count; ++g) {
            const auto& original = candidates[g];
            for (unsigned a = 0; a < original.alias_count; ++a) {
                auto& variant = variants[g][a]; variant = original;
                if (a != 0 && original.group.voiced) {
                    variant.group.f0 = original.alias_f0[a];
                    Spectrum support{};
                    actual_peak_support(analyzer, variant.group.f0, support, variant.peak_membership);
                    // An octave reinterpretation may remove/reweight observed
                    // support; it may NOT conjure target energy outside the
                    // canonical group's already observed support.
                    for (std::size_t k = 0; k < Bins; ++k) {
                        variant.group.support[k] = std::min(original.group.support[k], support[k]);
                        variant.observation_support[k] = std::min(original.observation_support[k], support[k]);
                    }
                    for (unsigned p = 0; p < analyzer.peak_count; ++p)
                        variant.peak_membership[p] = variant.peak_membership[p] && original.peak_membership[p];
                    float agreement = 0, weight = 0;
                    for (unsigned p = 0; p < analyzer.peak_count; ++p) if (variant.peak_membership[p]) {
                        weight += analyzer.peaks[p].power;
                        agreement += analyzer.peaks[p].power * peak_agreement(variant.group.f0, analyzer.peaks[p]);
                    }
                    variant.group.harmonic = unit(agreement / (weight + Eps));
                    observe_candidate(variant);
                }
                variant.identity_support.fill(0); variant.identity_power.fill(0);
                variant.identity_membership.fill(false); variant.independent_peaks = 0;
                if (!ablation.grouping) {
                    // Diagnostic baseline only: one whole MIXTURE view, not
                    // an independently separated/clean source. Disabling the
                    // grouper must not also silently disable the matcher.
                    variant.identity_support = variant.group.support;
                    variant.identity_membership = variant.peak_membership;
                    for (unsigned p = 0; p < analyzer.peak_count; ++p)
                        variant.independent_peaks += variant.identity_membership[p] &&
                            (!variant.group.voiced || peak_agreement(variant.group.f0, analyzer.peaks[p]) > 0);
                } else if (variant.group.voiced) {
                    for (unsigned p = 0; p < analyzer.peak_count; ++p) if (variant.peak_membership[p]) {
                        bool shared = ambiguous_peak[p];
                        for (unsigned j = 0; j < candidate_count; ++j)
                            if (j != g && candidates[j].group.voiced && candidates[j].peak_membership[p]) shared = true;
                        if (shared) continue;
                        ++variant.independent_peaks;
                        variant.identity_membership[p] = true;
                        const float center = analyzer.peaks[p].frequency * float(FftSize) / float(SampleRate);
                        for (int bin = std::max(1, int(std::floor(center - 2.2f)));
                             bin <= std::min(int(Bins) - 1, int(std::ceil(center + 2.2f))); ++bin) {
                            const auto k = std::size_t(bin); const float distance = std::abs(float(bin) - center);
                            // Unresolved boundaries toward another observed
                            // family's peak remain missing, not target evidence.
                            float nearest_other = std::numeric_limits<float>::infinity();
                            for (unsigned other = 0; other < analyzer.peak_count; ++other) {
                                // An unassigned observed peak is unknown, not
                                // clean target evidence merely because no full
                                // voiced family passed the source-budget gate.
                                bool belongs_other = !variant.peak_membership[other];
                                for (unsigned j = 0; j < candidate_count; ++j)
                                    belongs_other |= j != g && candidates[j].group.voiced && candidates[j].peak_membership[other];
                                if (belongs_other) nearest_other = std::min(nearest_other,
                                    std::abs(float(bin) - analyzer.peaks[other].frequency * float(FftSize) / float(SampleRate)));
                            }
                            if (nearest_other < distance + .35f) continue;
                            const float w = distance <= 1.2f ? 1.f : unit(2.2f - distance);
                            variant.identity_support[k] = std::max(variant.identity_support[k], w);
                        }
                    }
                } else {
                    variant.identity_support = variant.group.support;
                    for (std::size_t k = 0; k < Bins; ++k)
                        variant.identity_support[k] *= 1 - unresolved_support[k];
                    for (unsigned j = 0; j < candidate_count; ++j) if (j != g && candidates[j].group.voiced)
                        for (std::size_t k = 0; k < Bins; ++k)
                            variant.identity_support[k] *= 1 - candidates[j].group.support[k];
                }
                float usable_energy = 0, supported_energy = 0;
                for (std::size_t k = 0; k < Bins; ++k) {
                    variant.identity_power[k] = frontend.power[k] * variant.identity_support[k];
                    usable_energy += variant.identity_power[k];
                    supported_energy += frontend.power[k] * variant.observation_support[k];
                }
                variant.exclusive_fraction = unit(usable_energy / (supported_energy + Eps));
                normalize(variant.identity_power);
                auto& identity = variant_identity[g][a];
                if (ablation.identity) identity = match(variant, profile, reference_power, analyzer, ablation.reliability);
                else identity.missing_reason = MissingReason::DisabledForAblation;
                // Low energy remains a missing identity observation even with
                // fixed/full scheduling. The scheduler is not the VAD gate.
                if (frontend.rms < config.minimum_rms) {
                    identity.valid = false; identity.reliability = 0; identity.missing_reason = MissingReason::LowEnergy;
                }
            }
            result.grouping[g] = variants[g][0].group; result.identity[g] = variant_identity[g][0];
        }
    }
    void associate(FrameResult& result) noexcept {
        std::array<bool, MaxTracks> used{};
        if (!ablation.tracking) { tracks = {}; pending = {}; }
        for (unsigned g = 0; g < candidate_count; ++g) {
            auto& c = candidates[g]; float best = std::numeric_limits<float>::infinity(); int slot = -1;
            if (ablation.tracking) for (unsigned t = 0; t < config.max_tracks; ++t) {
                const auto& track = tracks[t]; if (!track.active || used[t]) continue;
                float pitch_cost = 0;
                if (c.group.voiced && track.voiced) {
                    pitch_cost = 4;
                    for (unsigned a = 0; a < c.alias_count; ++a)
                        pitch_cost = std::min(pitch_cost, cents(c.alias_f0[a], std::max(60.f, track.f0 + track.velocity)) / 600.f + c.alias_cost[a]);
                } else if (c.group.voiced != track.voiced) pitch_cost = .45f;
                const float spectral = 1 - cosine(c.envelope, track.envelope);
                const float energy = std::abs(std::log((c.group.energy + Eps) / (track.energy + Eps)));
                const float cost = pitch_cost + 1.1f * spectral + .10f * std::min(energy, 4.f) + .06f * float(track.misses);
                if (cost < best) { best = cost; slot = int(t); }
            }
            bool born = false;
            if (slot < 0 || best > 1.7f) {
                slot = -1;
                for (unsigned t = 0; t < config.max_tracks; ++t) if (!used[t] && !tracks[t].active) { slot = int(t); break; }
                if (slot < 0) {
                    unsigned oldest = 0;
                    for (unsigned t = 1; t < config.max_tracks; ++t) if (!used[t] && (used[oldest] || tracks[t].misses > tracks[oldest].misses)) oldest = t;
                    if (!used[oldest] && tracks[oldest].misses >= 2) slot = int(oldest);
                }
                if (slot < 0) { result.budget_exhausted = true; continue; }
                auto& track = tracks[std::size_t(slot)]; track = {}; track.active = true; track.id = next_track++;
                born = true; best = 1;
            }
            c.slot = slot; used[std::size_t(slot)] = true; auto& track = tracks[std::size_t(slot)];
            auto& a = result.association[g]; a.track_id = track.id; a.transition_cost = best;
            a.born = born; a.paused = track.misses > 0; a.valid = ablation.tracking;
            a.observation_age_hops = track.misses;
            a.missing_reason = ablation.tracking ? MissingReason::None : MissingReason::DisabledForAblation;
            a.reliability = ablation.tracking ? (ablation.reliability ? unit(std::exp(-best) * (born ? .5f : 1)) : 1) : 0;
            for (unsigned alias = 0; alias < c.alias_count; ++alias) {
                if (!born && track.voiced && c.group.voiced)
                    c.alias_cost[alias] += .15f * cents(c.alias_f0[alias], std::max(60.f, track.f0 + track.velocity)) / 600.f;
            }
        }
        for (unsigned t = 0; t < config.max_tracks; ++t) {
            auto& track = tracks[t]; if (!track.active) continue;
            ++track.age;
            if (!used[t]) {
                ++track.misses; track.velocity *= .8f;
                if (track.misses > config.missed_track_hops) track = {};
            }
        }
    }
    float local_cost(unsigned g, unsigned alias, Ownership owner, const FrameResult& result) const noexcept {
        const auto& c = variants[g][alias]; const auto& id = variant_identity[g][alias];
        const float q = id.valid ? id.reliability : 0;
        const float score = id.valid ? id.relative_score - config.identity_margin : 0;
        float cost = 0;
        switch (owner) {
        case Ownership::Target:
            if (!id.valid || q < .35f || score <= .10f) return 100.f;
            cost = softplus(-3 * score) * q + 1.5f * (1 - q); break;
        case Ownership::Interference:
            cost = softplus(3 * score) * q + .65f * (1 - q);
            if (std::abs(score) < .20f || !id.valid) cost += .40f; break;
        case Ownership::Noise:
            // Broadband, low-structure observations may have a noise
            // explanation without any speaker model. Structured unvoiced
            // observations remain U unless independent identity supports them.
            cost = c.group.voiced ? 2.6f : .02f + (1 - frontend.flatness) * .20f + q * std::max(score, 0.f); break;
        default:
            cost = .15f + .85f * q + .15f * std::abs(score) * q; break;
        }
        if (ablation.tracking && c.slot >= 0) {
            const auto& track = tracks[std::size_t(c.slot)]; const auto& association = result.association[g];
            if (!association.born && track.previous != Ownership::Unknown && track.previous != owner)
                cost += .28f * association.reliability;
        }
        return cost;
    }
    float pair_cost(unsigned a, unsigned b, unsigned alias_a, unsigned alias_b, Ownership za, Ownership zb) const noexcept {
        const auto& ga = variants[a][alias_a].group; const auto& gb = variants[b][alias_b].group;
        float overlap = 0, amount = 0;
        for (std::size_t k = 0; k < Bins; ++k) {
            overlap += frontend.power[k] * std::min(ga.support[k], gb.support[k]);
            amount += frontend.power[k] * std::max(ga.support[k], gb.support[k]);
        }
        overlap /= amount + Eps;
        float cost = 0;
        if (za != Ownership::Unknown && zb != Ownership::Unknown) {
            cost += (za == zb ? 1.4f : .45f) * overlap;
            if (za == Ownership::Target && zb == Ownership::Target) cost += 1.8f * overlap;
        }
        if (ablation.grouping && (za == Ownership::Target) != (zb == Ownership::Target) &&
            za != Ownership::Unknown && zb != Ownership::Unknown) {
            const float source_compatibility = ga.voiced && gb.voiced ? unit(1 - cents(ga.f0, gb.f0) / 300) : .35f;
            const float affinity = source_compatibility * ga.onset * gb.onset * ga.modulation * gb.modulation;
            cost += .8f * affinity;
        }
        return cost;
    }
    float compete(FrameResult& result) noexcept {
        constexpr std::array<Ownership, 4> labels = {Ownership::Unknown, Ownership::Target, Ownership::Interference, Ownership::Noise};
        beam = {}; unsigned count = 1;
        if (!ablation.competition) {
            for (unsigned g = 0; g < candidate_count; ++g) {
                float best = std::numeric_limits<float>::infinity();
                for (Ownership owner : labels) { const float cost = local_cost(g, 0, owner, result); if (cost < best) { best = cost; result.ownership[g] = owner; } }
            }
            // No fabricated joint gap: competition ablation cannot qualify
            // reconstructed feedback by inventing an ownership alternative.
            result.beam_states = candidate_count ? 1 : 0; return 0.f;
        }
        for (unsigned g = 0; g < candidate_count; ++g) {
            unsigned next_count = 0; next_beam = {};
            for (unsigned b = 0; b < count; ++b) for (Ownership owner : labels) {
                for (unsigned alias = 0; alias < candidates[g].alias_count; ++alias) {
                    const float base_cost = local_cost(g, alias, owner, result); if (base_cost >= 100) continue;
                    Beam proposal = beam[b]; proposal.owner[g] = owner; proposal.alias[g] = alias;
                    proposal.cost += base_cost + candidates[g].alias_cost[alias];
                    for (unsigned j = 0; j < g; ++j)
                        proposal.cost += pair_cost(j, g, proposal.alias[j], alias, proposal.owner[j], owner);
                    unsigned at = next_count;
                    if (at >= config.beam_width) { if (proposal.cost >= next_beam[at - 1].cost) continue; --at; }
                    else ++next_count;
                    while (at && next_beam[at - 1].cost > proposal.cost) { next_beam[at] = next_beam[at - 1]; --at; }
                    next_beam[at] = proposal;
                }
            }
            beam = next_beam; count = next_count;
        }
        result.beam_states = candidate_count ? count : 0;
        if (!count) return 0;
        for (unsigned g = 0; g < candidate_count; ++g) {
            result.ownership[g] = beam[0].owner[g];
            const unsigned selected = beam[0].alias[g];
            candidates[g] = variants[g][selected];
            result.grouping[g] = candidates[g].group; result.identity[g] = variant_identity[g][selected];
            if (candidates[g].slot >= 0) {
                auto& track = tracks[std::size_t(candidates[g].slot)];
                track.alias_count = candidates[g].alias_count;
                track.alias_f0 = candidates[g].alias_f0;
                auto& association = result.association[g];
                if (!association.born && ablation.tracking) {
                    float pitch_cost = 0;
                    if (candidates[g].group.voiced && track.voiced)
                        pitch_cost = cents(candidates[g].group.f0, std::max(60.f, track.f0 + track.velocity)) / 600.f;
                    else if (candidates[g].group.voiced != track.voiced) pitch_cost = .45f;
                    const float energy_cost = std::min(4.f, std::abs(std::log((candidates[g].group.energy + Eps) / (track.energy + Eps))));
                    association.transition_cost = pitch_cost + 1.1f * (1 - cosine(candidates[g].envelope, track.envelope)) +
                                                  .10f * energy_cost + .06f * float(track.misses);
                    association.reliability = ablation.reliability ? unit(std::exp(-association.transition_cost)) : 1.f;
                }
            }
        }
        // Compare alternative LABEL explanations, not merely two F0 aliases
        // of the same labels, for feedback's ownership-gap guard.
        // If beam pruning retained only alias variations of the winning
        // labels, there is no measured alternative-label gap. Conservatively
        // refuse feedback rather than inventing confidence from absent states.
        float gap = 0;
        for (unsigned b = 1; b < count; ++b) {
            bool different = false;
            for (unsigned g = 0; g < candidate_count; ++g) different |= beam[b].owner[g] != beam[0].owner[g];
            if (different) { gap = std::max(0.f, beam[b].cost - beam[0].cost); break; }
        }
        return gap;
    }
    void add_basis(Spectrum basis, Ownership owner, std::uint64_t track, unsigned kind, FrameResult& result) noexcept {
        if (normalize(basis) <= Eps) return;
        // Keep one dictionary slot for explicit U even at minimum valid
        // configured capacity. Source/reference columns must not consume it
        // and silently force all ambiguous fitted energy into a target column.
        const unsigned capacity = kind == 3 ? config.max_basis : config.max_basis - 1;
        if (basis_count >= capacity) { result.budget_exhausted = true; return; }
        dictionary[basis_count] = basis; basis_owner[basis_count] = owner; basis_track[basis_count] = track;
        basis_kind[basis_count] = kind;
        coefficient[basis_count] = 0; coefficient_prior[basis_count] = 0;
        bool historical_target_qualified = owner != Ownership::Target;
        if (owner == Ownership::Target && ablation.feedback && ablation.tracking)
            for (const auto& t : tracks) if (t.active && t.id == track && t.trusted &&
                t.trusted_age <= config.history_hops && t.trusted_frame == cached_frame)
                historical_target_qualified = true;
        if (historical_target_qualified && cached_frame < frame && frame - cached_frame <= config.history_hops) {
            float closest = .75f;
            for (unsigned old = 0; old < cached_count; ++old) {
                if (cached_owner[old] != owner || cached_track[old] != track || cached_kind[old] != kind) continue;
                const float agreement = cosine(cached_dictionary[old], basis);
                if (agreement > closest) {
                    closest = agreement;
                    coefficient_prior[basis_count] = bounded(cached_coefficient[old], 0, frontend.total * 1.5f);
                }
            }
        }
        ++basis_count;
    }
    void solve(FrameResult& result, unsigned iterations) noexcept {
        basis_count = 0; coefficient.fill(0); coefficient_prior.fill(0); prediction.fill(0); claimed.fill(0);
        for (unsigned g = 0; g < candidate_count; ++g) {
            const auto& c = candidates[g]; const auto owner = result.ownership[g];
            const auto tid = c.slot >= 0 ? tracks[std::size_t(c.slot)].id : 0;
            Spectrum observed = c.power;
            if (owner == Ownership::Target) {
                const auto& reference = reference_envelopes[result.identity[g].prototype];
                for (std::size_t k = 0; k < Bins; ++k) {
                    const float ratio = bounded(reference[k] / (c.envelope[k] + Eps), .25f, 4.f);
                    observed[k] *= std::sqrt(ratio) * (1 - unresolved_support[k]);
                }
            }
            add_basis(observed, owner, tid, 0, result);
            if (owner == Ownership::Target) {
                Spectrum reference{};
                for (std::size_t k = 0; k < Bins; ++k)
                    reference[k] = reference_envelopes[result.identity[g].prototype][k] * c.group.support[k] * (1 - unresolved_support[k]);
                add_basis(reference, owner, tid, 1, result);
            } else if (c.slot >= 0 && tracks[std::size_t(c.slot)].trusted) {
                Spectrum prior{};
                for (std::size_t k = 0; k < Bins; ++k) prior[k] = tracks[std::size_t(c.slot)].trusted_spectrum[k] * c.group.support[k];
                add_basis(prior, owner, tid, 2, result);
            }
            for (std::size_t k = 0; k < Bins; ++k) claimed[k] = std::max(claimed[k], c.group.support[k]);
        }
        Spectrum noise_basis{}, unknown{};
        for (std::size_t k = 0; k < Bins; ++k) {
            noise_basis[k] = noise[k] * (.15f + .85f * (1 - claimed[k]));
            unknown[k] = frontend.power[k] * std::max(1 - claimed[k], unresolved_support[k]);
        }
        // Unknown is allocated before noise when configured capacity is low.
        add_basis(unknown, Ownership::Unknown, 0, 3, result);
        add_basis(noise_basis, Ownership::Noise, 0, 4, result);
        // A new source gets a bounded current-observation seed, not a copied
        // historical person's activation. Existing seeds are mapped by
        // track+owner+basis role+spectral shape. The target mapping above is
        // additionally gated by an actually committed qualified feedback frame.
        for (unsigned j = 0; j < basis_count; ++j) {
            float observed_seed = 0;
            if (basis_kind[j] <= 2) {
                unsigned related = 0;
                for (unsigned i = 0; i < basis_count; ++i)
                    related += basis_kind[i] <= 2 && basis_owner[i] == basis_owner[j] && basis_track[i] == basis_track[j];
                for (unsigned g = 0; g < candidate_count; ++g) {
                    const auto& c = candidates[g];
                    const auto tid = c.slot >= 0 ? tracks[std::size_t(c.slot)].id : 0;
                    if (tid == basis_track[j] && result.ownership[g] == basis_owner[j])
                        observed_seed += c.group.energy / float(std::max(1u, related));
                }
            } else if (basis_kind[j] == 3) observed_seed = sum(unknown);
            coefficient[j] = bounded(coefficient_prior[j] > 0 ?
                                     .5f * coefficient_prior[j] + .5f * observed_seed : observed_seed,
                                     0, frontend.total * 1.5f);
        }
        float frobenius = 0;
        for (std::size_t k = 0; k < Bins; ++k) {
            float valid_support = 0;
            for (unsigned g = 0; g < candidate_count; ++g)
                valid_support = std::max(valid_support, candidates[g].group.support[k] * candidates[g].group.reliability);
            solve_weight[k] = k == 0 ? 0 : (ablation.reliability ? .25f + .75f * unit(valid_support) : 1.f);
            for (unsigned j = 0; j < basis_count; ++j) frobenius += solve_weight[k] * dictionary[j][k] * dictionary[j][k];
        }
        const float step = 1.f / (frobenius + HistoryRegularization + Eps);
        const auto predict = [this]() noexcept {
            prediction.fill(0);
            for (unsigned j = 0; j < basis_count; ++j)
                for (std::size_t k = 0; k < Bins; ++k) prediction[k] += dictionary[j][k] * coefficient[j];
        };
        const auto objective = [this]() noexcept {
            double value = 0;
            for (std::size_t k = 0; k < Bins; ++k) {
                const double residual = prediction[k] - frontend.power[k]; value += .5 * solve_weight[k] * residual * residual;
            }
            for (unsigned j = 0; j < basis_count; ++j) {
                const double deviation = coefficient[j] - coefficient_prior[j]; value += .5 * HistoryRegularization * deviation * deviation;
            }
            return value;
        };
        predict(); result.solver_objective_before = objective();
        result.basis_count = basis_count; result.solver_finite = true;
        for (unsigned iteration = 0; iteration < iterations; ++iteration) {
            // True simultaneous projected gradient for specification 3.6:
            // 1/2 ||sqrt(W)(Db-V)||^2 + lambda/2 ||b-mapped_prior||^2.
            // eta <= 1/(||sqrt(W)D||_F^2+lambda+epsilon). At most three
            // iterations are an explicit budget, not a convergence claim.
            for (unsigned j = 0; j < basis_count; ++j) {
                float gradient = 0;
                for (std::size_t k = 0; k < Bins; ++k) {
                    gradient += dictionary[j][k] * solve_weight[k] * (prediction[k] - frontend.power[k]);
                }
                gradient += HistoryRegularization * (coefficient[j] - coefficient_prior[j]);
                const float updated = bounded(coefficient[j] - step * gradient, 0, frontend.total * 1.5f);
                if (!std::isfinite(updated)) { result.solver_finite = false; return; }
                next_coefficient[j] = updated;
            }
            coefficient = next_coefficient; predict();
            ++result.solver_steps;
        }
        result.solver_objective_after = objective();
        result.solver_finite &= std::isfinite(result.solver_objective_before) && std::isfinite(result.solver_objective_after);
        for (unsigned j = 0; j < basis_count; ++j) {
            Spectrum* destination = &result.unknown_power;
            if (basis_owner[j] == Ownership::Target) destination = &result.target_power;
            else if (basis_owner[j] == Ownership::Interference) destination = &result.interference_power;
            else if (basis_owner[j] == Ownership::Noise) destination = &result.noise_power;
            for (std::size_t k = 0; k < Bins; ++k) (*destination)[k] += dictionary[j][k] * coefficient[j];
        }
        float error = 0;
        for (std::size_t k = 0; k < Bins; ++k) {
            const float fitted = result.target_power[k] + result.interference_power[k] + result.noise_power[k] + result.unknown_power[k];
            const float residual = frontend.power[k] - fitted;
            result.signed_residual_sum += residual;
            // Fitted U is not counted again as positive unexplained residual.
            result.unknown_power[k] += std::max(0.f, residual); error += std::abs(residual);
            result.solver_finite &= std::isfinite(fitted) && fitted >= 0;
        }
        result.absolute_residual_sum = error; result.residual_ratio = error / (frontend.total + Eps);
    }
    void cache_solver_result() noexcept {
        cached_count = basis_count; cached_frame = frame;
        for (unsigned j = 0; j < basis_count; ++j) {
            cached_dictionary[j] = dictionary[j]; cached_coefficient[j] = coefficient[j];
            cached_owner[j] = basis_owner[j]; cached_track[j] = basis_track[j]; cached_kind[j] = basis_kind[j];
        }
    }
    void direct_contribution(FrameResult& result) noexcept {
        // Explicit ablation: no dictionary, iterations or temporal priors.
        // Structural supports partition the observation proportionally instead.
        result.solver_finite = true;
        for (std::size_t k = 0; k < Bins; ++k) {
            float support = 0;
            for (unsigned g = 0; g < candidate_count; ++g) support += candidates[g].group.support[k];
            const float reserved_unknown = frontend.power[k] * unresolved_support[k];
            const float scale = (frontend.power[k] - reserved_unknown) / std::max(1.f, support);
            float explained = 0;
            for (unsigned g = 0; g < candidate_count; ++g) {
                const float power = scale * candidates[g].group.support[k]; explained += power;
                switch (result.ownership[g]) {
                case Ownership::Target: result.target_power[k] += power; break;
                case Ownership::Interference: result.interference_power[k] += power; break;
                case Ownership::Noise: result.noise_power[k] += power; break;
                default: result.unknown_power[k] += power; break;
                }
            }
            result.unknown_power[k] += reserved_unknown + std::max(0.f, frontend.power[k] - reserved_unknown - explained);
        }
    }
    void feedback(FrameResult& result, float ownership_gap) noexcept {
        // The feedback summarizes the CURRENT spectral contribution and fixed
        // observation, not the delayed mature OLA samples. Its explicit range
        // is [input_end-Window,input_end); it is consumed only at frame+1.
        for (unsigned g = 0; g < candidate_count; ++g) {
            auto& c = candidates[g]; if (c.slot < 0) continue;
            auto& track = tracks[std::size_t(c.slot)]; const auto& identity = result.identity[g];
            const bool qualified = ablation.feedback && ablation.identity && ablation.tracking && ablation.grouping &&
                result.ownership[g] == Ownership::Target && identity.valid && identity.relative_score > config.identity_margin + .45f &&
                identity.reliability >= .58f && c.group.reliability >= .60f && ownership_gap >= .20f &&
                result.residual_ratio <= .25f && result.solver_finite && result.target_confidence > .65f;
            if (qualified) {
                auto& f = pending[std::size_t(c.slot)]; f = {}; f.valid = true;
                f.epoch = epoch; f.frame = frame; f.input_end = result.input_end_sample; f.track = track.id;
                f.f0 = c.group.f0; f.energy = c.group.energy;
                f.velocity = c.group.voiced && track.voiced ? c.group.f0 - track.f0 : 0;
                for (std::size_t k = 0; k < Bins; ++k) f.spectrum[k] = result.target_power[k] * c.group.support[k];
                f.coefficient = normalize(f.spectrum); ++result.feedback_accepted;
            } else if (result.ownership[g] == Ownership::Target || track.trusted) {
                ++result.feedback_rejected;
                track.coefficient *= .75f; track.velocity *= .8f;
                if (!identity.valid || identity.relative_score < config.identity_margin - .2f || track.trusted_age >= config.history_hops) {
                    track.trusted = false; track.coefficient = 0; track.trusted_spectrum.fill(0);
                }
            }
            // Observation-based association state is separate from trusted
            // reconstructed feedback and never writes the immutable Profile.
            track.f0 = c.group.f0; track.voiced = c.group.voiced; track.energy = c.group.energy;
            track.envelope = c.envelope; track.previous = result.ownership[g]; track.misses = 0;
        }
    }
};

Engine::Engine(const Config& config, const Profile& profile) {
    if (!config.valid()) throw std::invalid_argument("Invalid or unsupported frozen MECAG configuration.");
    if (!profile.valid()) throw std::invalid_argument("Invalid or incompatible MECAG reference profile.");
    state_ = std::make_unique<State>(config, profile);
}
Engine::~Engine() = default;
void Engine::reset(std::uint64_t epoch) noexcept { state_->clear(epoch); }
void Engine::set_ablation(const Ablation& ablation) noexcept {
    state_->ablation = ablation;
    if (!ablation.feedback) { state_->pending = {}; for (auto& t : state_->tracks) { t.trusted = false; t.coefficient = 0; t.trusted_spectrum.fill(0); } }
}

void Engine::process_unity_hop(const std::array<float, Hop>& input, std::array<float, Hop>& output) noexcept {
    auto& s = *state_; ++s.frame;
    if (!s.frontend.analyze(input)) { s.invalidate_audio_state(); output.fill(0); return; }
    s.frontend.reconstruct(s.unit_gate, output);
}

void Engine::process_hop(const std::array<float, Hop>& input, std::array<float, Hop>& output, FrameResult& result) noexcept {
    const auto begin = std::chrono::steady_clock::now(); auto& s = *state_; result = FrameResult{};
    ++s.frame; result.epoch = s.epoch; result.frame = s.frame; result.input_end_sample = s.frame * Hop;
    result.output_begin_sample = std::int64_t(result.input_end_sample) - std::int64_t(Window);
    result.output_end_sample = result.output_begin_sample + std::int64_t(Hop);
    result.anchor_hash = s.profile.anchor_hash;
    result.config_hash = s.config.fingerprint();
    const auto elapsed = [&begin]() noexcept { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count(); };
    if (!s.frontend.analyze(input)) {
        for (unsigned g = 0; g < MaxTracks; ++g) {
            result.grouping[g].missing_reason = MissingReason::NumericFault;
            result.identity[g].missing_reason = MissingReason::NumericFault;
            result.association[g].missing_reason = MissingReason::NumericFault;
        }
        s.invalidate_audio_state(); result.state_reset = true; output.fill(0);
        result.compute_us = elapsed(); return;
    }
    result.feedback_committed = s.commit_previous_feedback(); s.group_sources(result); s.associate(result);
    result.groups = s.candidate_count;
    s.observe_aliases(result);
    for (const auto& track : s.tracks) result.tracks += track.active;
    const float ownership_gap = s.compete(result);
    for (unsigned g = 0; g < s.candidate_count; ++g) {
        if (!s.ablation.grouping) {
            result.grouping[g].valid = false; result.grouping[g].missing_reason = MissingReason::DisabledForAblation;
        }
        const auto& c = s.candidates[g]; const auto& identity = result.identity[g];
        if (c.slot >= 0 && (!identity.valid || identity.relative_score < s.config.identity_margin - .2f)) {
            auto& track = s.tracks[std::size_t(c.slot)];
            if (track.trusted) ++result.feedback_rejected;
            track.trusted = false; track.coefficient = 0; track.velocity = 0; track.trusted_spectrum.fill(0);
        }
    }
    unsigned steps = s.config.solver_iterations;
    if (s.ablation.scheduling && s.candidate_count < 2) steps = std::min(steps, 2u);
    if (s.ablation.scheduling && s.frontend.rms < s.config.minimum_rms) steps = 1;
    if (elapsed() > 8000) { steps = 1; result.budget_exhausted = true; }
    if (s.ablation.contribution) s.solve(result, steps); else s.direct_contribution(result);
    for (unsigned g = 0; g < s.candidate_count; ++g) if (result.ownership[g] == Ownership::Target) {
        const auto& id = result.identity[g];
        const float confidence = (.65f + .35f * id.reliability) * logistic(3 * (id.relative_score - s.config.identity_margin));
        result.target_confidence = std::max(result.target_confidence, confidence);
    }
    result.target_present = result.target_confidence > .55f;
    const float rho = result.target_present ? unit((result.target_confidence - .40f) / .40f) : 0;
    for (std::size_t k = 0; k < Bins; ++k) {
        const float total = result.target_power[k] + result.interference_power[k] + result.noise_power[k] + result.unknown_power[k];
        result.gate[k] = bounded(rho * result.target_power[k] / (total + Eps), 0, s.config.maximum_gain);
        result.solver_finite &= std::isfinite(result.gate[k]);
    }
    if (!result.solver_finite || !std::isfinite(result.residual_ratio) || result.residual_ratio > 2.5f) {
        s.invalidate_audio_state(); result.state_reset = true; output.fill(0); result.compute_us = elapsed(); return;
    }
    if (elapsed() >= 10000) {
        // No stale audio is replayed on an overrun; the transport can continue
        // its independent zero-value clock. This also clears the old OLA tail.
        s.invalidate_audio_state(); result.state_reset = true; output.fill(0); result.budget_exhausted = true;
        result.compute_us = elapsed(); return;
    }
    s.frontend.reconstruct(result.gate, output); s.feedback(result, ownership_gap);
    if (s.ablation.contribution) s.cache_solver_result(); else { s.cached_count = 0; s.cached_frame = 0; }
    double energy = 0; for (float v : output) energy += double(v) * v;
    result.output_rms = float(std::sqrt(energy / Hop)); result.valid = true;
    // Noise statistics are conservatively updated only in quiet observations;
    // uncertain speech never becomes a learned noise or identity prototype.
    if (s.frontend.rms < s.config.minimum_rms && !result.target_present)
        for (std::size_t k = 0; k < Bins; ++k) s.noise[k] = .98f * s.noise[k] + .02f * s.frontend.power[k];
    s.history[s.history_write] = s.frontend.power; s.history_write = (s.history_write + 1) % History;
    s.history_count = std::min(s.history_count + 1, s.config.history_hops);
    s.prior_total = s.frontend.total; result.compute_us = elapsed();
}
}
