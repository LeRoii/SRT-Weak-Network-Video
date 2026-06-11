#include "video/video_renderer.hpp"

#include "common/utils.hpp"

#include <SDL2/SDL.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr std::size_t kMaxQueuedFrames = 3;
}

VideoRenderer::VideoRenderer(LatencyStats *latency_stats)
    : latency_stats_(latency_stats) {}

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
    rendered_frames_.fetch_add(1, std::memory_order_relaxed);
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
            {copy, latency_start_unix_us, latency_generation});
    }
    cv_.notify_one();
}

void VideoRenderer::render_loop() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "sdl_init_failed=" << SDL_GetError() << std::endl;
        return;
    }

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SwsContext *sws = nullptr;
    AVFrame *yuv_frame = nullptr;
    int width = 0;
    int height = 0;
    AVPixelFormat source_format = AV_PIX_FMT_NONE;

    while (!stopping_.load()) {
        QueuedFrame queued;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50),
                         [this] { return stopping_.load() || !frames_.empty(); });
            if (!frames_.empty()) {
                queued = frames_.front();
                frames_.pop_front();
            }
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                stopping_.store(true);
            }
        }

        AVFrame *frame = queued.frame;
        if (!frame) {
            continue;
        }

        const auto frame_format = static_cast<AVPixelFormat>(frame->format);
        if (!window || width != frame->width || height != frame->height ||
            source_format != frame_format) {
            width = frame->width;
            height = frame->height;
            source_format = frame_format;

            if (!window) {
                window = SDL_CreateWindow("WebRTC Receiver", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, width, height,
                                          SDL_WINDOW_RESIZABLE);
                renderer = window ? SDL_CreateRenderer(
                                        window, -1, SDL_RENDERER_ACCELERATED)
                                  : nullptr;
                if (window && !renderer) {
                    renderer = SDL_CreateRenderer(
                        window, -1, SDL_RENDERER_SOFTWARE);
                }
            } else {
                SDL_SetWindowSize(window, width, height);
            }

            if (!window || !renderer) {
                std::cerr << "sdl_window_failed=" << SDL_GetError() << std::endl;
                av_frame_free(&frame);
                break;
            }

            if (texture) {
                SDL_DestroyTexture(texture);
            }
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING, width, height);

            sws_freeContext(sws);
            sws = sws_getContext(width, height, source_format, width, height,
                                 AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                                 nullptr);

            av_frame_free(&yuv_frame);
            yuv_frame = av_frame_alloc();
            yuv_frame->format = AV_PIX_FMT_YUV420P;
            yuv_frame->width = width;
            yuv_frame->height = height;
            av_frame_get_buffer(yuv_frame, 32);
        }

        av_frame_make_writable(yuv_frame);
        sws_scale(sws, frame->data, frame->linesize, 0, height,
                  yuv_frame->data, yuv_frame->linesize);

        SDL_UpdateYUVTexture(texture, nullptr,
                             yuv_frame->data[0], yuv_frame->linesize[0],
                             yuv_frame->data[1], yuv_frame->linesize[1],
                             yuv_frame->data[2], yuv_frame->linesize[2]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        if (latency_stats_ && queued.latency_start_unix_us != 0) {
            latency_stats_->record(
                queued.latency_generation,
                queued.latency_start_unix_us,
                unix_time_us(),
                monotonic_us());
        }
        av_frame_free(&frame);
    }

    av_frame_free(&yuv_frame);
    sws_freeContext(sws);
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
