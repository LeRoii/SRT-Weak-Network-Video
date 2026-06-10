#include "sender.hpp"

#include "common/utils.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

SenderApp::SenderApp(Endpoint peer,
                     std::string video_file,
                     int max_video_kbps)
    : peer_(std::move(peer)),
      reader_(std::move(video_file)),
      adaptation_(max_video_kbps) {}

void SenderApp::run() {
    int reconnect_delay_ms = 100;
    uint32_t connection_epoch = 0;
    while (!g_stop_requested.load()) {
        try {
            std::cerr << "srt_connecting=" << peer_.host << ":"
                      << peer_.port << std::endl;
            auto socket = SrtSocket::connect_to(peer_);
            reconnect_delay_ms = 100;
            ++connection_epoch;
            if (connection_epoch == 0) {
                ++connection_epoch;
            }
            keyframe_requested_.store(true);
            std::cerr << "srt_connected=1 connection_epoch="
                      << connection_epoch << std::endl;
            run_connection(socket, connection_epoch);
        } catch (const std::exception &error) {
            std::cerr << "srt_connection_failed=" << error.what() << std::endl;
        }

        if (g_stop_requested.load()) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(reconnect_delay_ms));
        reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 1000);
    }
}

bool SenderApp::run_connection(SrtSocket &socket,
                               uint32_t connection_epoch) {
    uint64_t frame_id = 0;
    auto profile = adaptation_.current();
    auto next_stats = std::chrono::steady_clock::now() +
                      std::chrono::seconds(1);
    bool send_ok = true;
    receiver_loss_percent_.reset();
    network_report_sequence_ = 0;

    while (!g_stop_requested.load()) {
        if (!receive_network_reports(socket)) {
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
        const auto send_result =
            send_frame(socket, frame, profile, stream_epoch, frame_id++);
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
                      << "bandwidth=" << network.bandwidth_kbps << "kbps "
                      << "retransmit=" << network.retransmitted_packets << " "
                      << "profile=" << profile.bitrate_kbps << "kbps/"
                      << profile.fps << "fps/"
                      << profile.width << "x" << profile.height << " "
                      << "fec=" << profile.parity_ratio << std::endl;
            next_stats = now + std::chrono::seconds(1);
        }

        const auto frame_interval =
            std::chrono::microseconds(
                frame.duration_90khz * 1'000'000ULL / 90000ULL);
        std::this_thread::sleep_for(frame_interval);
    }

    std::cerr << "srt_connected=0" << std::endl;
    return send_ok;
}

bool SenderApp::receive_network_reports(SrtSocket &socket) {
    for (;;) {
        std::vector<uint8_t> message;
        const auto result = socket.receive(message);
        if (result == ReceiveResult::Timeout) {
            return true;
        }
        if (result == ReceiveResult::Closed) {
            return false;
        }

        const auto parsed = parse_message(message.data(), message.size());
        if (!parsed || !parsed->network_report ||
            parsed->network_report->sequence <= network_report_sequence_) {
            continue;
        }

        network_report_sequence_ = parsed->network_report->sequence;
        receiver_loss_percent_ =
            parsed->network_report->loss_basis_points / 100.0;
        network_report_received_at_ = std::chrono::steady_clock::now();
    }
}

SendResult SenderApp::send_frame(SrtSocket &socket,
                                 const EncodedVideoFrame &frame,
                                 const VideoProfile &profile,
                                 uint32_t stream_epoch,
                                 uint64_t frame_id) {
    FecBlock block;
    try {
        block = fec_.encode(frame.data, profile.parity_ratio);
    } catch (const std::exception &error) {
        std::cerr << "frame_fec_failed=" << error.what()
                  << " bytes=" << frame.data.size() << std::endl;
        keyframe_requested_.store(true);
        return SendResult::Sent;
    }

    const uint32_t crc = frame_crc32(frame.data.data(), frame.data.size());
    const uint64_t pts_us = static_cast<uint64_t>(monotonic_us());
    for (std::size_t index = 0; index < block.shards.size(); ++index) {
        ShardPacket packet;
        packet.stream_epoch = stream_epoch;
        packet.frame_id = frame_id;
        packet.pts_us = pts_us;
        packet.original_size = static_cast<uint32_t>(frame.data.size());
        packet.frame_crc = crc;
        packet.bitrate_kbps = static_cast<uint32_t>(profile.bitrate_kbps);
        packet.width = static_cast<uint16_t>(profile.width);
        packet.height = static_cast<uint16_t>(profile.height);
        packet.data_shards = block.data_shards;
        packet.parity_shards = block.parity_shards;
        packet.shard_index = static_cast<uint16_t>(index);
        packet.shard_size = block.shard_size;
        packet.keyframe = frame.keyframe;
        packet.payload = block.shards[index];

        const auto message = encode_shard_packet(packet);
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
            std::cerr << "srt_send_failed=" << srt_last_error() << std::endl;
            return result;
        }
        if (result == SendResult::WouldBlock) {
            return result;
        }
    }
    return SendResult::Sent;
}
