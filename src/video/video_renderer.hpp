#pragma once

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
    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer &) = delete;
    VideoRenderer &operator=(const VideoRenderer &) = delete;

    void start(bool enabled);
    void stop();
    void submit(const AVFrame *frame);
    uint64_t rendered_frames() const;

private:
    void render_loop();
    void clear_queue();

    std::atomic<bool> stopping_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<uint64_t> rendered_frames_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AVFrame *> frames_;
    std::thread thread_;
};
