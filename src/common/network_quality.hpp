#pragma once

#include <cstdint>
#include <optional>

class NetworkLossEstimator {
public:
    explicit NetworkLossEstimator(uint64_t minimum_resolved_packets = 400);

    void reset(uint64_t resolved_packets_total,
               uint64_t lost_packets_total);
    std::optional<double> update(uint64_t resolved_packets_total,
                                 uint64_t lost_packets_total);

private:
    uint64_t minimum_resolved_packets_;
    std::optional<uint64_t> resolved_baseline_;
    std::optional<uint64_t> lost_baseline_;
};
