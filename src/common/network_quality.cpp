#include "common/network_quality.hpp"

#include <algorithm>

NetworkLossEstimator::NetworkLossEstimator(
    uint64_t minimum_resolved_packets)
    : minimum_resolved_packets_(minimum_resolved_packets) {}

std::optional<double> NetworkLossEstimator::update(
    uint64_t resolved_packets_total,
    uint64_t lost_packets_total) {
    if (!resolved_baseline_ || !lost_baseline_ ||
        resolved_packets_total < *resolved_baseline_ ||
        lost_packets_total < *lost_baseline_) {
        resolved_baseline_ = resolved_packets_total;
        lost_baseline_ = lost_packets_total;
        return std::nullopt;
    }

    const uint64_t resolved =
        resolved_packets_total - *resolved_baseline_;
    if (resolved < minimum_resolved_packets_) {
        return std::nullopt;
    }

    const uint64_t lost = lost_packets_total - *lost_baseline_;
    resolved_baseline_ = resolved_packets_total;
    lost_baseline_ = lost_packets_total;
    return std::min(
        static_cast<double>(lost) * 100.0 /
            static_cast<double>(resolved),
        100.0);
}
