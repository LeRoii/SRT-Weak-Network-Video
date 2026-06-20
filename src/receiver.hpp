#pragma once

#include "common/latency_stats.hpp"
#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "protocol/frame_assembler.hpp"
#include "transport/srt_transport.hpp"
#include "transport/udp_transport.hpp"
#include "video/h264_decoder.hpp"
#include "video/video_renderer.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>

class ReceiverApp {
public:
    ReceiverApp(Endpoint local,
                std::string output_file,
                bool display_enabled,
                int minimum_output_width,
                int minimum_output_height,
                int minimum_output_fps,
                UpscaleMode upscale_mode,
                InterpolationMode interpolation_mode,
                bool write_h264,
                LatencyConfig latency_config,
                TransportConfig transport_config);
    ~ReceiverApp();

    void run();

private:
    void run_srt();
    void run_udp();
    void run_srt_connection(SrtSocket &socket);
    void handle_frame(RecoveredFrame frame);
    void reset_media_session();
    uint32_t last_frame_age_ms() const;
    uint64_t maximum_frame_gap_ms() const;

    Endpoint local_;
    std::ofstream output_;
    LatencyConfig latency_config_;
    TransportConfig transport_config_;
    LatencyStats latency_stats_;
    VideoRenderer renderer_;
    H264Decoder decoder_;
    FrameAssembler assembler_;
    bool display_enabled_ = true;
    bool write_h264_ = true;
    bool synchronized_ = false;
    bool output_started_ = false;
    bool have_decoded_frame_id_ = false;
    uint32_t stream_epoch_ = 0;
    uint64_t last_decoded_frame_id_ = 0;
    uint64_t completed_frames_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t decoder_errors_ = 0;
    uint64_t latency_generation_ = 0;
    uint64_t active_session_id_ = 0;
    uint64_t active_session_started_unix_us_ = 0;
    std::chrono::steady_clock::time_point receiver_started_at_{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_complete_frame_at_{};
    uint64_t maximum_complete_frame_gap_ms_ = 0;
};
