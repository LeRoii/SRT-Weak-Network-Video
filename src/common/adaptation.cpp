#include "common/adaptation.hpp"

#include <algorithm>
#include <array>

namespace {

constexpr std::array<VideoProfile, 9> kProfiles{{
    {0, 2000, 30, 1280, 720, 30, 4, 0.10, 0.30, false},
    {1, 900, 24, 640, 360, 12, 4, 0.50, 0.75, false},
    {2, 700, 24, 640, 360, 12, 4, 0.50, 0.75, false},
    {3, 500, 20, 640, 360, 10, 4, 1.00, 2.00, false},
    {4, 350, 20, 426, 240, 10, 2, 2.00, 4.00, false},
    {5, 220, 15, 320, 180, 3, 2, 3.00, 6.00, false},
    {6, 150, 12, 320, 180, 2, 2, 5.00, 8.00, false},
    {7, 90, 12, 256, 144, 1, 1, 8.00, 10.00, true},
    {8, 60, 10, 160, 90, 1, 1, 12.00, 12.00, true},
}};

constexpr int kLossL2ConfirmationWindows = 3;
constexpr int kLossL5ConfirmationWindows = 3;
constexpr int kProfileRecoveryWindows = 5;
constexpr int kRttConfirmationWindows = 5;
constexpr double kL2RecoveryLossPercent = 12.0;
constexpr double kL0RecoveryLossPercent = 3.0;
constexpr double kL5ConfirmedEntryLossPercent = 55.0;
constexpr double kL5ImmediateEntryLossPercent = 60.0;
constexpr double kL5RecoveryLossPercent = 50.0;

int loss_required_level(double loss_percent) {
    if (loss_percent > 77.0) {
        return 8;
    }
    if (loss_percent > 72.0) {
        return 7;
    }
    if (loss_percent > 65.0) {
        return 6;
    }
    if (loss_percent > 50.0) {
        return 5;
    }
    if (loss_percent > 30.0) {
        return 4;
    }
    if (loss_percent > 20.0) {
        return 3;
    }
    if (loss_percent > 15.0) {
        return 2;
    }
    if (loss_percent > 5.0) {
        return 1;
    }
    return 0;
}

int rtt_required_level(double rtt_ms) {
    if (rtt_ms > 400.0) {
        return 7;
    }
    if (rtt_ms > 300.0) {
        return 6;
    }
    if (rtt_ms > 220.0) {
        return 5;
    }
    if (rtt_ms > 160.0) {
        return 3;
    }
    return 0;
}

} // namespace

AdaptationController::AdaptationController(int max_video_kbps)
    : max_video_kbps_(std::clamp(max_video_kbps, 60, 2000)) {
    while (level_ + 1 < static_cast<int>(kProfiles.size()) &&
           kProfiles[static_cast<std::size_t>(level_)].bitrate_kbps >
               max_video_kbps_) {
        ++level_;
    }
    current_ = profile_for_level(level_);
}

VideoProfile AdaptationController::update(const NetworkSnapshot &network) {
    if (!network.valid) {
        return current_;
    }

    const int required =
        required_level_for_network(network, true);

    if (required > level_) {
        level_ = required;
        healthy_windows_ = 0;
        emergency_recovery_active_ = required == 8;
    } else if (required < level_) {
        if (++healthy_windows_ >= kProfileRecoveryWindows) {
            --level_;
            healthy_windows_ = 0;
            if (level_ <= required) {
                emergency_recovery_active_ = false;
            }
        }
    } else {
        healthy_windows_ = 0;
        if (required == level_ && required < 8) {
            emergency_recovery_active_ = false;
        }
    }

    current_ = profile_for_level(level_);
    return current_;
}

VideoProfile AdaptationController::current() const {
    return current_;
}

AdaptationDiagnostics AdaptationController::diagnostics() const {
    return diagnostics_;
}

void AdaptationController::force_emergency() {
    level_ = static_cast<int>(kProfiles.size()) - 1;
    healthy_windows_ = 0;
    reset_rtt_confirmation();
    emergency_recovery_active_ = true;
    current_ = profile_for_level(level_);
}

VideoProfile AdaptationController::reset_to_network(
    const NetworkSnapshot &network) {
    if (!network.valid) {
        return current_;
    }
    reset_rtt_confirmation();
    reset_loss_confirmation();
    level_ = required_level_for_network(network, false);
    while (level_ + 1 < static_cast<int>(kProfiles.size()) &&
           kProfiles[static_cast<std::size_t>(level_)].bitrate_kbps >
               max_video_kbps_) {
        ++level_;
    }
    healthy_windows_ = 0;
    emergency_recovery_active_ = false;
    current_ = profile_for_level(level_);
    return current_;
}

int AdaptationController::required_level_for_network(
    const NetworkSnapshot &network,
    bool update_confirmations) {
    const int raw_loss_required =
        loss_required_level(network.loss_percent);
    const int loss_required =
        loss_required_level_for_network(network.loss_percent,
                                        update_confirmations);
    const int raw_rtt_required =
        rtt_required_level(network.rtt_ms);

    if (update_confirmations) {
        if (raw_rtt_required == 0) {
            reset_rtt_confirmation();
        } else {
            if (raw_rtt_required == rtt_candidate_level_) {
                ++rtt_high_windows_;
            } else {
                rtt_candidate_level_ = raw_rtt_required;
                rtt_high_windows_ = 1;
            }

            if (rtt_high_windows_ >= kRttConfirmationWindows) {
                confirmed_rtt_required_level_ =
                    rtt_candidate_level_;
            } else if (confirmed_rtt_required_level_ >
                       raw_rtt_required) {
                confirmed_rtt_required_level_ =
                    raw_rtt_required;
            }
        }
    }

    diagnostics_.raw_loss_required_level = raw_loss_required;
    diagnostics_.loss_required_level = loss_required;
    diagnostics_.loss_candidate_level = loss_candidate_level_;
    diagnostics_.loss_candidate_windows = loss_candidate_windows_;
    diagnostics_.raw_rtt_required_level = raw_rtt_required;
    diagnostics_.confirmed_rtt_required_level =
        confirmed_rtt_required_level_;
    diagnostics_.rtt_high_windows = rtt_high_windows_;

    return std::max(loss_required, confirmed_rtt_required_level_);
}

void AdaptationController::reset_rtt_confirmation() {
    rtt_candidate_level_ = 0;
    rtt_high_windows_ = 0;
    confirmed_rtt_required_level_ = 0;
}

int AdaptationController::loss_required_level_for_network(
    double loss_percent,
    bool update_confirmation) {
    const int raw_loss_required = loss_required_level(loss_percent);

    if (raw_loss_required >= 6) {
        if (update_confirmation) {
            reset_loss_confirmation();
        }
        return raw_loss_required;
    }

    if (level_ >= 5 && loss_percent >= kL5RecoveryLossPercent) {
        if (update_confirmation) {
            reset_loss_confirmation();
        }
        return 5;
    }

    if (raw_loss_required == 5) {
        if (loss_percent > kL5ImmediateEntryLossPercent) {
            if (update_confirmation) {
                reset_loss_confirmation();
            }
            return 5;
        }

        if (loss_percent > kL5ConfirmedEntryLossPercent) {
            if (update_confirmation) {
                if (loss_candidate_level_ == 5) {
                    ++loss_candidate_windows_;
                } else {
                    loss_candidate_level_ = 5;
                    loss_candidate_windows_ = 1;
                }
            }

            return loss_candidate_windows_ >=
                           kLossL5ConfirmationWindows
                       ? 5
                       : 4;
        }

        if (update_confirmation) {
            reset_loss_confirmation();
        }
        return 4;
    }

    if (raw_loss_required >= 3) {
        if (update_confirmation) {
            reset_loss_confirmation();
        }
        return raw_loss_required;
    }

    if (raw_loss_required == 2) {
        if (level_ >= 2) {
            if (update_confirmation) {
                reset_loss_confirmation();
            }
            return 2;
        }

        if (update_confirmation) {
            if (loss_candidate_level_ == 2) {
                ++loss_candidate_windows_;
            } else {
                loss_candidate_level_ = 2;
                loss_candidate_windows_ = 1;
            }
        }

        return loss_candidate_windows_ >=
                       kLossL2ConfirmationWindows
                   ? 2
                   : 1;
    }

    if (update_confirmation) {
        reset_loss_confirmation();
    }

    if (raw_loss_required == 1) {
        if (level_ >= 2 &&
            loss_percent >= kL2RecoveryLossPercent) {
            return 2;
        }
        return 1;
    }

    if (level_ >= 2 &&
        loss_percent >= kL2RecoveryLossPercent) {
        return 2;
    }

    if (level_ >= 1 &&
        loss_percent >= kL0RecoveryLossPercent) {
        return 1;
    }

    return 0;
}

void AdaptationController::reset_loss_confirmation() {
    loss_candidate_level_ = 0;
    loss_candidate_windows_ = 0;
}

VideoProfile AdaptationController::profile_for_level(int level) const {
    VideoProfile profile =
        kProfiles[static_cast<std::size_t>(
            std::clamp(level, 0, static_cast<int>(kProfiles.size() - 1)))];
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, max_video_kbps_);
    return profile;
}

VideoProfile udp_recovery_profile(int max_video_kbps) {
    VideoProfile profile = kProfiles.back();
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, std::max(60, max_video_kbps));
    return profile;
}

bool udp_recovery_required(bool have_feedback,
                           bool feedback_stale,
                           bool receiver_stalled,
                           std::optional<double> loss_percent) {
    return (have_feedback && feedback_stale) ||
           receiver_stalled ||
           (loss_percent && *loss_percent > 85.0);
}

bool udp_send_failure_requires_recovery(bool have_feedback) {
    return have_feedback;
}
