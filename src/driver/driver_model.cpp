#include "noisekiller/driver_model.hpp"
#include <algorithm>
#include <limits>

namespace nk::driver_test {
namespace {
bool header_valid(const NK_HEADER& h, std::size_t size, NK_OPERATION operation) noexcept {
    return h.size == size && h.version == NK_DRIVER_VERSION && h.operation == operation;
}
}
TransportModel::TransportModel(std::uint64_t frequency) noexcept : frequency_(frequency) {}
bool TransportModel::is_owner(Peer peer) const noexcept {
    return owner_.process != 0 && peer.process == owner_.process &&
        peer.creation_time == owner_.creation_time && peer.handle == owner_.handle &&
        peer.session == owner_.session;
}
void TransportModel::invalidate(bool remove_owner) noexcept {
    configured_ = false;
    epoch_ = 0;
    sequence_ = producer_sample_ = last_capture_qpc_ = 0;
    for (auto& sample : ring_) sample = {};
    if (remove_owner) owner_ = {};
}
Result TransportModel::reject(Result result) noexcept { ++rejected_; return result; }
Result TransportModel::grant_from_test_authority(Peer peer, std::uint64_t now) noexcept {
    if (!peer.process || !peer.creation_time || !peer.handle) return Result::Unauthorized;
    if (owner_.process) return Result::Busy;
    const auto clock = advance_clock(now);
    if (clock != Result::Ok) return clock;
    owner_ = peer;
    highest_epoch_ = 0; // epoch monotonicity is scoped to this authorized owner handle
    return Result::Ok;
}
Result TransportModel::configure(Peer peer, const NK_CONFIGURE& m, std::uint64_t now) noexcept {
    if (!is_owner(peer)) return reject(Result::Unauthorized);
    if (!header_valid(m.header, sizeof(m), NK_OP_CONFIGURE) || m.reserved ||
        m.sample_rate != NK_DRIVER_RATE || m.channels != NK_DRIVER_CHANNELS ||
        m.bits != NK_DRIVER_BITS || m.hop_samples != NK_DRIVER_HOP ||
        m.max_queue_hops != NK_DRIVER_QUEUE_HOPS) return reject(Result::InvalidMessage);
    if (m.header.epoch == 0 || m.header.epoch <= highest_epoch_) return reject(Result::OldEpoch);
    if (configured_) return reject(Result::Busy);
    const auto clock = advance_clock(now);
    if (clock != Result::Ok) return reject(clock);
    if (position_ > UINT64_MAX - NK_DRIVER_HOP) return reject(Result::BadClock);
    invalidate(false);
    epoch_ = highest_epoch_ = m.header.epoch;
    origin_ = position_ + NK_DRIVER_HOP;
    last_progress_qpc_ = now;
    configured_ = true;
    return Result::Ok;
}
Result TransportModel::submit(Peer peer, const NK_SUBMIT& m, std::size_t bytes,
                              std::uint64_t now) noexcept {
    if (!is_owner(peer)) return reject(Result::Unauthorized);
    if (bytes != sizeof(m) || !header_valid(m.header, sizeof(m), NK_OP_SUBMIT) ||
        m.reserved || m.sample_count != NK_DRIVER_HOP) return reject(Result::InvalidMessage);
    const auto clock = advance_clock(now);
    if (clock != Result::Ok) return reject(clock);
    if (!configured_) return reject(Result::NotConfigured);
    if (m.header.epoch != epoch_) return reject(Result::OldEpoch);
    if (m.sequence != sequence_) return reject(Result::WrongSequence);
    if (m.first_sample_index != producer_sample_) return reject(Result::Discontinuous);
    if (origin_ > UINT64_MAX - NK_DRIVER_HOP ||
        m.first_sample_index > UINT64_MAX - origin_ - NK_DRIVER_HOP || sequence_ == UINT64_MAX)
        return reject(Result::InvalidMessage);
    if (m.capture_qpc > now || m.capture_qpc < last_capture_qpc_) return reject(Result::Discontinuous);
    const auto first = origin_ + m.first_sample_index;
    const auto end = first + NK_DRIVER_HOP;
    if (first < position_) return reject(Result::Late);
    if (end - position_ > History) return reject(Result::Full);
    for (std::size_t i = 0; i < NK_DRIVER_HOP; ++i) {
        auto& slot = ring_[(first + i) % Ring];
        slot.position = first + i;
        slot.pcm = m.pcm[i];
    }
    ++sequence_;
    producer_sample_ += NK_DRIVER_HOP;
    last_progress_qpc_ = now;
    last_capture_qpc_ = m.capture_qpc;
    ++accepted_;
    return Result::Ok;
}
Result TransportModel::stop(Peer peer) noexcept {
    if (!is_owner(peer)) return reject(Result::Unauthorized);
    invalidate(false);
    return Result::Ok;
}
void TransportModel::disconnect(Peer peer) noexcept { if (is_owner(peer)) invalidate(true); }
Result TransportModel::advance_clock(std::uint64_t now) noexcept {
    if (frequency_ < 1000 || frequency_ > UINT64_MAX / (NK_DRIVER_RATE + 1u)) return Result::BadClock;
    if (!clock_initialized_) { clock_qpc_ = now; clock_initialized_ = true; return Result::Ok; }
    if (now < clock_qpc_) return Result::BadClock;
    const auto elapsed = now - clock_qpc_;
    const auto seconds = elapsed / frequency_;
    const auto fraction = elapsed % frequency_;
    if (seconds > UINT64_MAX / NK_DRIVER_RATE) return Result::BadClock;
    const auto fractional = fraction * NK_DRIVER_RATE + remainder_;
    const auto whole_samples = seconds * NK_DRIVER_RATE;
    if (fractional / frequency_ > UINT64_MAX - whole_samples) return Result::BadClock;
    const auto delta = whole_samples + fractional / frequency_;
    if (delta > UINT64_MAX - position_) return Result::BadClock;
    position_ += delta;
    remainder_ = fractional % frequency_;
    clock_qpc_ = now;
    const auto watchdog_ticks = (frequency_ / 1000u) * NK_DRIVER_WATCHDOG_MS +
        ((frequency_ % 1000u) * NK_DRIVER_WATCHDOG_MS) / 1000u;
    if (configured_ && now - last_progress_qpc_ >= watchdog_ticks) {
        invalidate(true);
        ++watchdogs_;
    }
    return Result::Ok;
}
Consumer TransportModel::open_consumer() noexcept {
    if (!next_consumer_) return {};
    for (auto& reader : readers_) if (!reader.id) {
        reader = {next_consumer_++, position_};
        return {reader.id, reader.cursor};
    }
    return {};
}
void TransportModel::close_consumer(Consumer& consumer) noexcept {
    for (auto& reader : readers_) if (reader.id && reader.id == consumer.id) {
        reader = {}; break;
    }
    consumer = {};
}
std::size_t TransportModel::read(Consumer& consumer, std::int16_t* output, std::size_t count) noexcept {
    if (!output || count > NK_DRIVER_HOP) return 0;
    std::fill_n(output, count, std::int16_t{0});
    for (auto& reader : readers_) if (reader.id && reader.id == consumer.id) {
        if (position_ > History && reader.cursor < position_ - History) {
            reader.cursor = position_ - History;
            ++overruns_;
        }
        const auto available = position_ - reader.cursor;
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count, available));
        for (std::size_t i = 0; i < take; ++i) {
            const auto at = reader.cursor + i;
            const auto& sample = ring_[at % Ring];
            if (sample.position == at) output[i] = sample.pcm;
            else ++zeros_;
        }
        reader.cursor += take;
        consumer.cursor = reader.cursor;
        return take;
    }
    return 0;
}
NK_STATS TransportModel::stats() const noexcept {
    NK_STATS result{};
    result.header = {sizeof(result), NK_DRIVER_VERSION, NK_OP_STATS, epoch_};
    result.endpoint_sample_position = position_;
    result.endpoint_qpc = clock_qpc_;
    result.accepted_blocks = accepted_;
    result.rejected_blocks = rejected_;
    result.zero_filled_samples = zeros_;
    result.watchdog_stops = watchdogs_;
    result.consumer_overruns = overruns_;
    result.owner_active = owner_.process != 0;
    for (const auto& reader : readers_) if (reader.id) ++result.active_consumers;
    for (const auto& sample : ring_) if (sample.position != UINT64_MAX && sample.position >= position_)
        ++result.queued_samples;
    return result;
}
}
