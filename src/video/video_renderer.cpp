#include "video/video_renderer.hpp"

#include "common/utils.hpp"

#include <SDL2/SDL.h>

#include <chrono>
#include <iostream>

extern "C" {
#include <libavutil/frame.h>
}

namespace {

constexpr std::size_t kMaxQueuedFrames = 3;

int64_t steady_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct CachedFrame {
    AVFrame *frame = nullptr;
    uint64_t latency_start_unix_us = 0;
    uint64_t latency_generation = 0;
    bool latency_recorded = false;
};

void clear_cached(CachedFrame &cached) {
    av_frame_free(&cached.frame);
    cached = {};
}

} // namespace

VideoRenderer::VideoRenderer(int minimum_output_width,
                             int minimum_output_height,
                             int minimum_output_fps,
                             UpscaleMode upscale_mode,
                             InterpolationMode interpolation_mode,
                             LatencyStats *latency_stats)
    : processor_(minimum_output_width,
                 minimum_output_height,
                 upscale_mode),
      cadence_(minimum_output_fps, interpolation_mode),
      interpolation_mode_(interpolation_mode),
      latency_stats_(latency_stats) {}

VideoRenderer::~VideoRenderer() {
    stop();
}

void VideoRenderer::start(bool enabled) {
    enabled_.store(enabled);
    stopping_.store(false);
    if (enabled) {
        thread_ = std::thread([this] { render_loop(); });
    }
}

void VideoRenderer::stop() {
    stopping_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    clear_queue();
}

void VideoRenderer::submit(const AVFrame *frame,
                           uint64_t latency_start_unix_us,
                           uint64_t latency_generation) {
    decoded_interval_frames_.fetch_add(1, std::memory_order_relaxed);
    if (!enabled_.load()) {
        return;
    }
    AVFrame *copy = av_frame_clone(frame);
    if (!copy) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (frames_.size() >= kMaxQueuedFrames) {
            AVFrame *old = frames_.front().frame;
            frames_.pop_front();
            av_frame_free(&old);
        }
        frames_.push_back(
            {copy,
             latency_start_unix_us,
             latency_generation,
             steady_time_ms()});
    }
    cv_.notify_one();
}

void VideoRenderer::render_loop() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "sdl_init_failed=" << SDL_GetError() << std::endl;
        return;
    }

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    int texture_width = 0;
    int texture_height = 0;
    CachedFrame previous;
    CachedFrame current;

    auto ensure_output = [&](int width, int height) {
        if (!window) {
            window = SDL_CreateWindow(
                "Weak-Network Receiver",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                width,
                height,
                0);
            renderer = window
                           ? SDL_CreateRenderer(
                                 window, -1, SDL_RENDERER_ACCELERATED)
                           : nullptr;
            if (window && !renderer) {
                renderer = SDL_CreateRenderer(
                    window, -1, SDL_RENDERER_SOFTWARE);
            }
        }
        if (!window || !renderer) {
            return false;
        }
        if (texture_width == width && texture_height == height &&
            texture) {
            return true;
        }
        SDL_SetWindowSize(window, width, height);
        if (texture) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            width,
            height);
        texture_width = width;
        texture_height = height;
        current_output_width_.store(width, std::memory_order_relaxed);
        current_output_height_.store(height, std::memory_order_relaxed);
        return texture != nullptr;
    };

    auto present = [&](OutputAction action) {
        const bool synthetic =
            action == OutputAction::BlendSynthetic ||
            action == OutputAction::RepeatSynthetic;
        AVFrame *temporary = nullptr;
        AVFrame *frame = current.frame;
        if (action == OutputAction::BlendSynthetic &&
            interpolation_mode_ == InterpolationMode::Blend) {
            temporary =
                OutputProcessor::blend(previous.frame, current.frame);
            if (temporary) {
                frame = temporary;
            } else if (previous.frame) {
                frame = previous.frame;
            }
        }
        if (!frame || !ensure_output(frame->width, frame->height)) {
            av_frame_free(&temporary);
            return;
        }

        SDL_UpdateYUVTexture(
            texture,
            nullptr,
            frame->data[0],
            frame->linesize[0],
            frame->data[1],
            frame->linesize[1],
            frame->data[2],
            frame->linesize[2]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        rendered_frames_.fetch_add(1, std::memory_order_relaxed);
        output_interval_frames_.fetch_add(
            1, std::memory_order_relaxed);
        if (synthetic) {
            synthetic_interval_frames_.fetch_add(
                1, std::memory_order_relaxed);
        } else if (!current.latency_recorded &&
                   latency_stats_ &&
                   current.latency_start_unix_us != 0) {
            latency_stats_->record(
                current.latency_generation,
                current.latency_start_unix_us,
                unix_time_us(),
                monotonic_us());
            current.latency_recorded = true;
        }
        av_frame_free(&temporary);
    };

    while (!stopping_.load()) {
        std::deque<QueuedFrame> pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(
                lock,
                std::chrono::milliseconds(10),
                [this] {
                    return stopping_.load() || !frames_.empty();
                });
            pending.swap(frames_);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                stopping_.store(true);
            }
        }

        while (!pending.empty()) {
            QueuedFrame queued = pending.front();
            pending.pop_front();
            AVFrame *processed = processor_.process(queued.frame);
            av_frame_free(&queued.frame);
            if (!processed) {
                continue;
            }

            clear_cached(previous);
            previous = current;
            current = {
                processed,
                queued.latency_start_unix_us,
                queued.latency_generation,
                false,
            };
            const bool can_blend =
                previous.frame &&
                previous.frame->width == current.frame->width &&
                previous.frame->height == current.frame->height;
            const auto action = cadence_.on_real_frame(
                queued.received_at_ms, can_blend);
            if (action != OutputAction::None) {
                present(action);
            }
        }

        const auto action = cadence_.on_tick(steady_time_ms());
        if (action != OutputAction::None) {
            present(action);
        }
    }

    clear_cached(previous);
    clear_cached(current);
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

uint64_t VideoRenderer::rendered_frames() const {
    return rendered_frames_.load(std::memory_order_relaxed);
}

RendererStats VideoRenderer::take_stats() {
    RendererStats stats;
    stats.decoded_frames =
        decoded_interval_frames_.exchange(0, std::memory_order_relaxed);
    stats.output_frames =
        output_interval_frames_.exchange(0, std::memory_order_relaxed);
    stats.synthetic_frames =
        synthetic_interval_frames_.exchange(0, std::memory_order_relaxed);
    stats.output_width =
        current_output_width_.load(std::memory_order_relaxed);
    stats.output_height =
        current_output_height_.load(std::memory_order_relaxed);
    return stats;
}

void VideoRenderer::discard_pending() {
    clear_queue();
}

void VideoRenderer::clear_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!frames_.empty()) {
        AVFrame *frame = frames_.front().frame;
        frames_.pop_front();
        av_frame_free(&frame);
    }
}
