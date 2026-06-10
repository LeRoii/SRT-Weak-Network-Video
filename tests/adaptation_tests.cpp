#include "common/adaptation.hpp"

#include <cassert>
#include <iostream>

namespace {

NetworkSnapshot network(double loss, double rtt = 50.0) {
    NetworkSnapshot snapshot;
    snapshot.loss_percent = loss;
    snapshot.rtt_ms = rtt;
    snapshot.valid = true;
    return snapshot;
}

void expect_profile(const VideoProfile &profile,
                    int level,
                    int bitrate,
                    int fps,
                    int width,
                    int height,
                    double parity_ratio,
                    bool all_intra) {
    assert(profile.level == level);
    assert(profile.bitrate_kbps == bitrate);
    assert(profile.fps == fps);
    assert(profile.width == width);
    assert(profile.height == height);
    assert(profile.parity_ratio == parity_ratio);
    assert(profile.all_intra == all_intra);
}

void test_profile_ladder_and_loss_boundaries() {
    struct Expected {
        double loss;
        int level;
        int bitrate;
        int fps;
        int width;
        int height;
        double parity_ratio;
        bool all_intra;
    };
    const Expected cases[] = {
        {0.0, 0, 2000, 30, 1280, 720, 0.15, false},
        {3.0, 0, 2000, 30, 1280, 720, 0.15, false},
        {3.01, 1, 1400, 20, 1280, 720, 0.25, false},
        {7.0, 1, 1400, 20, 1280, 720, 0.25, false},
        {7.01, 2, 900, 10, 960, 540, 0.50, false},
        {15.0, 2, 900, 10, 960, 540, 0.50, false},
        {15.01, 3, 500, 5, 640, 360, 0.75, false},
        {30.0, 3, 500, 5, 640, 360, 0.75, false},
        {30.01, 4, 250, 3, 426, 240, 1.00, false},
        {50.0, 4, 250, 3, 426, 240, 1.00, false},
        {50.01, 5, 120, 2, 426, 240, 2.00, true},
        {70.0, 5, 120, 2, 426, 240, 2.00, true},
        {70.01, 6, 50, 1, 320, 180, 3.00, true},
        {80.0, 6, 50, 1, 320, 180, 3.00, true},
        {80.01, 7, 20, 1, 160, 90, 5.00, true},
        {90.0, 7, 20, 1, 160, 90, 5.00, true},
        {90.01, 8, 8, 1, 128, 72, 11.00, true},
    };

    for (const auto &expected : cases) {
        AdaptationController controller(2000);
        expect_profile(controller.update(network(expected.loss)),
                       expected.level,
                       expected.bitrate,
                       expected.fps,
                       expected.width,
                       expected.height,
                       expected.parity_ratio,
                       expected.all_intra);
    }
}

void test_rtt_floor_and_fast_degradation() {
    struct Expected {
        double rtt;
        int level;
    };
    const Expected cases[] = {
        {150.0, 0},
        {150.01, 3},
        {220.01, 5},
        {300.01, 6},
        {400.01, 7},
    };

    for (const auto &expected : cases) {
        AdaptationController controller(2000);
        assert(controller.update(network(0.0, expected.rtt)).level ==
               expected.level);
    }

    AdaptationController emergency(2000);
    expect_profile(emergency.update(network(100.0, 500.0)),
                   8, 8, 1, 128, 72, 11.00, true);
}

void test_recovery_requires_five_healthy_windows() {
    AdaptationController controller(2000);
    assert(controller.update(network(75.0)).level == 6);

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(0.0, 50.0)).level == 6);
    }
    assert(controller.update(network(0.0, 50.0)).level == 5);

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(0.0, 50.0)).level == 5);
    }
    assert(controller.update(network(0.0, 50.0)).level == 4);

    assert(controller.update(network(4.0, 50.0)).level == 4);
    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(0.0, 50.0)).level == 4);
    }
    assert(controller.update(network(0.0, 50.0)).level == 3);
}

void test_max_video_bitrate_selects_supported_profile() {
    AdaptationController capped(1000);
    expect_profile(capped.current(), 2, 900, 10, 960, 540, 0.50, false);

    AdaptationController minimum(1);
    expect_profile(minimum.current(), 8, 8, 1, 128, 72, 11.00, true);
}

void test_invalid_snapshot_keeps_current_profile() {
    AdaptationController controller(2000);
    assert(controller.update(network(55.0)).level == 5);

    NetworkSnapshot invalid;
    assert(controller.update(invalid).level == 5);
}

} // namespace

int main() {
    test_profile_ladder_and_loss_boundaries();
    test_rtt_floor_and_fast_degradation();
    test_recovery_requires_five_healthy_windows();
    test_max_video_bitrate_selects_supported_profile();
    test_invalid_snapshot_keeps_current_profile();
    std::cout << "adaptation_tests=passed" << std::endl;
    return 0;
}
