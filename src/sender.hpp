#pragma once

#include "common/adaptation.hpp"
#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "fec/reed_solomon.hpp"
#include "transport/srt_transport.hpp"
#include "transport/udp_transport.hpp"
#include "video/video_source_reader.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class SenderApp {
public:
    SenderApp(Endpoint peer,
              SenderConfig config,
              TransportConfig transport_config);
    void run();

private:
    struct UdpFeedbackSnapshot {
        std::optional<double> loss_percent;
        double rtt_ms = 0.0;
        double bandwidth_kbps = 0.0;
        uint64_t sequence = 0;
        uint64_t last_complete_frame_id = 0;
        uint64_t complete_report_count = 0;
        uint32_t last_frame_age_ms = 0;
        bool request_keyframe = false;
        std::chrono::steady_clock::time_point received_at{};
    };

    void run_srt();
    void run_udp();
    bool run_srt_connection(SrtSocket &socket,
                            uint32_t connection_epoch,
                            uint64_t session_id,
                            uint64_t session_started_unix_us);
    SendResult send_frame_srt(SrtSocket &socket,
                              const EncodedVideoFrame &frame,
                              const VideoProfile &profile,
                              uint32_t stream_epoch,
                              uint64_t frame_id,
                              uint64_t session_id,
                              uint64_t session_started_unix_us,
                              uint64_t &packet_sequence);
    UdpResult send_frame_udp(UdpSocket &socket,
                             const EncodedVideoFrame &frame,
                             const VideoProfile &profile,
                             uint32_t stream_epoch,
                             uint64_t frame_id,
                             uint64_t session_id,
                             uint64_t session_started_unix_us,
                             uint64_t &packet_sequence);
    std::vector<std::vector<uint8_t>> encode_frame_packets(
        const EncodedVideoFrame &frame,
        const VideoProfile &profile,
        uint32_t stream_epoch,
        uint64_t frame_id,
        uint64_t session_id,
        uint64_t session_started_unix_us,
        uint64_t &packet_sequence);
    bool receive_srt_network_reports(SrtSocket &socket);
    void udp_feedback_loop(UdpSocket &socket,
                           uint64_t session_id,
                           uint64_t session_started_unix_us);
    UdpFeedbackSnapshot udp_feedback_snapshot();
    void reset_udp_feedback();

    Endpoint peer_;
    SenderConfig sender_config_;
    TransportConfig transport_config_;
    VideoSourceReader reader_;
    AdaptationController adaptation_;
    ReedSolomon fec_;
    std::atomic<bool> keyframe_requested_{true};
    std::optional<double> receiver_loss_percent_;
    uint64_t network_report_sequence_ = 0;
    std::chrono::steady_clock::time_point network_report_received_at_{};

    std::mutex udp_feedback_mutex_;
    UdpFeedbackSnapshot udp_feedback_;
    uint64_t udp_loss_baseline_highest_ = 0;
    uint64_t udp_loss_baseline_unique_ = 0;
    uint64_t udp_bandwidth_baseline_bytes_ = 0;
    std::chrono::steady_clock::time_point udp_bandwidth_baseline_at_{};
    bool udp_feedback_baseline_valid_ = false;
    std::atomic<bool> udp_feedback_stopping_{false};
};
