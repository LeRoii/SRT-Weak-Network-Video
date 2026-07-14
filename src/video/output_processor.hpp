#pragma once

#include "common/runtime_config.hpp"

#include <cstdint>

struct AVFrame;

struct OutputDimensions {
    int width = 0;
    int height = 0;

    bool operator==(const OutputDimensions &other) const {
        return width == other.width && height == other.height;
    }
};

enum class OutputAction {
    None,
    CurrentReal,
    BlendSynthetic,
    RepeatSynthetic,
};

class OutputProcessor {
public:
    OutputProcessor(int minimum_width,
                    int minimum_height,
                    UpscaleMode upscale_mode);

    OutputDimensions dimensions_for(int source_width,
                                    int source_height) const;
    AVFrame *process(const AVFrame *source) const;
    static AVFrame *blend(const AVFrame *previous,
                          const AVFrame *current);

private:
    int minimum_width_ = 640;
    int minimum_height_ = 360;
    UpscaleMode upscale_mode_ = UpscaleMode::LanczosSharpen;
};

class OutputCadenceController {
public:
    OutputCadenceController(int minimum_fps,
                            InterpolationMode interpolation_mode);

    OutputAction on_real_frame(int64_t now_ms, bool can_blend);
    OutputAction on_tick(int64_t now_ms);
    int64_t next_output_at_ms() const;

private:
    int64_t interval_ms_ = 200;
    InterpolationMode interpolation_mode_ =
        InterpolationMode::Blend;
    int64_t last_real_at_ms_ = -1;
    int64_t next_output_at_ms_ = -1;
    bool low_rate_mode_ = false;
    bool blend_pending_ = false;
    bool real_pending_ = false;
    int64_t synthetic_credit_ms_ = 0;
};
