#include "sender.hpp"

#include "common/utils.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

namespace {

uint64_t random_session_id() {
    std::random_device device;
    const uint64_t high = static_cast<uint64_t>(device()) << 32U;
    const uint64_t low = static_cast<uint64_t>(device());
    const uint64_t result = high | low;
    return result == 0 ? 1 : result;
}

} // namespace

SenderApp::SenderApp(Endpoint peer,
                     SenderConfig config,
                     TransportConfig transport_config)
    : peer_(std::move(peer)),
      sender_config_(config),
      transport_config_(transport_config),
      reader_(std::move(config)),
      adaptation_(sender_config_.max_video_kbps) {}

void SenderApp::run() {
    if (transport_config_.mode == TransportMode::Udp) {
        run_udp();
    } else {
        run_srt();
    }
}

void SenderApp::run_srt() {
    int reconnect_delay_ms = 100;
    uint32_t connection_epoch = 0;
    while (!g_stop_requested.load()) {
        try {
            std::cerr << "srt_connecting="
                      << peer_.host << ":" << peer_.port << std::endl;
            auto socket = SrtSocket::connect_to(peer_);
            reconnect_delay_ms = 100;
            ++connection_epoch;
            if (connection_epoch == 0) {
                ++connection_epoch;
            }
            keyframe_requested_.store(true);
            const uint64_t session_id = random_session_id();
            const uint64_t session_started_unix_us = unix_time_us();
            std::cerr << "srt_connected=1 connection_epoch="
                      << connection_epoch << std::endl;
            run_srt_connection(
                socket, connection_epoch, session_id,
                session_started_unix_us);
        } catch (const std::exception &error) {
            std::cerr << "srt_connection_failed="
                      << error.what() << std::endl;
        }

        if (g_stop_requested.load()) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(reconnect_delay_ms));
        reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 1000);
    }
}

void SenderApp::run_udp() {
    auto socket = UdpSocket::create_sender(peer_);
    const uint64_t session_id = random_session_id();
    const uint64_t session_started_unix_us = unix_time_us();
    uint64_t packet_sequence = 0;
    uint64_t frame_id = 1;
    uint32_t stream_epoch = 1;
    bool recovering = false;
    uint64_t last_recovery_frame_id = 0;
    uint64_t recovery_report_baseline = 0;
    uint64_t last_adaptation_feedback_sequence = 0;
    auto profile = adaptation_.current();
    const auto udp_started_at = std::chrono::steady_clock::now();
    auto next_stats =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);

    reset_udp_feedback();
    udp_feedback_stopping_.store(false);
    std::thread feedback_thread(
        [this, &socket, session_id] {
            udp_feedback_loop(socket, session_id);
        });

    std::cerr << "udp_ready=1 peer="
              << peer_.host << ":" << peer_.port
              << " session_id=" << session_id << std::endl;

    try {
        while (!g_stop_requested.load()) {
            const auto now = std::chrono::steady_clock::now();
            const auto feedback = udp_feedback_snapshot();
            const bool have_feedback =
                feedback.received_at.time_since_epoch().count() != 0;
            const bool feedback_stale =
                have_feedback
                    ? now - feedback.received_at >=
                          std::chrono::milliseconds(
                              transport_config_.feedback_timeout_ms)
                    : now - udp_started_at >=
                          std::chrono::milliseconds(
                              transport_config_.feedback_timeout_ms);
            const bool receiver_stalled =
                feedback.request_keyframe ||
                feedback.last_frame_age_ms > 1500;

            const bool recovery_required =
                feedback_stale || receiver_stalled ||
                (feedback.loss_percent &&
                 *feedback.loss_percent > 85.0);
            if (recovery_required && !recovering) {
                recovering = true;
                last_recovery_frame_id = 0;
                recovery_report_baseline =
                    feedback.complete_report_count;
                adaptation_.force_emergency();
            }

            VideoProfile desired = adaptation_.current();
            if (recovering && last_recovery_frame_id != 0 &&
                !feedback_stale &&
                !receiver_stalled &&
                feedback.last_complete_frame_id >=
                    last_recovery_frame_id &&
                feedback.complete_report_count >=
                    recovery_report_baseline + 2 &&
                feedback.loss_percent &&
                *feedback.loss_percent <= 85.0) {
                recovering = false;
                last_recovery_frame_id = 0;
                NetworkSnapshot network;
                network.loss_percent = *feedback.loss_percent;
                network.rtt_ms = feedback.rtt_ms;
                network.bandwidth_kbps =
                    feedback.bandwidth_kbps;
                network.valid = true;
                desired = adaptation_.reset_to_network(network);
                last_adaptation_feedback_sequence =
                    feedback.sequence;
            }

            if (!recovering &&
                feedback.loss_percent &&
                feedback.sequence >
                    last_adaptation_feedback_sequence) {
                NetworkSnapshot network;
                network.loss_percent = *feedback.loss_percent;
                network.rtt_ms = feedback.rtt_ms;
                network.bandwidth_kbps =
                    feedback.bandwidth_kbps;
                network.valid = true;
                desired = adaptation_.update(network);
                last_adaptation_feedback_sequence =
                    feedback.sequence;
            }

            if (desired != profile) {
                profile = desired;
                keyframe_requested_.store(true);
            }

            const bool force_keyframe =
                keyframe_requested_.exchange(false) ||
                profile.all_intra;
            EncodedVideoFrame frame;
            if (!reader_.next_frame(frame, profile, force_keyframe)) {
                reader_.reset();
                keyframe_requested_.store(true);
                continue;
            }

            const uint32_t frame_stream_epoch =
                (stream_epoch << 16U) | frame.encoder_epoch;
            const uint64_t current_frame_id = frame_id++;
            const auto result = send_frame_udp(
                socket, frame, profile, frame_stream_epoch,
                current_frame_id, session_id,
                session_started_unix_us, packet_sequence);
            if (result != UdpResult::Data) {
                if (!recovering) {
                    adaptation_.force_emergency();
                    profile = adaptation_.current();
                }
                recovering = true;
                last_recovery_frame_id = 0;
                recovery_report_baseline =
                    feedback.complete_report_count;
                keyframe_requested_.store(true);
                std::cerr << "udp_send_congested=1"
                          << std::endl;
            } else if (recovering && last_recovery_frame_id == 0) {
                last_recovery_frame_id = current_frame_id;
                recovery_report_baseline =
                    feedback.complete_report_count;
            }

            const auto stats_now = std::chrono::steady_clock::now();
            if (stats_now >= next_stats) {
                const auto stats = udp_feedback_snapshot();
                const double loss =
                    stats.loss_percent.value_or(100.0);
                std::cout
                    << "loss=" << loss << "% "
                    << "rtt=" << stats.rtt_ms << "ms "
                    << "bandwidth=" << stats.bandwidth_kbps << "kbps "
                    << "retransmit=0 "
                    << "profile=" << profile.bitrate_kbps << "kbps/"
                    << profile.fps << "fps/"
                    << profile.width << "x" << profile.height << " "
                    << "fec=" << profile.parity_ratio << " "
                    << "keyframe_fec="
                    << profile.keyframe_parity_ratio << " "
                    << "recovery=" << (recovering ? 1 : 0)
                    << std::endl;
                next_stats = stats_now + std::chrono::seconds(1);
            }

            if (!reader_.is_live()) {
                const auto frame_interval =
                    std::chrono::microseconds(
                        frame.duration_90khz * 1'000'000ULL /
                        90000ULL);
                std::this_thread::sleep_for(frame_interval);
            }
        }
    } catch (...) {
        udp_feedback_stopping_.store(true);
        feedback_thread.join();
        throw;
    }

    udp_feedback_stopping_.store(true);
    feedback_thread.join();
}

bool SenderApp::run_srt_connection(
    SrtSocket &socket,
    uint32_t connection_epoch,
    uint64_t session_id,
    uint64_t session_started_unix_us) {
    uint64_t frame_id = 1;
    uint64_t packet_sequence = 0;
    auto profile = adaptation_.current();
    auto next_stats = std::chrono::steady_clock::now() +
                      std::chrono::seconds(1);
    bool send_ok = true;
    network_report_sequence_ = 0;
    if (receiver_loss_percent_) {
        network_report_received_at_ = std::chrono::steady_clock::now();
    }

    while (!g_stop_requested.load()) {
        if (!receive_srt_network_reports(socket)) {
            send_ok = false;
            break;
        }

        const bool force_keyframe =
            keyframe_requested_.exchange(false) || profile.all_intra;
        EncodedVideoFrame frame;
        if (!reader_.next_frame(frame, profile, force_keyframe)) {
            reader_.reset();
            keyframe_requested_.store(true);
            continue;
        }

        const uint32_t stream_epoch =
            (connection_epoch << 16U) | frame.encoder_epoch;
        const auto send_result = send_frame_srt(
            socket, frame, profile, stream_epoch, frame_id++,
            session_id, session_started_unix_us, packet_sequence);
        if (send_result == SendResult::Closed) {
            send_ok = false;
            break;
        }
        if (send_result == SendResult::WouldBlock) {
            NetworkSnapshot congestion;
            congestion.loss_percent = 100.0;
            congestion.rtt_ms = 500.0;
            congestion.valid = true;
            profile = adaptation_.update(congestion);
            keyframe_requested_.store(true);
            std::cerr << "srt_send_congested=1 profile="
                      << profile.bitrate_kbps << "kbps/"
                      << profile.fps << "fps/"
                      << profile.width << "x" << profile.height
                      << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_stats) {
            auto network = socket.network_snapshot(true);
            if (receiver_loss_percent_ &&
                now - network_report_received_at_ <
                    std::chrono::seconds(12)) {
                network.loss_percent = *receiver_loss_percent_;
            }
            auto next_profile = adaptation_.update(network);
            if (next_profile != profile) {
                profile = next_profile;
                keyframe_requested_.store(true);
            }
            std::cout << "loss=" << network.loss_percent << "% "
                      << "rtt=" << network.rtt_ms << "ms "
                      << "bandwidth=" << network.bandwidth_kbps
                      << "kbps "
                      << "retransmit="
                      << network.retransmitted_packets << " "
                      << "profile=" << profile.bitrate_kbps << "kbps/"
                      << profile.fps << "fps/"
                      << profile.width << "x" << profile.height << " "
                      << "fec=" << profile.parity_ratio << " "
                      << "keyframe_fec="
                      << profile.keyframe_parity_ratio << std::endl;
            next_stats = now + std::chrono::seconds(1);
        }

        if (!reader_.is_live()) {
            const auto frame_interval =
                std::chrono::microseconds(
                    frame.duration_90khz * 1'000'000ULL / 90000ULL);
            std::this_thread::sleep_for(frame_interval);
        }
    }

    std::cerr << "srt_connected=0" << std::endl;
    return send_ok;
}

std::vector<std::vector<uint8_t>> SenderApp::encode_frame_packets(
    const EncodedVideoFrame &frame,
    const VideoProfile &profile,
    uint32_t stream_epoch,
    uint64_t frame_id,
    uint64_t session_id,
    uint64_t session_started_unix_us,
    uint64_t &packet_sequence) {
    const double parity_ratio =
        frame.keyframe
            ? profile.keyframe_parity_ratio
            : profile.parity_ratio;
    const auto blocks =
        fec_.encode_blocks(frame.data, parity_ratio, 4);
    if (blocks.size() > 64) {
        throw std::runtime_error("encoded frame has too many FEC blocks");
    }

    const uint32_t crc =
        frame_crc32(frame.data.data(), frame.data.size());
    std::vector<std::vector<uint8_t>> messages;
    for (std::size_t block_index = 0;
         block_index < blocks.size();
         ++block_index) {
        const auto &block = blocks[block_index];
        for (std::size_t shard_index = 0;
             shard_index < block.shards.size();
             ++shard_index) {
            ShardPacket packet;
            packet.session_id = session_id;
            packet.session_started_unix_us =
                session_started_unix_us;
            packet.packet_sequence = ++packet_sequence;
            packet.sent_monotonic_us =
                static_cast<uint64_t>(monotonic_us());
            packet.stream_epoch = stream_epoch;
            packet.frame_id = frame_id;
            packet.encoded_at_unix_us = frame.encoded_at_unix_us;
            packet.source_to_encoded_us = frame.source_to_encoded_us;
            packet.original_size =
                static_cast<uint32_t>(frame.data.size());
            packet.frame_crc = crc;
            packet.bitrate_kbps =
                static_cast<uint32_t>(profile.bitrate_kbps);
            packet.width = static_cast<uint16_t>(profile.width);
            packet.height = static_cast<uint16_t>(profile.height);
            packet.block_index =
                static_cast<uint16_t>(block_index);
            packet.block_count =
                static_cast<uint16_t>(blocks.size());
            packet.block_original_size = block.original_size;
            packet.data_shards = block.data_shards;
            packet.parity_shards = block.parity_shards;
            packet.shard_index =
                static_cast<uint16_t>(shard_index);
            packet.shard_size = block.shard_size;
            packet.keyframe = frame.keyframe;
            packet.payload = block.shards[shard_index];
            messages.push_back(encode_shard_packet(packet));
        }
    }
    return messages;
}

SendResult SenderApp::send_frame_srt(
    SrtSocket &socket,
    const EncodedVideoFrame &frame,
    const VideoProfile &profile,
    uint32_t stream_epoch,
    uint64_t frame_id,
    uint64_t session_id,
    uint64_t session_started_unix_us,
    uint64_t &packet_sequence) {
    std::vector<std::vector<uint8_t>> messages;
    try {
        messages = encode_frame_packets(
            frame, profile, stream_epoch, frame_id, session_id,
            session_started_unix_us, packet_sequence);
    } catch (const std::exception &error) {
        std::cerr << "frame_fec_failed=" << error.what()
                  << " bytes=" << frame.data.size() << std::endl;
        keyframe_requested_.store(true);
        return SendResult::Sent;
    }

    for (const auto &message : messages) {
        SendResult result = SendResult::WouldBlock;
        for (int attempt = 0;
             attempt < 20 && result == SendResult::WouldBlock;
             ++attempt) {
            result = socket.send(message);
            if (result == SendResult::WouldBlock) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(2));
            }
        }
        if (result == SendResult::Closed) {
            std::cerr << "srt_send_failed=" << srt_last_error()
                      << " state=" << socket.state_name() << std::endl;
            return result;
        }
        if (result == SendResult::WouldBlock) {
            return result;
        }
    }
    return SendResult::Sent;
}

UdpResult SenderApp::send_frame_udp(
    UdpSocket &socket,
    const EncodedVideoFrame &frame,
    const VideoProfile &profile,
    uint32_t stream_epoch,
    uint64_t frame_id,
    uint64_t session_id,
    uint64_t session_started_unix_us,
    uint64_t &packet_sequence) {
    std::vector<std::vector<uint8_t>> messages;
    try {
        messages = encode_frame_packets(
            frame, profile, stream_epoch, frame_id, session_id,
            session_started_unix_us, packet_sequence);
    } catch (const std::exception &error) {
        std::cerr << "frame_fec_failed=" << error.what()
                  << " bytes=" << frame.data.size() << std::endl;
        keyframe_requested_.store(true);
        return UdpResult::WouldBlock;
    }

    for (const auto &message : messages) {
        const auto result = socket.send(message);
        if (result != UdpResult::Data) {
            return result;
        }
    }
    return UdpResult::Data;
}

bool SenderApp::receive_srt_network_reports(SrtSocket &socket) {
    for (;;) {
        std::vector<uint8_t> message;
        const auto result = socket.receive(message);
        if (result == ReceiveResult::Timeout) {
            return true;
        }
        if (result == ReceiveResult::Closed) {
            std::cerr << "srt_receive_failed=" << srt_last_error()
                      << " state=" << socket.state_name() << std::endl;
            return false;
        }

        const auto parsed = parse_message(message.data(), message.size());
        if (!parsed || !parsed->network_report ||
            parsed->network_report->sequence <=
                network_report_sequence_) {
            continue;
        }

        network_report_sequence_ =
            parsed->network_report->sequence;
        receiver_loss_percent_ =
            parsed->network_report->loss_basis_points / 100.0;
        network_report_received_at_ =
            std::chrono::steady_clock::now();
    }
}

void SenderApp::udp_feedback_loop(UdpSocket &socket,
                                  uint64_t session_id) {
    while (!udp_feedback_stopping_.load() &&
           !g_stop_requested.load()) {
        std::vector<uint8_t> message;
        const auto result = socket.receive(message);
        if (result == UdpResult::WouldBlock) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
            continue;
        }
        if (result == UdpResult::Closed) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
            continue;
        }

        const auto parsed = parse_message(
            message.data(), message.size());
        if (!parsed || !parsed->udp_feedback ||
            parsed->udp_feedback->session_id != session_id) {
            continue;
        }
        const auto &report = *parsed->udp_feedback;
        const auto now = std::chrono::steady_clock::now();
        const uint64_t now_us =
            static_cast<uint64_t>(monotonic_us());

        std::lock_guard<std::mutex> lock(udp_feedback_mutex_);
        if (report.sequence < udp_feedback_.sequence) {
            continue;
        }
        udp_feedback_.received_at = now;
        if (report.sequence == udp_feedback_.sequence) {
            continue;
        }

        if (!udp_feedback_baseline_valid_ ||
            report.highest_packet_sequence <
                udp_loss_baseline_highest_ ||
            report.unique_packets < udp_loss_baseline_unique_) {
            udp_loss_baseline_highest_ =
                report.highest_packet_sequence;
            udp_loss_baseline_unique_ = report.unique_packets;
            udp_bandwidth_baseline_bytes_ =
                report.received_bytes;
            udp_bandwidth_baseline_at_ = now;
            udp_feedback_baseline_valid_ = true;
        } else {
            const uint64_t sent =
                report.highest_packet_sequence -
                udp_loss_baseline_highest_;
            if (sent >= 32) {
                const uint64_t received = std::min(
                    sent,
                    report.unique_packets -
                        udp_loss_baseline_unique_);
                const double sample_loss =
                    static_cast<double>(sent - received) *
                    100.0 / static_cast<double>(sent);
                if (udp_feedback_.loss_percent) {
                    udp_feedback_.loss_percent =
                        *udp_feedback_.loss_percent * 0.8 +
                        sample_loss * 0.2;
                } else {
                    udp_feedback_.loss_percent = sample_loss;
                }
                udp_loss_baseline_highest_ =
                    report.highest_packet_sequence;
                udp_loss_baseline_unique_ =
                    report.unique_packets;
            }

            const double elapsed_seconds =
                std::chrono::duration<double>(
                    now - udp_bandwidth_baseline_at_).count();
            if (elapsed_seconds >= 0.2 &&
                report.received_bytes >=
                    udp_bandwidth_baseline_bytes_ &&
                report.received_bytes >
                    udp_bandwidth_baseline_bytes_) {
                udp_feedback_.bandwidth_kbps =
                    static_cast<double>(
                        report.received_bytes -
                        udp_bandwidth_baseline_bytes_) *
                    8.0 / 1000.0 / elapsed_seconds;
                udp_bandwidth_baseline_bytes_ =
                    report.received_bytes;
                udp_bandwidth_baseline_at_ = now;
            }
        }

        if (report.highest_packet_sequence >
                udp_rtt_highest_sequence_ &&
            report.echoed_sender_monotonic_us != 0 &&
            now_us >= report.echoed_sender_monotonic_us) {
            udp_feedback_.rtt_ms =
                static_cast<double>(
                    now_us -
                    report.echoed_sender_monotonic_us) /
                1000.0;
            udp_rtt_highest_sequence_ =
                report.highest_packet_sequence;
        }
        udp_feedback_.sequence = report.sequence;
        udp_feedback_.last_complete_frame_id =
            report.last_complete_frame_id;
        if (report.last_complete_frame_id != 0) {
            ++udp_feedback_.complete_report_count;
        }
        udp_feedback_.last_frame_age_ms =
            report.last_frame_age_ms;
        udp_feedback_.request_keyframe =
            report.request_keyframe;
    }
}

SenderApp::UdpFeedbackSnapshot
SenderApp::udp_feedback_snapshot() {
    std::lock_guard<std::mutex> lock(udp_feedback_mutex_);
    return udp_feedback_;
}

void SenderApp::reset_udp_feedback() {
    std::lock_guard<std::mutex> lock(udp_feedback_mutex_);
    udp_feedback_ = {};
    udp_loss_baseline_highest_ = 0;
    udp_loss_baseline_unique_ = 0;
    udp_bandwidth_baseline_bytes_ = 0;
    udp_rtt_highest_sequence_ = 0;
    udp_bandwidth_baseline_at_ = {};
    udp_feedback_baseline_valid_ = false;
}
