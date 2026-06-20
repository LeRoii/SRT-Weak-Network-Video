#pragma once

#include "fec/reed_solomon.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

struct RecoveredFrame {
    uint64_t session_id = 0;
    uint64_t session_started_unix_us = 0;
    uint32_t stream_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t encoded_at_unix_us = 0;
    uint32_t source_to_encoded_us = 0;
    uint32_t bitrate_kbps = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

struct AssemblerCleanup {
    uint64_t expired_frames = 0;
    bool keyframe_lost = false;
};

class FrameAssembler {
public:
    std::optional<RecoveredFrame> push(ShardPacket packet);
    AssemblerCleanup expire(int64_t now_us, int64_t timeout_us);
    void reset();

private:
    struct PartialBlock {
        ShardPacket metadata;
        std::size_t received = 0;
        std::vector<std::optional<std::vector<uint8_t>>> shards;
        std::optional<std::vector<uint8_t>> recovered;
    };

    struct PartialFrame {
        ShardPacket metadata;
        int64_t first_seen_us = 0;
        std::size_t recovered_blocks = 0;
        std::vector<PartialBlock> blocks;
    };

    using FrameKey = std::tuple<uint64_t, uint32_t, uint64_t>;

    ReedSolomon fec_;
    std::map<FrameKey, PartialFrame> frames_;
    std::map<FrameKey, int64_t> completed_;
};
