#pragma once

#include "video/video_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVFrame;

class H264Decoder {
public:
    explicit H264Decoder(VideoRenderer &renderer);
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool decode(const uint8_t *data,
                std::size_t size,
                uint64_t display_latency_start_us,
                uint64_t latency_generation,
                uint64_t *decoded_at_unix_us);
    void reset();

private:
    void open();
    void close();

    VideoRenderer &renderer_;
    AVCodecContext *context_ = nullptr;
    AVFrame *frame_ = nullptr;
};
