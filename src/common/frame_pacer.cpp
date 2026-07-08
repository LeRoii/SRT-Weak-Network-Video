#include "common/frame_pacer.hpp"

#include <algorithm>

void FramePacer::reset(int64_t now_us) {
    initialized_ = true;
    next_frame_at_us_ = now_us;
}

int64_t FramePacer::delay_after_frame(int64_t now_us, int64_t interval_us) {
    if (!initialized_) {
        reset(now_us);
    }

    const int64_t safe_interval_us = std::max<int64_t>(0, interval_us);
    next_frame_at_us_ += safe_interval_us;
    if (next_frame_at_us_ <= now_us) {
        next_frame_at_us_ = now_us;
        return 0;
    }
    return next_frame_at_us_ - now_us;
}
