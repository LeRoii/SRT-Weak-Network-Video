#include "video/output_processor.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

AVFrame *make_frame(int width, int height, uint8_t value) {
    AVFrame *frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    assert(av_frame_get_buffer(frame, 32) == 0);
    assert(av_frame_make_writable(frame) == 0);
    for (int plane = 0; plane < 3; ++plane) {
        const int plane_width = plane == 0 ? width : width / 2;
        const int plane_height = plane == 0 ? height : height / 2;
        for (int y = 0; y < plane_height; ++y) {
            for (int x = 0; x < plane_width; ++x) {
                frame->data[plane][
                    y * frame->linesize[plane] + x] =
                    static_cast<uint8_t>(
                        value + ((x + y) % 7));
            }
        }
    }
    return frame;
}

int luma_range(const AVFrame *frame) {
    uint8_t minimum = 255;
    uint8_t maximum = 0;
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            const uint8_t value =
                frame->data[0][y * frame->linesize[0] + x];
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    return static_cast<int>(maximum) - static_cast<int>(minimum);
}

int count_outputs_for_input_interval(int input_interval_ms,
                                     int target_fps) {
    OutputCadenceController cadence(
        target_fps, InterpolationMode::Blend);
    int outputs = 0;
    int next_real_ms = 0;
    for (int now_ms = 0; now_ms < 1000; ++now_ms) {
        if (now_ms == next_real_ms) {
            outputs += cadence.on_real_frame(now_ms, true) !=
                       OutputAction::None;
            next_real_ms += input_interval_ms;
        }
        outputs += cadence.on_tick(now_ms) != OutputAction::None;
    }
    return outputs;
}

int count_synthetic_for_input_interval(int input_interval_ms,
                                       int target_fps) {
    OutputCadenceController cadence(
        target_fps, InterpolationMode::Blend);
    int synthetic = 0;
    int next_real_ms = 0;
    auto count_synthetic = [&synthetic](OutputAction action) {
        if (action == OutputAction::BlendSynthetic ||
            action == OutputAction::RepeatSynthetic) {
            ++synthetic;
        }
    };
    for (int now_ms = 0; now_ms < 1000; ++now_ms) {
        if (now_ms == next_real_ms) {
            count_synthetic(cadence.on_real_frame(now_ms, true));
            next_real_ms += input_interval_ms;
        }
        count_synthetic(cadence.on_tick(now_ms));
    }
    return synthetic;
}

void test_dimensions_and_scaling() {
    OutputProcessor bilinear(
        640, 360, UpscaleMode::Bilinear);
    OutputProcessor lanczos(
        640, 360, UpscaleMode::LanczosSharpen);

    assert((bilinear.dimensions_for(1280, 720) ==
            OutputDimensions{1280, 720}));
    assert((bilinear.dimensions_for(640, 360) ==
            OutputDimensions{640, 360}));
    assert((bilinear.dimensions_for(320, 180) ==
            OutputDimensions{640, 360}));
    assert((bilinear.dimensions_for(320, 240) ==
            OutputDimensions{640, 480}));
    assert((bilinear.dimensions_for(426, 240) ==
            OutputDimensions{640, 362}));

    AVFrame *large = make_frame(640, 360, 40);
    AVFrame *unchanged = lanczos.process(large);
    assert(unchanged);
    assert(unchanged->width == 640);
    assert(unchanged->height == 360);
    assert(unchanged->data[0][100] == large->data[0][100]);

    AVFrame *small = make_frame(320, 180, 20);
    AVFrame *bilinear_output = bilinear.process(small);
    AVFrame *lanczos_output = lanczos.process(small);
    assert(bilinear_output && lanczos_output);
    assert(bilinear_output->width == 640);
    assert(bilinear_output->height == 360);
    assert(lanczos_output->width == 640);
    assert(lanczos_output->height == 360);

    av_frame_free(&large);
    av_frame_free(&unchanged);
    av_frame_free(&small);
    av_frame_free(&bilinear_output);
    av_frame_free(&lanczos_output);
}

void test_lanczos_enhances_low_contrast_upscaled_frames() {
    OutputProcessor lanczos(
        640, 360, UpscaleMode::LanczosSharpen);
    AVFrame *small = make_frame(160, 90, 100);
    AVFrame *enhanced = lanczos.process(small);
    assert(enhanced);
    assert(enhanced->width == 640);
    assert(enhanced->height == 360);
    assert(luma_range(enhanced) >= 32);

    av_frame_free(&small);
    av_frame_free(&enhanced);
}

void test_blend() {
    AVFrame *previous = make_frame(640, 360, 20);
    AVFrame *current = make_frame(640, 360, 100);
    AVFrame *blended =
        OutputProcessor::blend(previous, current);
    assert(blended);
    assert(blended->data[0][0] == 60);
    assert(blended->data[1][0] == 60);
    assert(blended->data[2][0] == 60);

    AVFrame *incompatible = make_frame(320, 180, 50);
    assert(!OutputProcessor::blend(previous, incompatible));

    av_frame_free(&previous);
    av_frame_free(&current);
    av_frame_free(&blended);
    av_frame_free(&incompatible);
}

void test_repeat_cadence() {
    OutputCadenceController cadence(
        5, InterpolationMode::Repeat);
    assert(cadence.on_real_frame(0, false) ==
           OutputAction::CurrentReal);
    assert(cadence.on_tick(199) == OutputAction::None);
    assert(cadence.on_tick(200) ==
           OutputAction::RepeatSynthetic);
    assert(cadence.on_tick(400) ==
           OutputAction::RepeatSynthetic);
    assert(cadence.on_real_frame(500, true) ==
           OutputAction::None);
    assert(cadence.on_tick(600) ==
           OutputAction::CurrentReal);
    assert(cadence.on_tick(800) ==
           OutputAction::RepeatSynthetic);
}

void test_blend_cadence() {
    OutputCadenceController cadence(
        5, InterpolationMode::Blend);
    assert(cadence.on_real_frame(0, false) ==
           OutputAction::CurrentReal);
    assert(cadence.on_tick(200) ==
           OutputAction::RepeatSynthetic);
    assert(cadence.on_real_frame(500, true) ==
           OutputAction::None);
    assert(cadence.on_tick(600) ==
           OutputAction::BlendSynthetic);
    assert(cadence.on_tick(800) ==
           OutputAction::CurrentReal);
    assert(cadence.on_tick(1000) ==
           OutputAction::RepeatSynthetic);
}

void test_high_rate_passthrough() {
    OutputCadenceController ten_fps(
        5, InterpolationMode::Blend);
    for (int frame = 0; frame < 10; ++frame) {
        const int64_t at = frame * 100;
        assert(ten_fps.on_real_frame(at, true) ==
               OutputAction::CurrentReal);
        assert(ten_fps.on_tick(at) == OutputAction::None);
    }

    OutputCadenceController thirty_fps(
        5, InterpolationMode::Blend);
    for (int frame = 0; frame < 30; ++frame) {
        assert(thirty_fps.on_real_frame(frame * 33, true) ==
               OutputAction::CurrentReal);
    }
}

void test_two_and_three_fps_reach_minimum_output() {
    OutputCadenceController two_fps(
        5, InterpolationMode::Blend);
    int two_fps_outputs = 0;
    two_fps_outputs +=
        two_fps.on_real_frame(0, false) != OutputAction::None;
    two_fps_outputs +=
        two_fps.on_tick(200) != OutputAction::None;
    two_fps_outputs +=
        two_fps.on_tick(400) != OutputAction::None;
    (void)two_fps.on_real_frame(500, true);
    two_fps_outputs +=
        two_fps.on_tick(600) != OutputAction::None;
    two_fps_outputs +=
        two_fps.on_tick(800) != OutputAction::None;
    assert(two_fps_outputs >= 5);

    OutputCadenceController three_fps(
        5, InterpolationMode::Blend);
    int three_fps_outputs = 0;
    three_fps_outputs +=
        three_fps.on_real_frame(0, false) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_tick(200) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_real_frame(333, true) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_tick(400) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_tick(600) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_real_frame(666, true) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_tick(800) != OutputAction::None;
    three_fps_outputs +=
        three_fps.on_real_frame(999, true) != OutputAction::None;
    assert(three_fps_outputs >= 5);
}

void test_eight_ten_and_twelve_fps_reach_twenty_output() {
    assert(count_outputs_for_input_interval(125, 20) >= 20);
    assert(count_outputs_for_input_interval(100, 20) >= 20);
    assert(count_outputs_for_input_interval(83, 20) >= 20);
}

void test_twenty_to_twenty_four_fps_reach_near_thirty_output() {
    assert(count_outputs_for_input_interval(50, 30) >= 28);
    assert(count_outputs_for_input_interval(45, 30) >= 28);
    assert(count_outputs_for_input_interval(42, 30) >= 28);
    assert(count_outputs_for_input_interval(42, 30) <= 32);
    assert(count_synthetic_for_input_interval(42, 30) <= 10);
}

void test_thirty_fps_input_does_not_trigger_synthetic_cadence() {
    assert(count_synthetic_for_input_interval(33, 30) <= 1);
}

void test_twenty_fps_cadence_has_scheduler_headroom() {
    OutputCadenceController cadence(
        20, InterpolationMode::Repeat);
    int outputs = 0;
    outputs += cadence.on_real_frame(0, false) != OutputAction::None;
    for (int now_ms = 0; now_ms < 1000; ++now_ms) {
        outputs += cadence.on_tick(now_ms) != OutputAction::None;
    }
    assert(outputs >= 21);
}

} // namespace

int main() {
    test_dimensions_and_scaling();
    test_lanczos_enhances_low_contrast_upscaled_frames();
    test_blend();
    test_repeat_cadence();
    test_blend_cadence();
    test_high_rate_passthrough();
    test_two_and_three_fps_reach_minimum_output();
    test_eight_ten_and_twelve_fps_reach_twenty_output();
    test_twenty_to_twenty_four_fps_reach_near_thirty_output();
    test_thirty_fps_input_does_not_trigger_synthetic_cadence();
    test_twenty_fps_cadence_has_scheduler_headroom();
    std::cout << "output_processing_tests=passed" << std::endl;
    return 0;
}
