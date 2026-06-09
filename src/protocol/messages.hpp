#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

enum class MessageType : uint8_t {
    VideoShard = 1,
    FrameAck = 2,
    KeyframeRequest = 3,
};

struct ShardPacket {
    uint32_t stream_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t pts_us = 0;
    uint32_t original_size = 0;
    uint32_t frame_crc = 0;
    uint32_t bitrate_kbps = 0;
    uint16_t width = 0;
    uint16_t height = 0;
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

struct ParsedMessage {
    MessageType type = MessageType::VideoShard;
    std::optional<ShardPacket> shard;
    std::optional<ControlPacket> control;
};

std::vector<uint8_t> encode_shard_packet(const ShardPacket &packet);
std::vector<uint8_t> encode_control_packet(const ControlPacket &packet);
std::optional<ParsedMessage> parse_message(const uint8_t *data,
                                           std::size_t size);
