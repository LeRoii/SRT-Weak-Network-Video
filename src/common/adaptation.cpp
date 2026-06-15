#include "common/adaptation.hpp"

#include <algorithm>
#include <array>

namespace {

constexpr std::array<VideoProfile, 9> kProfiles{{
    {0, 2000, 30, 1280, 720, 0.10, 0.30, false},
    {1, 1200, 20, 960, 540, 0.15, 0.50, false},
    {2, 700, 15, 640, 360, 0.25, 0.75, false},
    {3, 400, 10, 640, 360, 0.40, 1.00, false},
    {4, 220, 5, 426, 240, 0.75, 2.00, false},
    {5, 140, 3, 426, 240, 1.25, 3.00, false},
    {6, 80, 3, 320, 180, 2.50, 5.00, false},
    {7, 50, 2, 320, 180, 4.00, 7.00, false},
    {8, 30, 2, 256, 144, 8.00, 12.00, false},
}};

} // namespace

AdaptationController::AdaptationController(int max_video_kbps)
    : max_video_kbps_(std::clamp(max_video_kbps, 30, 2000)) {
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
    if (network.loss_percent > 77.0) {
        required = 8;
    } else if (network.loss_percent > 72.0) {
        required = 7;
    } else if (network.loss_percent > 65.0) {
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

void AdaptationController::force_emergency() {
    level_ = static_cast<int>(kProfiles.size()) - 1;
    healthy_windows_ = 0;
    emergency_recovery_active_ = true;
    current_ = profile_for_level(level_);
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
        std::min(profile.bitrate_kbps, std::max(30, max_video_kbps));
    return profile;
}
