#pragma once

#include "common/adaptation.hpp"
#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "fec/reed_solomon.hpp"
#include "transport/srt_transport.hpp"
#include "video/video_source_reader.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

class SenderApp {
public:
    SenderApp(Endpoint peer, SenderConfig config);
    void run();

private:
    bool run_connection(SrtSocket &socket, uint32_t connection_epoch);
    SendResult send_frame(SrtSocket &socket,
                          const EncodedVideoFrame &frame,
                          const VideoProfile &profile,
                          uint32_t stream_epoch,
                          uint64_t frame_id);
    bool receive_network_reports(SrtSocket &socket);

    Endpoint peer_;
    VideoSourceReader reader_;
    AdaptationController adaptation_;
    ReedSolomon fec_;
    std::atomic<bool> keyframe_requested_{true};
    std::optional<double> receiver_loss_percent_;
    uint64_t network_report_sequence_ = 0;
    std::chrono::steady_clock::time_point network_report_received_at_{};
};
