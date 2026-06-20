#include "protocol/messages.hpp"

#include <array>
#include <cstddef>

namespace {

constexpr std::array<uint8_t, 4> kMagic{{'S', 'V', 'T', '1'}};
constexpr uint8_t kProtocolVersion = 3;
constexpr std::size_t kShardHeaderSize = 98;
constexpr std::size_t kControlSize = 22;
constexpr std::size_t kUdpFeedbackSize = 70;

void append_u16(std::vector<uint8_t> &output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value >> 8U));
    output.push_back(static_cast<uint8_t>(value));
}

void append_u32(std::vector<uint8_t> &output, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<uint8_t> &output, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool read_u16(const uint8_t *data, std::size_t size,
              std::size_t &offset, uint16_t &value) {
    if (offset + 2 > size) {
        return false;
    }
    value = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8U) |
        static_cast<uint16_t>(data[offset + 1]));
    offset += 2;
    return true;
}

bool read_u32(const uint8_t *data, std::size_t size,
              std::size_t &offset, uint32_t &value) {
    if (offset + 4 > size) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8U) | data[offset++];
    }
    return true;
}

bool read_u64(const uint8_t *data, std::size_t size,
              std::size_t &offset, uint64_t &value) {
    if (offset + 8 > size) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | data[offset++];
    }
    return true;
}

void append_prefix(std::vector<uint8_t> &output, MessageType type) {
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    output.push_back(kProtocolVersion);
    output.push_back(static_cast<uint8_t>(type));
}

bool parse_prefix(const uint8_t *data, std::size_t size,
                  MessageType &type, std::size_t &offset) {
    if (size < 6) {
        return false;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (data[i] != kMagic[i]) {
            return false;
        }
    }
    if (data[4] != kProtocolVersion) {
        return false;
    }
    type = static_cast<MessageType>(data[5]);
    offset = 6;
    return true;
}

} // namespace

std::vector<uint8_t> encode_shard_packet(const ShardPacket &packet) {
    std::vector<uint8_t> output;
    output.reserve(kShardHeaderSize + packet.payload.size());
    append_prefix(output, MessageType::VideoShard);
    append_u16(output, packet.keyframe ? 1U : 0U);
    append_u64(output, packet.session_id);
    append_u64(output, packet.session_started_unix_us);
    append_u64(output, packet.packet_sequence);
    append_u64(output, packet.sent_monotonic_us);
    append_u32(output, packet.stream_epoch);
    append_u64(output, packet.frame_id);
    append_u64(output, packet.encoded_at_unix_us);
    append_u32(output, packet.source_to_encoded_us);
    append_u32(output, packet.original_size);
    append_u32(output, packet.frame_crc);
    append_u32(output, packet.bitrate_kbps);
    append_u16(output, packet.width);
    append_u16(output, packet.height);
    append_u16(output, packet.block_index);
    append_u16(output, packet.block_count);
    append_u32(output, packet.block_original_size);
    append_u16(output, packet.data_shards);
    append_u16(output, packet.parity_shards);
    append_u16(output, packet.shard_index);
    append_u16(output, packet.shard_size);
    append_u16(output, static_cast<uint16_t>(packet.payload.size()));
    output.insert(output.end(), packet.payload.begin(), packet.payload.end());
    return output;
}

std::vector<uint8_t> encode_control_packet(const ControlPacket &packet) {
    std::vector<uint8_t> output;
    output.reserve(kControlSize);
    append_prefix(output, packet.type);
    append_u32(output, packet.stream_epoch);
    append_u64(output, packet.frame_id);
    append_u32(output, 0);
    return output;
}

std::vector<uint8_t> encode_network_report(const NetworkReport &report) {
    std::vector<uint8_t> output;
    output.reserve(kControlSize);
    append_prefix(output, MessageType::NetworkReport);
    append_u32(output, report.loss_basis_points);
    append_u64(output, report.sequence);
    append_u32(output, 0);
    return output;
}

std::vector<uint8_t> encode_udp_feedback(const UdpFeedback &feedback) {
    std::vector<uint8_t> output;
    output.reserve(kUdpFeedbackSize);
    append_prefix(output, MessageType::UdpFeedback);
    append_u64(output, feedback.session_id);
    append_u64(output, feedback.sequence);
    append_u64(output, feedback.highest_packet_sequence);
    append_u64(output, feedback.unique_packets);
    append_u64(output, feedback.received_bytes);
    append_u64(output, feedback.last_complete_frame_id);
    append_u64(output, feedback.echoed_sender_monotonic_us);
    append_u32(output, feedback.last_frame_age_ms);
    append_u32(output, feedback.request_keyframe ? 1U : 0U);
    return output;
}

std::optional<ParsedMessage> parse_message(const uint8_t *data,
                                           std::size_t size) {
    MessageType type{};
    std::size_t offset = 0;
    if (!parse_prefix(data, size, type, offset)) {
        return std::nullopt;
    }

    ParsedMessage parsed;
    parsed.type = type;
    if (type == MessageType::VideoShard) {
        uint16_t flags = 0;
        uint16_t payload_size = 0;
        ShardPacket packet;
        if (!read_u16(data, size, offset, flags) ||
            !read_u64(data, size, offset, packet.session_id) ||
            !read_u64(data, size, offset, packet.session_started_unix_us) ||
            !read_u64(data, size, offset, packet.packet_sequence) ||
            !read_u64(data, size, offset, packet.sent_monotonic_us) ||
            !read_u32(data, size, offset, packet.stream_epoch) ||
            !read_u64(data, size, offset, packet.frame_id) ||
            !read_u64(data, size, offset, packet.encoded_at_unix_us) ||
            !read_u32(data, size, offset, packet.source_to_encoded_us) ||
            !read_u32(data, size, offset, packet.original_size) ||
            !read_u32(data, size, offset, packet.frame_crc) ||
            !read_u32(data, size, offset, packet.bitrate_kbps) ||
            !read_u16(data, size, offset, packet.width) ||
            !read_u16(data, size, offset, packet.height) ||
            !read_u16(data, size, offset, packet.block_index) ||
            !read_u16(data, size, offset, packet.block_count) ||
            !read_u32(data, size, offset, packet.block_original_size) ||
            !read_u16(data, size, offset, packet.data_shards) ||
            !read_u16(data, size, offset, packet.parity_shards) ||
            !read_u16(data, size, offset, packet.shard_index) ||
            !read_u16(data, size, offset, packet.shard_size) ||
            !read_u16(data, size, offset, payload_size) ||
            offset + payload_size != size) {
            return std::nullopt;
        }
        packet.keyframe = (flags & 1U) != 0;
        packet.payload.assign(data + offset, data + size);
        parsed.shard = std::move(packet);
        return parsed;
    }

    if (type == MessageType::FrameAck ||
        type == MessageType::KeyframeRequest) {
        ControlPacket packet;
        packet.type = type;
        uint32_t reserved = 0;
        if (!read_u32(data, size, offset, packet.stream_epoch) ||
            !read_u64(data, size, offset, packet.frame_id) ||
            !read_u32(data, size, offset, reserved) ||
            offset != size) {
            return std::nullopt;
        }
        parsed.control = packet;
        return parsed;
    }

    if (type == MessageType::NetworkReport) {
        NetworkReport report;
        uint32_t reserved = 0;
        if (!read_u32(data, size, offset, report.loss_basis_points) ||
            !read_u64(data, size, offset, report.sequence) ||
            !read_u32(data, size, offset, reserved) ||
            offset != size ||
            report.loss_basis_points > 10'000) {
            return std::nullopt;
        }
        parsed.network_report = report;
        return parsed;
    }
    if (type == MessageType::UdpFeedback) {
        UdpFeedback feedback;
        uint32_t flags = 0;
        if (!read_u64(data, size, offset, feedback.session_id) ||
            !read_u64(data, size, offset, feedback.sequence) ||
            !read_u64(data, size, offset,
                      feedback.highest_packet_sequence) ||
            !read_u64(data, size, offset, feedback.unique_packets) ||
            !read_u64(data, size, offset, feedback.received_bytes) ||
            !read_u64(data, size, offset,
                      feedback.last_complete_frame_id) ||
            !read_u64(data, size, offset,
                      feedback.echoed_sender_monotonic_us) ||
            !read_u32(data, size, offset, feedback.last_frame_age_ms) ||
            !read_u32(data, size, offset, flags) ||
            offset != size) {
            return std::nullopt;
        }
        feedback.request_keyframe = (flags & 1U) != 0;
        parsed.udp_feedback = feedback;
        return parsed;
    }
    return std::nullopt;
}
