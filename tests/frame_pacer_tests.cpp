#include "common/frame_pacer.hpp"

#include <cassert>
#include <iostream>

namespace {

void test_processing_time_is_subtracted_from_sleep() {
    FramePacer pacer;
    pacer.reset(0);

    const int64_t interval_us = 33'333;
    assert(pacer.delay_after_frame(12'000, interval_us) == 21'333);
    assert(pacer.delay_after_frame(45'333, interval_us) == 21'333);
}

void test_late_frame_does_not_try_to_catch_up_with_burst() {
    FramePacer pacer;
    pacer.reset(0);

    const int64_t interval_us = 33'333;
    assert(pacer.delay_after_frame(40'000, interval_us) == 0);
    assert(pacer.delay_after_frame(52'000, interval_us) == 21'333);
}

} // namespace

int main() {
    test_processing_time_is_subtracted_from_sleep();
    test_late_frame_does_not_try_to_catch_up_with_burst();
    std::cout << "frame_pacer_tests=passed" << std::endl;
    return 0;
}
