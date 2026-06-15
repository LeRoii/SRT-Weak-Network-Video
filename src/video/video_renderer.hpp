#pragma once

#include "common/latency_stats.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct AVFrame;

class VideoRenderer {
public:
    VideoRenderer(int output_width,
                  int output_height,
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
    void discard_pending();

private:
    struct QueuedFrame {
        AVFrame *frame = nullptr;
        uint64_t latency_start_unix_us = 0;
        uint64_t latency_generation = 0;
    };

    void render_loop();
    void clear_queue();

    std::atomic<bool> stopping_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<uint64_t> rendered_frames_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    int output_width_ = 640;
    int output_height_ = 360;
    LatencyStats *latency_stats_ = nullptr;
    std::deque<QueuedFrame> frames_;
    std::thread thread_;
};
