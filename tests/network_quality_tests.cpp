#include "common/network_quality.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

void expect_near(double actual, double expected) {
    assert(std::abs(actual - expected) < 0.001);
}

void test_minimum_sample_and_loss_ratios() {
    NetworkLossEstimator estimator(400);
    assert(!estimator.update(1000, 100));
    assert(!estimator.update(1399, 219));

    const auto thirty_percent = estimator.update(1400, 220);
    assert(thirty_percent);
    expect_near(*thirty_percent, 30.0);

    const auto eighty_one_percent = estimator.update(1800, 544);
    assert(eighty_one_percent);
    expect_near(*eighty_one_percent, 81.0);
}

void test_caps_at_one_hundred_percent() {
    NetworkLossEstimator estimator(100);
    assert(!estimator.update(0, 0));
    const auto loss = estimator.update(100, 150);
    assert(loss);
    expect_near(*loss, 100.0);
}

void test_counter_reset_starts_a_new_baseline() {
    NetworkLossEstimator estimator(100);
    assert(!estimator.update(1000, 500));
    assert(estimator.update(1100, 550));
    assert(!estimator.update(10, 5));

    const auto loss = estimator.update(110, 35);
    assert(loss);
    expect_near(*loss, 30.0);
}

void test_explicit_reset_discards_stalled_interval() {
    NetworkLossEstimator estimator(100);
    assert(!estimator.update(1000, 500));
    estimator.reset(1100, 900);
    assert(!estimator.update(1199, 949));

    const auto loss = estimator.update(1200, 950);
    assert(loss);
    expect_near(*loss, 50.0);
}

} // namespace

int main() {
    test_minimum_sample_and_loss_ratios();
    test_caps_at_one_hundred_percent();
    test_counter_reset_starts_a_new_baseline();
    test_explicit_reset_discards_stalled_interval();
    std::cout << "network_quality_tests=passed" << std::endl;
    return 0;
}
