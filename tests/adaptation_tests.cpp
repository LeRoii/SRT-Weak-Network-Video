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
                    double keyframe_parity_ratio,
                    bool all_intra) {
    assert(profile.level == level);
    assert(profile.bitrate_kbps == bitrate);
    assert(profile.fps == fps);
    assert(profile.width == width);
    assert(profile.height == height);
    assert(profile.parity_ratio == parity_ratio);
    assert(profile.keyframe_parity_ratio ==
           keyframe_parity_ratio);
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
        double keyframe_parity_ratio;
        bool all_intra;
    };
    const Expected cases[] = {
        {0.0, 0, 2000, 30, 1280, 720, 0.10, 0.30, false},
        {3.0, 0, 2000, 30, 1280, 720, 0.10, 0.30, false},
        {3.01, 1, 1200, 20, 960, 540, 0.15, 0.50, false},
        {7.0, 1, 1200, 20, 960, 540, 0.15, 0.50, false},
        {7.01, 2, 700, 15, 640, 360, 0.25, 0.75, false},
        {15.0, 2, 700, 15, 640, 360, 0.25, 0.75, false},
        {15.01, 3, 400, 10, 640, 360, 0.40, 1.00, false},
        {30.0, 3, 400, 10, 640, 360, 0.40, 1.00, false},
        {30.01, 4, 220, 5, 426, 240, 0.75, 2.00, false},
        {50.0, 4, 220, 5, 426, 240, 0.75, 2.00, false},
        {50.01, 5, 140, 3, 426, 240, 1.25, 3.00, false},
        {65.0, 5, 140, 3, 426, 240, 1.25, 3.00, false},
        {65.01, 6, 80, 3, 320, 180, 2.50, 5.00, false},
        {72.0, 6, 80, 3, 320, 180, 2.50, 5.00, false},
        {72.01, 7, 50, 2, 320, 180, 4.00, 7.00, false},
        {77.0, 7, 50, 2, 320, 180, 4.00, 7.00, false},
        {77.01, 8, 30, 2, 256, 144, 8.00, 12.00, false},
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
                       expected.keyframe_parity_ratio,
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
                   8, 30, 2, 256, 144, 8.00, 12.00, false);
}

void test_recovery_requires_five_healthy_windows() {
    AdaptationController controller(2000);
    assert(controller.update(network(70.0)).level == 6);

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

void test_emergency_recovers_to_current_network_level() {
    AdaptationController controller(2000);
    assert(controller.update(network(100.0, 500.0)).level == 8);

    for (int target_level = 7; target_level >= 4; --target_level) {
        for (int sample = 0; sample < 4; ++sample) {
            assert(controller.update(network(50.0, 50.0)).level ==
                   target_level + 1);
        }
        assert(controller.update(network(50.0, 50.0)).level ==
               target_level);
    }

    for (int sample = 0; sample < 10; ++sample) {
        assert(controller.update(network(50.0, 50.0)).level == 4);
    }
}

void test_max_video_bitrate_selects_supported_profile() {
    AdaptationController capped(1000);
    expect_profile(
        capped.current(), 2, 700, 15, 640, 360,
        0.25, 0.75, false);

    AdaptationController minimum(1);
    expect_profile(
        minimum.current(), 8, 30, 2, 256, 144,
        8.00, 12.00, false);
}

void test_invalid_snapshot_keeps_current_profile() {
    AdaptationController controller(2000);
    assert(controller.update(network(55.0)).level == 5);

    NetworkSnapshot invalid;
    assert(controller.update(invalid).level == 5);
}

void test_udp_recovery_profile() {
    expect_profile(
        udp_recovery_profile(2000),
        8, 30, 2, 256, 144, 8.0, 12.0, false);

    AdaptationController controller(2000);
    controller.force_emergency();
    expect_profile(
        controller.current(),
        8, 30, 2, 256, 144, 8.0, 12.0, false);
}

void test_reset_to_network_after_udp_recovery() {
    AdaptationController controller(2000);
    controller.force_emergency();
    expect_profile(
        controller.reset_to_network(network(0.0, 50.0)),
        0, 2000, 30, 1280, 720, 0.10, 0.30, false);

    controller.force_emergency();
    expect_profile(
        controller.reset_to_network(network(70.0, 50.0)),
        6, 80, 3, 320, 180, 2.50, 5.00, false);
}

} // namespace

int main() {
    test_profile_ladder_and_loss_boundaries();
    test_rtt_floor_and_fast_degradation();
    test_recovery_requires_five_healthy_windows();
    test_emergency_recovers_to_current_network_level();
    test_max_video_bitrate_selects_supported_profile();
    test_invalid_snapshot_keeps_current_profile();
    test_udp_recovery_profile();
    test_reset_to_network_after_udp_recovery();
    std::cout << "adaptation_tests=passed" << std::endl;
    return 0;
}
