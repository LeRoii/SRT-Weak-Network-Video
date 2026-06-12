#pragma once

#include "common/runtime_config.hpp"
#include "common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVFormatContext;
struct SwsContext;

class VideoSourceReader {
public:
    explicit VideoSourceReader(SenderConfig config);
    ~VideoSourceReader();

    VideoSourceReader(const VideoSourceReader &) = delete;
    VideoSourceReader &operator=(const VideoSourceReader &) = delete;

    bool next_frame(EncodedVideoFrame &frame,
                    const VideoProfile &profile,
                    bool force_keyframe);
    void reset();
    bool is_live() const;

private:
    struct CameraBuffer {
        void *data = nullptr;
        std::size_t length = 0;
    };

    void open();
    void open_file();
    void open_camera();
    void close();
    void close_camera();
    void close_encoder();
    void configure_encoder(const VideoProfile &profile);
    bool next_decoded_frame();
    bool next_file_frame();
    bool next_camera_frame();
    bool should_output_decoded_frame(int fps);

    SenderConfig config_;
    AVFormatContext *format_context_ = nullptr;
    AVCodecContext *decoder_context_ = nullptr;
    AVCodecContext *encoder_context_ = nullptr;
    AVFrame *decoded_frame_ = nullptr;
    AVFrame *scaled_frame_ = nullptr;
    SwsContext *sws_context_ = nullptr;
    int video_stream_index_ = -1;
    int stream_time_base_num_ = 0;
    int stream_time_base_den_ = 1;
    int camera_fd_ = -1;
    int camera_bytes_per_line_ = 0;
    std::vector<CameraBuffer> camera_buffers_;
    bool camera_streaming_ = false;
    bool input_eof_ = false;
    bool decoder_flushed_ = false;
    bool encoder_configured_ = false;
    VideoProfile encoder_profile_;
    uint16_t epoch_ = 0;
    int64_t encoder_pts_ = 0;
    double next_output_source_seconds_ = -1.0;
};
