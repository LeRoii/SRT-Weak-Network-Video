#pragma once

#include "common/types.hpp"

#include <optional>

class AdaptationController {
public:
    explicit AdaptationController(int max_video_kbps);

    VideoProfile update(const NetworkSnapshot &network);
    VideoProfile current() const;
    void force_emergency();
    VideoProfile reset_to_network(const NetworkSnapshot &network);

private:
    VideoProfile profile_for_level(int level) const;

    int max_video_kbps_ = 2000;
    int level_ = 0;
    int healthy_windows_ = 0;
    bool emergency_recovery_active_ = false;
    VideoProfile current_;
};

VideoProfile udp_recovery_profile(int max_video_kbps);
bool udp_recovery_required(bool have_feedback,
                           bool feedback_stale,
                           bool receiver_stalled,
                           std::optional<double> loss_percent);
bool udp_send_failure_requires_recovery(bool have_feedback);
