#include "common/adaptation.hpp"

#include <cassert>
#include <iostream>
#include <optional>

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
                    int gop_frames,
                    int min_data_shards,
                    double parity_ratio,
                    double keyframe_parity_ratio,
                    bool all_intra) {
    assert(profile.level == level);
    assert(profile.bitrate_kbps == bitrate);
    assert(profile.fps == fps);
    assert(profile.width == width);
    assert(profile.height == height);
    assert(profile.gop_frames == gop_frames);
    assert(profile.min_data_shards == min_data_shards);
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
        int gop_frames;
        int min_data_shards;
        double parity_ratio;
        double keyframe_parity_ratio;
        bool all_intra;
    };
    const Expected cases[] = {
        {0.0, 0, 2000, 30, 1280, 720, 30, 4, 0.10, 0.30, false},
        {5.0, 0, 2000, 30, 1280, 720, 30, 4, 0.10, 0.30, false},
        {5.01, 1, 900, 24, 640, 360, 12, 4, 0.50, 0.75, false},
        {15.0, 1, 900, 24, 640, 360, 12, 4, 0.50, 0.75, false},
        {15.01, 1, 900, 24, 640, 360, 12, 4, 0.50, 0.75, false},
        {20.0, 1, 900, 24, 640, 360, 12, 4, 0.50, 0.75, false},
        {20.01, 3, 500, 20, 640, 360, 10, 4, 1.00, 2.00, false},
        {30.0, 3, 500, 20, 640, 360, 10, 4, 1.00, 2.00, false},
        {30.01, 4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
        {50.0, 4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
        {50.01, 4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
        {55.0, 4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
        {60.0, 4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
        {60.01, 5, 220, 15, 320, 180, 3, 2, 3.00, 6.00, false},
        {65.0, 5, 220, 15, 320, 180, 3, 2, 3.00, 6.00, false},
        {65.01, 6, 150, 12, 320, 180, 2, 2, 5.00, 8.00, false},
        {72.0, 6, 150, 12, 320, 180, 2, 2, 5.00, 8.00, false},
        {72.01, 7, 90, 12, 256, 144, 1, 1, 8.00, 10.00, true},
        {77.0, 7, 90, 12, 256, 144, 1, 1, 8.00, 10.00, true},
        {77.01, 8, 60, 10, 160, 90, 1, 1, 12.00, 12.00, true},
    };

    for (const auto &expected : cases) {
        AdaptationController controller(2000);
        expect_profile(controller.update(network(expected.loss)),
                       expected.level,
                       expected.bitrate,
                       expected.fps,
                       expected.width,
                       expected.height,
                       expected.gop_frames,
                       expected.min_data_shards,
                       expected.parity_ratio,
                       expected.keyframe_parity_ratio,
                       expected.all_intra);
    }
}

void test_rtt_requires_sustained_confirmation() {
    struct Expected {
        double rtt;
        int level;
    };
    const Expected cases[] = {
        {150.01, 0},
        {160.0, 0},
        {160.01, 3},
        {220.01, 5},
        {300.01, 6},
        {400.01, 7},
    };

    for (const auto &expected : cases) {
        AdaptationController controller(2000);
        for (int sample = 0; sample < 4; ++sample) {
            assert(controller.update(network(0.0, expected.rtt)).level == 0);
        }
        assert(controller.update(network(0.0, expected.rtt)).level ==
               expected.level);
    }
}

void test_rtt_spike_does_not_degrade_profile() {
    AdaptationController controller(2000);

    assert(controller.update(network(0.0, 170.0)).level == 0);
    assert(controller.update(network(0.0, 50.0)).level == 0);
    assert(controller.update(network(0.0, 230.0)).level == 0);
    assert(controller.update(network(0.0, 50.0)).level == 0);
}

void test_rtt_diagnostics_track_confirmation() {
    AdaptationController controller(2000);

    assert(controller.update(network(0.0, 230.0)).level == 0);
    auto diagnostics = controller.diagnostics();
    assert(diagnostics.loss_required_level == 0);
    assert(diagnostics.raw_rtt_required_level == 5);
    assert(diagnostics.confirmed_rtt_required_level == 0);
    assert(diagnostics.rtt_high_windows == 1);

    assert(controller.update(network(0.0, 230.0)).level == 0);
    diagnostics = controller.diagnostics();
    assert(diagnostics.rtt_high_windows == 2);
    assert(diagnostics.confirmed_rtt_required_level == 0);

    assert(controller.update(network(0.0, 230.0)).level == 0);
    diagnostics = controller.diagnostics();
    assert(diagnostics.rtt_high_windows == 3);
    assert(diagnostics.confirmed_rtt_required_level == 0);

    assert(controller.update(network(0.0, 230.0)).level == 0);
    diagnostics = controller.diagnostics();
    assert(diagnostics.rtt_high_windows == 4);
    assert(diagnostics.confirmed_rtt_required_level == 0);

    assert(controller.update(network(0.0, 230.0)).level == 5);
    diagnostics = controller.diagnostics();
    assert(diagnostics.rtt_high_windows == 5);
    assert(diagnostics.confirmed_rtt_required_level == 5);
}

void test_low_loss_l2_requires_confirmation() {
    AdaptationController controller(2000);

    assert(controller.update(network(15.01, 50.0)).level == 1);
    assert(controller.update(network(15.01, 50.0)).level == 1);
    expect_profile(controller.update(network(15.01, 50.0)),
                   2, 700, 24, 640, 360, 12, 4,
                   0.50, 0.75, false);

    for (int sample = 0; sample < 10; ++sample) {
        assert(controller.update(network(14.5, 50.0)).level == 2);
    }

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(11.9, 50.0)).level == 2);
    }
    assert(controller.update(network(11.9, 50.0)).level == 1);
}

void test_loss_degradation_is_immediate_for_high_loss() {
    AdaptationController emergency(2000);
    expect_profile(emergency.update(network(80.0, 50.0)),
                   8, 60, 10, 160, 90, 1, 1,
                   12.00, 12.00, true);

    AdaptationController high_loss(2000);
    expect_profile(high_loss.update(network(70.0, 500.0)),
                   6, 150, 12, 320, 180, 2, 2,
                   5.00, 8.00, false);
    for (int sample = 0; sample < 3; ++sample) {
        assert(high_loss.update(network(70.0, 500.0)).level == 6);
    }
    assert(high_loss.update(network(70.0, 500.0)).level == 7);
}

void test_fifty_percent_loss_boundary_stays_at_level_four() {
    AdaptationController controller(2000);
    assert(controller.update(network(45.0, 50.0)).level == 4);

    const double jitter_losses[] = {
        49.0, 52.0, 48.0, 51.0, 50.5, 49.5, 54.5, 50.1,
    };
    for (const double loss : jitter_losses) {
        assert(controller.update(network(loss, 50.0)).level == 4);
    }
}

void test_level_five_requires_sustained_or_clear_loss() {
    AdaptationController sustained(2000);
    assert(sustained.update(network(45.0, 50.0)).level == 4);
    assert(sustained.update(network(56.0, 50.0)).level == 4);
    assert(sustained.update(network(56.0, 50.0)).level == 4);
    assert(sustained.update(network(56.0, 50.0)).level == 5);

    AdaptationController clear_loss(2000);
    assert(clear_loss.update(network(45.0, 50.0)).level == 4);
    assert(clear_loss.update(network(61.0, 50.0)).level == 5);
}

void test_level_five_recovery_requires_below_fifty_percent() {
    AdaptationController controller(2000);
    assert(controller.update(network(61.0, 50.0)).level == 5);

    for (int sample = 0; sample < 10; ++sample) {
        assert(controller.update(network(50.0, 50.0)).level == 5);
    }

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(49.9, 50.0)).level == 5);
    }
    assert(controller.update(network(49.9, 50.0)).level == 4);
}

void test_l1_recovers_to_l0_below_hysteresis_threshold() {
    AdaptationController controller(2000);
    assert(controller.update(network(10.0, 50.0)).level == 1);

    for (int sample = 0; sample < 10; ++sample) {
        assert(controller.update(network(3.0, 50.0)).level == 1);
    }

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(2.9, 50.0)).level == 1);
    }
    assert(controller.update(network(2.9, 50.0)).level == 0);
}

void test_recovery_requires_five_healthy_windows() {
    AdaptationController controller(2000);
    assert(controller.update(network(70.0)).level == 6);

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(4.5, 50.0)).level == 6);
    }
    assert(controller.update(network(4.5, 50.0)).level == 5);

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(4.5, 50.0)).level == 5);
    }
    assert(controller.update(network(4.5, 50.0)).level == 4);

    assert(controller.update(network(5.0, 50.0)).level == 4);
    for (int sample = 0; sample < 3; ++sample) {
        assert(controller.update(network(0.0, 50.0)).level == 4);
    }
    assert(controller.update(network(0.0, 50.0)).level == 3);
}

void test_emergency_recovers_to_current_network_level() {
    AdaptationController controller(2000);
    assert(controller.update(network(100.0, 500.0)).level == 8);

    for (int target_level = 7; target_level >= 5; --target_level) {
        for (int sample = 0; sample < 4; ++sample) {
            assert(controller.update(network(50.0, 50.0)).level ==
                   target_level + 1);
        }
        assert(controller.update(network(50.0, 50.0)).level ==
               target_level);
    }

    for (int sample = 0; sample < 10; ++sample) {
        assert(controller.update(network(50.0, 50.0)).level == 5);
    }
}

void test_high_loss_recovers_toward_lower_required_level() {
    AdaptationController controller(2000);
    assert(controller.update(network(10.0)).level == 1);
    assert(controller.update(network(61.0)).level == 5);

    for (int sample = 0; sample < 4; ++sample) {
        assert(controller.update(network(10.0, 50.0)).level == 5);
    }
    assert(controller.update(network(10.0, 50.0)).level == 4);

    for (int sample = 0; sample < 14; ++sample) {
        controller.update(network(10.0, 50.0));
    }
    assert(controller.update(network(10.0, 50.0)).level == 1);

    for (int sample = 0; sample < 5; ++sample) {
        assert(controller.update(network(10.0, 50.0)).level == 1);
    }
}

void test_max_video_bitrate_selects_supported_profile() {
    AdaptationController capped(1000);
    expect_profile(
        capped.current(), 1, 900, 24, 640, 360, 12, 4,
        0.50, 0.75, false);

    AdaptationController minimum(1);
    expect_profile(
        minimum.current(), 8, 60, 10, 160, 90, 1, 1,
        12.00, 12.00, true);
}

void test_invalid_snapshot_keeps_current_profile() {
    AdaptationController controller(2000);
    assert(controller.update(network(61.0)).level == 5);

    NetworkSnapshot invalid;
    assert(controller.update(invalid).level == 5);
}

void test_udp_recovery_profile() {
    expect_profile(
        udp_recovery_profile(2000),
        8, 60, 10, 160, 90, 1, 1, 12.0, 12.0, true);
    expect_profile(
        udp_recovery_profile(1),
        8, 60, 10, 160, 90, 1, 1, 12.0, 12.0, true);

    AdaptationController controller(2000);
    controller.force_emergency();
    expect_profile(
        controller.current(),
        8, 60, 10, 160, 90, 1, 1, 12.0, 12.0, true);
}

void test_reset_to_network_after_udp_recovery() {
    AdaptationController controller(2000);
    controller.force_emergency();
    expect_profile(
        controller.reset_to_network(network(0.0, 50.0)),
        0, 2000, 30, 1280, 720, 30, 4, 0.10, 0.30, false);

    controller.force_emergency();
    expect_profile(
        controller.reset_to_network(network(70.0, 50.0)),
        6, 150, 12, 320, 180, 2, 2, 5.00, 8.00, false);
}

void test_udp_recovery_ignores_stale_feedback_before_receiver_seen() {
    assert(!udp_recovery_required(
        false, true, false, std::optional<double>{}));
    assert(udp_recovery_required(
        true, true, false, std::optional<double>{}));
    assert(udp_recovery_required(
        true, false, true, std::optional<double>{}));
    assert(udp_recovery_required(
        true, false, false, std::optional<double>{86.0}));
}

void test_udp_send_failure_waits_for_receiver_before_recovery() {
    assert(!udp_send_failure_requires_recovery(false));
    assert(udp_send_failure_requires_recovery(true));
}

} // namespace

int main() {
    test_profile_ladder_and_loss_boundaries();
    test_rtt_requires_sustained_confirmation();
    test_rtt_spike_does_not_degrade_profile();
    test_rtt_diagnostics_track_confirmation();
    test_low_loss_l2_requires_confirmation();
    test_loss_degradation_is_immediate_for_high_loss();
    test_fifty_percent_loss_boundary_stays_at_level_four();
    test_level_five_requires_sustained_or_clear_loss();
    test_level_five_recovery_requires_below_fifty_percent();
    test_l1_recovers_to_l0_below_hysteresis_threshold();
    test_recovery_requires_five_healthy_windows();
    test_emergency_recovers_to_current_network_level();
    test_high_loss_recovers_toward_lower_required_level();
    test_max_video_bitrate_selects_supported_profile();
    test_invalid_snapshot_keeps_current_profile();
    test_udp_recovery_profile();
    test_reset_to_network_after_udp_recovery();
    test_udp_recovery_ignores_stale_feedback_before_receiver_seen();
    test_udp_send_failure_waits_for_receiver_before_recovery();
    std::cout << "adaptation_tests=passed" << std::endl;
    return 0;
}
