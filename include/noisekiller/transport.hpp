#pragma once
#include "noisekiller/driver_contract.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nk::win {
using TransportStats = NK_STATS;
// Real Win32 client only. No synthetic endpoint or TransportModel fallback.
// Requires separately installed and admitted TrustedBroker + signed driver.
class DriverTransport {
public:
    DriverTransport();
    ~DriverTransport();
    DriverTransport(const DriverTransport&) = delete;
    DriverTransport& operator=(const DriverTransport&) = delete;
    bool open(std::string& error);
    bool start(std::uint64_t epoch, std::string& error);
    bool write(const float* pcm, std::size_t count, std::uint64_t first_sample,
               std::uint64_t capture_qpc, std::string& error);
    bool stats(TransportStats& result, std::string& error);
    void stop() noexcept;
    void close() noexcept;
    bool available() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
