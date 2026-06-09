#pragma once

#include "fec/reed_solomon.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <map>
#include <optional>

struct RecoveredFrame {
    uint32_t stream_epoch = 0;
    uint64_t frame_id = 0;
    uint64_t pts_us = 0;
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
    struct PartialFrame {
        ShardPacket metadata;
        int64_t first_seen_us = 0;
        std::size_t received = 0;
        std::vector<std::optional<std::vector<uint8_t>>> shards;
    };

    ReedSolomon fec_;
    std::map<std::pair<uint32_t, uint64_t>, PartialFrame> frames_;
    std::map<std::pair<uint32_t, uint64_t>, int64_t> completed_;
};
