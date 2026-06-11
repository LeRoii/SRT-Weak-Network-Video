#pragma once

#include "common/latency_stats.hpp"
#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "protocol/frame_assembler.hpp"
#include "transport/srt_transport.hpp"
#include "video/h264_decoder.hpp"
#include "video/video_renderer.hpp"

#include <cstdint>
#include <fstream>
#include <string>

class ReceiverApp {
public:
    ReceiverApp(Endpoint local,
                std::string output_file,
                bool display_enabled,
                bool write_h264,
                LatencyConfig latency_config);
    ~ReceiverApp();

    void run();

private:
    void run_connection(SrtSocket &socket);
    void handle_frame(RecoveredFrame frame);

    Endpoint local_;
    std::ofstream output_;
    LatencyConfig latency_config_;
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
};
