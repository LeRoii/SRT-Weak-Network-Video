#pragma once

#include "common/types.hpp"

class AdaptationController {
public:
    explicit AdaptationController(int max_video_kbps);

    VideoProfile update(const NetworkSnapshot &network);
    VideoProfile current() const;

private:
    VideoProfile profile_for_level(int level) const;

    int max_video_kbps_ = 2000;
    int level_ = 0;
    int healthy_windows_ = 0;
    bool emergency_recovery_active_ = false;
    VideoProfile current_;
};
