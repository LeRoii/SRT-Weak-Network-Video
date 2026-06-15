#include "common/latency_stats.hpp"

namespace {
constexpr uint64_t kMaximumLatencyUs = 60'000'000;
}

LatencyStats::LatencyStats(int window_seconds)
    : window_us_(static_cast<int64_t>(window_seconds) * 1'000'000) {}

uint64_t LatencyStats::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    return generation_;
}

void LatencyStats::record(uint64_t generation,
                          uint64_t start_unix_us,
                          uint64_t end_unix_us,
                          int64_t observed_monotonic_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) {
        return;
    }
    if (start_unix_us == 0 || end_unix_us < start_unix_us ||
        end_unix_us - start_unix_us > kMaximumLatencyUs) {
        ++invalid_samples_;
        return;
    }
    expire_locked(observed_monotonic_us);
    samples_.push_back(
        {observed_monotonic_us, end_unix_us - start_unix_us});
}

LatencySnapshot LatencyStats::snapshot(int64_t now_monotonic_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire_locked(now_monotonic_us);

    LatencySnapshot result;
    result.invalid_samples = invalid_samples_;
    if (samples_.empty()) {
        return result;
    }

    uint64_t total = 0;
    for (const auto &sample : samples_) {
        total += sample.latency_us;
    }
    result.average_ms =
        static_cast<double>(total) / samples_.size() / 1000.0;
    return result;
}

void LatencyStats::expire_locked(int64_t now_monotonic_us) {
    while (!samples_.empty() &&
           now_monotonic_us - samples_.front().observed_monotonic_us >
               window_us_) {
        samples_.pop_front();
    }
}
