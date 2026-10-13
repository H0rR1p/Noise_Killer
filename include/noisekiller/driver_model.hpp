#pragma once
#include "noisekiller/driver_contract.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nk::driver_test {
// User-mode reference model for contract/fault tests only. This is not a
// kernel driver, audio endpoint, auth broker, or supported runtime sink.
enum class Result { Ok, Unauthorized, InvalidMessage, OldEpoch, WrongSequence,
                    Discontinuous, Late, Full, NotConfigured, BadClock, Busy };
struct Peer {
    std::uint64_t process = 0, creation_time = 0, handle = 0;
    std::uint32_t session = 0;
};
struct Consumer {
    std::uint64_t id = 0, cursor = 0;
};
class TransportModel {
public:
    explicit TransportModel(std::uint64_t qpc_frequency) noexcept;
    // Simulates a grant ALREADY authorized out of band by the trusted broker.
    // It does not authenticate any executable or SID; tests inject that result.
    Result grant_from_test_authority(Peer peer, std::uint64_t now_qpc) noexcept;
    Result configure(Peer peer, const NK_CONFIGURE& message, std::uint64_t now_qpc) noexcept;
    Result submit(Peer peer, const NK_SUBMIT& message, std::size_t bytes,
                  std::uint64_t now_qpc) noexcept;
    Result stop(Peer peer) noexcept;
    void disconnect(Peer peer) noexcept;
    Result advance_clock(std::uint64_t now_qpc) noexcept;
    Consumer open_consumer() noexcept;
    void close_consumer(Consumer& consumer) noexcept;
    std::size_t read(Consumer& consumer, std::int16_t* output, std::size_t count) noexcept;
    NK_STATS stats() const noexcept;
private:
    static constexpr std::size_t MaxConsumers = 8;
    static constexpr std::size_t History = NK_DRIVER_HOP * NK_DRIVER_QUEUE_HOPS;
    static constexpr std::size_t Ring = History * 2;
    struct Sample { std::uint64_t position = UINT64_MAX; std::int16_t pcm = 0; };
    struct Reader { std::uint64_t id = 0, cursor = 0; };
    std::array<Sample, Ring> ring_{};
    std::array<Reader, MaxConsumers> readers_{};
    Peer owner_{};
    std::uint64_t frequency_ = 0, clock_qpc_ = 0, position_ = 0, remainder_ = 0;
    std::uint64_t epoch_ = 0, highest_epoch_ = 0, sequence_ = 0;
    std::uint64_t producer_sample_ = 0, origin_ = 0, last_progress_qpc_ = 0, last_capture_qpc_ = 0;
    std::uint64_t accepted_ = 0, rejected_ = 0, zeros_ = 0, watchdogs_ = 0;
    std::uint64_t overruns_ = 0, next_consumer_ = 1;
    bool clock_initialized_ = false, configured_ = false;
    bool is_owner(Peer peer) const noexcept;
    void invalidate(bool remove_owner) noexcept;
    Result reject(Result result) noexcept;
};
}
