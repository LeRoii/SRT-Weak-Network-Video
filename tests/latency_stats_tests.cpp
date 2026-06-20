#include "common/latency_stats.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 0.001;
}

} // namespace

int main() {
    LatencyStats stats(10);
    const uint64_t generation = stats.reset();
    for (uint64_t milliseconds = 1; milliseconds <= 20; ++milliseconds) {
        stats.record(generation, 1'000'000,
                     1'000'000 + milliseconds * 1000,
                     static_cast<int64_t>(milliseconds * 1000));
    }

    auto snapshot = stats.snapshot(20'000);
    assert(snapshot.average_ms);
    assert(near(*snapshot.average_ms, 10.5));
    assert(snapshot.invalid_samples == 0);

    stats.record(generation, 2'000'000, 1'999'999, 21'000);
    stats.record(generation, 1, 60'000'002, 22'000);
    snapshot = stats.snapshot(22'000);
    assert(snapshot.invalid_samples == 2);

    const uint64_t next_generation = stats.reset();
    stats.record(generation, 1'000'000, 1'002'000, 23'000);
    snapshot = stats.snapshot(23'000);
    assert(!snapshot.average_ms);
    assert(snapshot.invalid_samples == 2);

    stats.record(next_generation, 2'000'000, 2'005'000, 24'000);
    snapshot = stats.snapshot(10'024'001);
    assert(!snapshot.average_ms);

    std::cout << "latency_stats_tests=passed" << std::endl;
    return 0;
}
