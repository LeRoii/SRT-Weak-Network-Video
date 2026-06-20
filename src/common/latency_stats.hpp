#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

struct LatencySnapshot {
    std::optional<double> average_ms;
    uint64_t invalid_samples = 0;
};

class LatencyStats {
public:
    explicit LatencyStats(int window_seconds);

    uint64_t reset();
    void record(uint64_t generation,
                uint64_t start_unix_us,
                uint64_t end_unix_us,
                int64_t observed_monotonic_us);
    LatencySnapshot snapshot(int64_t now_monotonic_us);

private:
    struct Sample {
        int64_t observed_monotonic_us = 0;
        uint64_t latency_us = 0;
    };

    void expire_locked(int64_t now_monotonic_us);

    int64_t window_us_ = 0;
    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    uint64_t generation_ = 0;
    uint64_t invalid_samples_ = 0;
};
