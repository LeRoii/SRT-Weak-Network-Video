#pragma once

#include "common/adaptation.hpp"
#include "common/types.hpp"
#include "fec/reed_solomon.hpp"
#include "transport/srt_transport.hpp"
#include "video/video_file_reader.hpp"

#include <atomic>
#include <cstdint>
#include <string>

class SenderApp {
public:
    SenderApp(Endpoint peer, std::string video_file, int max_video_kbps);
    void run();

private:
    bool run_connection(SrtSocket &socket, uint32_t connection_epoch);
    SendResult send_frame(SrtSocket &socket,
                          const EncodedVideoFrame &frame,
                          const VideoProfile &profile,
                          uint32_t stream_epoch,
                          uint64_t frame_id);

    Endpoint peer_;
    VideoFileReader reader_;
    AdaptationController adaptation_;
    ReedSolomon fec_;
    std::atomic<bool> keyframe_requested_{true};
};
