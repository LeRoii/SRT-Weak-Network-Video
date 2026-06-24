#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

enum class MessageType : uint8_t {
    VideoShard = 1,
    FrameAck = 2,
    KeyframeRequest = 3,
    NetworkReport = 4,
    UdpFeedback = 5,
    UdpProbe = 6,
};

struct ShardPacket {
    uint64_t session_id = 0;
    uint64_t session_started_unix_us = 0;
    uint64_t packet_sequence = 0;
    uint64_t sent_monotonic_us = 0;
    uint32_t stream_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t encoded_at_unix_us = 0;
    uint32_t source_to_encoded_us = 0;
    uint32_t original_size = 0;
    uint32_t frame_crc = 0;
    uint32_t bitrate_kbps = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t block_index = 0;
    uint16_t block_count = 1;
    uint32_t block_original_size = 0;
    uint16_t data_shards = 0;
    uint16_t parity_shards = 0;
    uint16_t shard_index = 0;
    uint16_t shard_size = 0;
    bool keyframe = false;
    std::vector<uint8_t> payload;
};

struct ControlPacket {
    MessageType type = MessageType::FrameAck;
    uint32_t stream_epoch = 0;
    uint64_t frame_id = 0;
};

struct NetworkReport {
    uint64_t sequence = 0;
    uint32_t loss_basis_points = 0;
};

struct UdpFeedback {
    uint64_t session_id = 0;
    uint64_t sequence = 0;
    uint64_t highest_packet_sequence = 0;
    uint64_t unique_packets = 0;
    uint64_t received_bytes = 0;
    uint64_t last_complete_frame_id = 0;
    uint64_t echoed_sender_monotonic_us = 0;
    uint32_t last_frame_age_ms = 0;
    bool request_keyframe = false;
    bool rtt_probe_response = false;
};

struct UdpProbe {
    uint64_t session_id = 0;
    uint64_t session_started_unix_us = 0;
    uint64_t sequence = 0;
    uint64_t sender_monotonic_us = 0;
};

struct ParsedMessage {
    MessageType type = MessageType::VideoShard;
    std::optional<ShardPacket> shard;
    std::optional<ControlPacket> control;
    std::optional<NetworkReport> network_report;
    std::optional<UdpFeedback> udp_feedback;
    std::optional<UdpProbe> udp_probe;
};

std::vector<uint8_t> encode_shard_packet(const ShardPacket &packet);
std::vector<uint8_t> encode_control_packet(const ControlPacket &packet);
std::vector<uint8_t> encode_network_report(const NetworkReport &report);
std::vector<uint8_t> encode_udp_feedback(const UdpFeedback &feedback);
std::vector<uint8_t> encode_udp_probe(const UdpProbe &probe);
std::optional<ParsedMessage> parse_message(const uint8_t *data,
                                           std::size_t size);
