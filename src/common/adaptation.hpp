#pragma once

#include "common/types.hpp"

#include <optional>

struct AdaptationDiagnostics {
    int loss_required_level = 0;
    int raw_rtt_required_level = 0;
    int confirmed_rtt_required_level = 0;
    int rtt_high_windows = 0;
};

class AdaptationController {
public:
    explicit AdaptationController(int max_video_kbps);

    VideoProfile update(const NetworkSnapshot &network);
    VideoProfile current() const;
    AdaptationDiagnostics diagnostics() const;
    void force_emergency();
    VideoProfile reset_to_network(const NetworkSnapshot &network);

private:
    int required_level_for_network(const NetworkSnapshot &network,
                                   bool update_confirmations);
    int loss_required_level_for_network(double loss_percent,
                                        bool update_confirmation);
    void reset_rtt_confirmation();
    void reset_loss_confirmation();
    VideoProfile profile_for_level(int level) const;

    int max_video_kbps_ = 2000;
    int level_ = 0;
    int healthy_windows_ = 0;
    int loss_candidate_level_ = 0;
    int loss_candidate_windows_ = 0;
    int rtt_candidate_level_ = 0;
    int rtt_high_windows_ = 0;
    int confirmed_rtt_required_level_ = 0;
    bool emergency_recovery_active_ = false;
    AdaptationDiagnostics diagnostics_;
    VideoProfile current_;
};

VideoProfile udp_recovery_profile(int max_video_kbps);
bool udp_recovery_required(bool have_feedback,
                           bool feedback_stale,
                           bool receiver_stalled,
                           std::optional<double> loss_percent);
bool udp_send_failure_requires_recovery(bool have_feedback);
