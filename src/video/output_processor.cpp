#include "video/output_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

int even_ceiling(double value) {
    int result = static_cast<int>(std::ceil(value - 1e-9));
    if (result % 2 != 0) {
        ++result;
    }
    return result;
}

AVFrame *allocate_yuv420p(int width, int height) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

void sharpen_luma(AVFrame *frame) {
    const int width = frame->width;
    const int height = frame->height;
    if (width < 3 || height < 3) {
        return;
    }

    std::vector<uint8_t> source(
        static_cast<std::size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        std::copy_n(
            frame->data[0] + y * frame->linesize[0],
            width,
            source.data() + static_cast<std::size_t>(y * width));
    }

    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y * width + x);
            const int center = source[index];
            const int blur =
                (center * 4 +
                 source[index - 1] + source[index + 1] +
                 source[index - static_cast<std::size_t>(width)] +
                 source[index + static_cast<std::size_t>(width)]) /
                8;
            const int sharpened =
                center + (center - blur) * 3 / 8;
            frame->data[0][y * frame->linesize[0] + x] =
                static_cast<uint8_t>(
                    std::clamp(sharpened, 0, 255));
        }
    }
}

bool blend_compatible(const AVFrame *left, const AVFrame *right) {
    return left && right &&
           left->format == AV_PIX_FMT_YUV420P &&
           right->format == AV_PIX_FMT_YUV420P &&
           left->width == right->width &&
           left->height == right->height;
}

} // namespace

OutputProcessor::OutputProcessor(int minimum_width,
                                 int minimum_height,
                                 UpscaleMode upscale_mode)
    : minimum_width_(minimum_width),
      minimum_height_(minimum_height),
      upscale_mode_(upscale_mode) {}

OutputDimensions OutputProcessor::dimensions_for(
    int source_width,
    int source_height) const {
    if (source_width >= minimum_width_ &&
        source_height >= minimum_height_) {
        return {source_width, source_height};
    }

    const double scale = std::max(
        static_cast<double>(minimum_width_) / source_width,
        static_cast<double>(minimum_height_) / source_height);
    return {
        even_ceiling(source_width * scale),
        even_ceiling(source_height * scale),
    };
}

AVFrame *OutputProcessor::process(const AVFrame *source) const {
    if (!source || source->width <= 0 || source->height <= 0) {
        return nullptr;
    }
    const auto dimensions =
        dimensions_for(source->width, source->height);
    const bool scaling =
        dimensions.width != source->width ||
        dimensions.height != source->height;
    if (!scaling && source->format == AV_PIX_FMT_YUV420P) {
        return av_frame_clone(source);
    }

    AVFrame *output =
        allocate_yuv420p(dimensions.width, dimensions.height);
    if (!output) {
        return nullptr;
    }

    const int scaling_flags =
        scaling && upscale_mode_ == UpscaleMode::LanczosSharpen
            ? SWS_LANCZOS
            : SWS_BILINEAR;
    SwsContext *context = sws_getContext(
        source->width,
        source->height,
        static_cast<AVPixelFormat>(source->format),
        dimensions.width,
        dimensions.height,
        AV_PIX_FMT_YUV420P,
        scaling_flags,
        nullptr,
        nullptr,
        nullptr);
    if (!context) {
        av_frame_free(&output);
        return nullptr;
    }
    const int rows = sws_scale(
        context,
        source->data,
        source->linesize,
        0,
        source->height,
        output->data,
        output->linesize);
    sws_freeContext(context);
    if (rows != dimensions.height) {
        av_frame_free(&output);
        return nullptr;
    }
    av_frame_copy_props(output, source);

    if (scaling &&
        upscale_mode_ == UpscaleMode::LanczosSharpen) {
        sharpen_luma(output);
    }
    return output;
}

AVFrame *OutputProcessor::blend(const AVFrame *previous,
                                const AVFrame *current) {
    if (!blend_compatible(previous, current)) {
        return nullptr;
    }
    AVFrame *output =
        allocate_yuv420p(current->width, current->height);
    if (!output) {
        return nullptr;
    }

    for (int plane = 0; plane < 3; ++plane) {
        const int width =
            plane == 0 ? current->width : current->width / 2;
        const int height =
            plane == 0 ? current->height : current->height / 2;
        for (int y = 0; y < height; ++y) {
            const uint8_t *left =
                previous->data[plane] +
                y * previous->linesize[plane];
            const uint8_t *right =
                current->data[plane] +
                y * current->linesize[plane];
            uint8_t *destination =
                output->data[plane] +
                y * output->linesize[plane];
            for (int x = 0; x < width; ++x) {
                destination[x] = static_cast<uint8_t>(
                    (static_cast<unsigned>(left[x]) +
                     static_cast<unsigned>(right[x]) + 1U) /
                    2U);
            }
        }
    }
    av_frame_copy_props(output, current);
    return output;
}

OutputCadenceController::OutputCadenceController(
    int minimum_fps,
    InterpolationMode interpolation_mode)
    : interval_ms_(std::max<int64_t>(1, 1000 / minimum_fps)),
      interpolation_mode_(interpolation_mode) {}

OutputAction OutputCadenceController::on_real_frame(
    int64_t now_ms,
    bool can_blend) {
    const bool have_previous = last_real_at_ms_ >= 0;
    const int64_t gap_ms =
        have_previous ? now_ms - last_real_at_ms_ : 0;
    last_real_at_ms_ = now_ms;

    if (!have_previous || gap_ms <= interval_ms_) {
        low_rate_mode_ = false;
        blend_pending_ = false;
        real_pending_ = false;
        next_output_at_ms_ = now_ms + interval_ms_;
        return OutputAction::CurrentReal;
    }

    low_rate_mode_ = true;
    blend_pending_ =
        interpolation_mode_ == InterpolationMode::Blend &&
        can_blend;
    real_pending_ = true;
    if (next_output_at_ms_ < now_ms) {
        next_output_at_ms_ = now_ms;
    }
    return OutputAction::None;
}

OutputAction OutputCadenceController::on_tick(int64_t now_ms) {
    if (last_real_at_ms_ < 0 ||
        next_output_at_ms_ < 0 ||
        now_ms < next_output_at_ms_) {
        return OutputAction::None;
    }
    next_output_at_ms_ += interval_ms_;
    if (next_output_at_ms_ <= now_ms) {
        next_output_at_ms_ = now_ms + interval_ms_;
    }
    if (!low_rate_mode_) {
        if (now_ms - last_real_at_ms_ < interval_ms_) {
            return OutputAction::None;
        }
        low_rate_mode_ = true;
    }
    if (blend_pending_) {
        blend_pending_ = false;
        return OutputAction::BlendSynthetic;
    }
    if (real_pending_) {
        real_pending_ = false;
        return OutputAction::CurrentReal;
    }
    return OutputAction::RepeatSynthetic;
}

int64_t OutputCadenceController::next_output_at_ms() const {
    return next_output_at_ms_;
}
