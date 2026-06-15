#include "common/adaptation.hpp"

#include <algorithm>
#include <array>

namespace {

constexpr std::array<VideoProfile, 9> kProfiles{{
    {0, 2000, 30, 1280, 720, 0.15, false},
    {1, 1400, 20, 1280, 720, 0.25, false},
    {2, 900, 10, 960, 540, 0.50, false},
    {3, 500, 5, 640, 360, 0.75, false},
    {4, 250, 3, 426, 240, 1.00, false},
    {5, 120, 2, 426, 240, 2.00, true},
    {6, 50, 1, 320, 180, 3.00, true},
    {7, 20, 1, 160, 90, 5.00, true},
    {8, 8, 1, 128, 72, 11.00, true},
}};

} // namespace

AdaptationController::AdaptationController(int max_video_kbps)
    : max_video_kbps_(std::clamp(max_video_kbps, 8, 2000)) {
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

    int required = 0;
    if (network.loss_percent > 90.0) {
        required = 8;
    } else if (network.loss_percent > 80.0) {
        required = 7;
    } else if (network.loss_percent > 70.0) {
        required = 6;
    } else if (network.loss_percent > 50.0) {
        required = 5;
    } else if (network.loss_percent > 30.0) {
        required = 4;
    } else if (network.loss_percent > 15.0) {
        required = 3;
    } else if (network.loss_percent > 7.0) {
        required = 2;
    } else if (network.loss_percent > 3.0) {
        required = 1;
    }

    if (network.rtt_ms > 400.0) {
        required = std::max(required, 7);
    } else if (network.rtt_ms > 300.0) {
        required = std::max(required, 6);
    } else if (network.rtt_ms > 220.0) {
        required = std::max(required, 5);
    } else if (network.rtt_ms > 150.0) {
        required = std::max(required, 3);
    }

    if (required > level_) {
        level_ = required;
        healthy_windows_ = 0;
        emergency_recovery_active_ = required == 8;
    } else if (required < level_ &&
               ((network.loss_percent < 3.0 &&
                 network.rtt_ms < 130.0) ||
                emergency_recovery_active_)) {
        if (++healthy_windows_ >= 5) {
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

VideoProfile AdaptationController::profile_for_level(int level) const {
    VideoProfile profile =
        kProfiles[static_cast<std::size_t>(
            std::clamp(level, 0, static_cast<int>(kProfiles.size() - 1)))];
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, max_video_kbps_);
    return profile;
}

VideoProfile udp_recovery_profile(int max_video_kbps) {
    VideoProfile profile{8, 8, 1, 128, 72, 20.0, true};
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, std::max(8, max_video_kbps));
    return profile;
}

VideoProfile udp_profile_for_loss(double loss_percent,
                                  int max_video_kbps) {
    VideoProfile profile;
    if (loss_percent > 85.0) {
        return udp_recovery_profile(max_video_kbps);
    }
    if (loss_percent > 75.0) {
        profile = {7, 20, 1, 160, 90, 15.0, true};
    } else {
        profile = {6, 50, 1, 320, 180, 8.0, true};
    }
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, std::max(8, max_video_kbps));
    return profile;
}
