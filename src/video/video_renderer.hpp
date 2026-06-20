#pragma once

#include "common/latency_stats.hpp"
#include "common/runtime_config.hpp"
#include "video/output_processor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct AVFrame;

struct RendererStats {
    uint64_t decoded_frames = 0;
    uint64_t output_frames = 0;
    uint64_t synthetic_frames = 0;
    int output_width = 0;
    int output_height = 0;
};

class VideoRenderer {
public:
    VideoRenderer(int minimum_output_width,
                  int minimum_output_height,
                  int minimum_output_fps,
                  UpscaleMode upscale_mode,
                  InterpolationMode interpolation_mode,
                  LatencyStats *latency_stats = nullptr);
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer &) = delete;
    VideoRenderer &operator=(const VideoRenderer &) = delete;

    void start(bool enabled);
    void stop();
    void submit(const AVFrame *frame,
                uint64_t latency_start_unix_us,
                uint64_t latency_generation);
    uint64_t rendered_frames() const;
    RendererStats take_stats();
    void discard_pending();

private:
    struct QueuedFrame {
        AVFrame *frame = nullptr;
        uint64_t latency_start_unix_us = 0;
        uint64_t latency_generation = 0;
        int64_t received_at_ms = 0;
    };

    void render_loop();
    void clear_queue();

    std::atomic<bool> stopping_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<uint64_t> rendered_frames_{0};
    std::atomic<uint64_t> decoded_interval_frames_{0};
    std::atomic<uint64_t> output_interval_frames_{0};
    std::atomic<uint64_t> synthetic_interval_frames_{0};
    std::atomic<int> current_output_width_{0};
    std::atomic<int> current_output_height_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    OutputProcessor processor_;
    OutputCadenceController cadence_;
    InterpolationMode interpolation_mode_ =
        InterpolationMode::Blend;
    LatencyStats *latency_stats_ = nullptr;
    std::deque<QueuedFrame> frames_;
    std::thread thread_;
};
