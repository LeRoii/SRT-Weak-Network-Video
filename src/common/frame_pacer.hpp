#pragma once

#include <cstdint>

class FramePacer {
public:
    void reset(int64_t now_us);
    int64_t delay_after_frame(int64_t now_us, int64_t interval_us);

private:
    bool initialized_ = false;
    int64_t next_frame_at_us_ = 0;
};
