#include "common/adaptation.hpp"

#include <algorithm>
#include <array>

namespace {

constexpr std::array<VideoProfile, 7> kProfiles{{
    {0, 2000, 30, 1280, 720, 0.15, false},
    {1, 1200, 24, 960, 540, 0.25, false},
    {2, 600, 15, 640, 360, 0.50, false},
    {3, 300, 10, 426, 240, 1.00, false},
    {4, 150, 5, 320, 180, 1.50, false},
    {5, 50, 2, 160, 90, 3.00, true},
    {6, 8, 1, 128, 72, 11.00, true},
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
    if (network.loss_percent > 70.0) {
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

    if (network.rtt_ms > 350.0) {
        required = std::max(required, 5);
    } else if (network.rtt_ms > 220.0) {
        required = std::max(required, 4);
    } else if (network.rtt_ms > 150.0) {
        required = std::max(required, 2);
    }

    if (required > level_) {
        level_ = required;
        healthy_windows_ = 0;
    } else if (required < level_ &&
               network.loss_percent < 3.0 &&
               network.rtt_ms < 130.0) {
        if (++healthy_windows_ >= 5) {
            --level_;
            healthy_windows_ = 0;
        }
    } else {
        healthy_windows_ = 0;
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
