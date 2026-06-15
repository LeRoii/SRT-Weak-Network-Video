#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class Role {
    Sender,
    Receiver,
};

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

struct VideoProfile {
    int level = 0;
    int bitrate_kbps = 2000;
    int fps = 30;
    int width = 1280;
    int height = 720;
    double parity_ratio = 0.15;
    double keyframe_parity_ratio = 0.30;
    bool all_intra = false;

    bool operator==(const VideoProfile &other) const {
        return level == other.level &&
               bitrate_kbps == other.bitrate_kbps &&
               fps == other.fps &&
               width == other.width &&
               height == other.height &&
               parity_ratio == other.parity_ratio &&
               keyframe_parity_ratio == other.keyframe_parity_ratio &&
               all_intra == other.all_intra;
    }

    bool operator!=(const VideoProfile &other) const {
        return !(*this == other);
    }
};

struct EncodedVideoFrame {
    std::vector<uint8_t> data;
    uint64_t encoded_at_unix_us = 0;
    uint32_t source_to_encoded_us = 0;
    uint32_t duration_90khz = 3000;
    uint16_t encoder_epoch = 1;
    bool keyframe = false;
};

struct NetworkSnapshot {
    double loss_percent = 0.0;
    double rtt_ms = 0.0;
    double bandwidth_kbps = 0.0;
    uint64_t sent_packets = 0;
    uint64_t lost_packets = 0;
    uint64_t retransmitted_packets = 0;
    bool valid = false;
};
