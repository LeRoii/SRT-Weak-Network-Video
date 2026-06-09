#include "protocol/frame_assembler.hpp"

#include "common/utils.hpp"

#include <algorithm>

std::optional<RecoveredFrame> FrameAssembler::push(ShardPacket packet) {
    const int total_shards = packet.data_shards + packet.parity_shards;
    if (packet.data_shards == 0 || packet.parity_shards == 0 ||
        total_shards > ReedSolomon::kMaxShards ||
        packet.shard_index >= total_shards ||
        packet.payload.size() != packet.shard_size ||
        packet.original_size == 0) {
        return std::nullopt;
    }

    const auto key = std::make_pair(packet.stream_epoch, packet.frame_id);
    if (completed_.count(key) != 0) {
        return std::nullopt;
    }
    auto [it, inserted] = frames_.try_emplace(key);
    auto &frame = it->second;
    if (inserted) {
        frame.metadata = packet;
        frame.first_seen_us = monotonic_us();
        frame.shards.resize(static_cast<std::size_t>(total_shards));
    } else if (frame.metadata.data_shards != packet.data_shards ||
               frame.metadata.parity_shards != packet.parity_shards ||
               frame.metadata.shard_size != packet.shard_size ||
               frame.metadata.original_size != packet.original_size ||
               frame.metadata.frame_crc != packet.frame_crc) {
        frames_.erase(it);
        return std::nullopt;
    }

    auto &slot = frame.shards[packet.shard_index];
    if (!slot) {
        slot = std::move(packet.payload);
        ++frame.received;
    }
    if (frame.received < frame.metadata.data_shards) {
        return std::nullopt;
    }

    auto data = fec_.decode(
        frame.metadata.data_shards,
        frame.metadata.parity_shards,
        frame.metadata.shard_size,
        frame.metadata.original_size,
        frame.shards);
    if (!data ||
        frame_crc32(data->data(), data->size()) != frame.metadata.frame_crc) {
        return std::nullopt;
    }

    RecoveredFrame result;
    result.stream_epoch = frame.metadata.stream_epoch;
    result.frame_id = frame.metadata.frame_id;
    result.pts_us = frame.metadata.pts_us;
    result.bitrate_kbps = frame.metadata.bitrate_kbps;
    result.width = frame.metadata.width;
    result.height = frame.metadata.height;
    result.keyframe = frame.metadata.keyframe;
    result.data = std::move(*data);
    frames_.erase(key);
    completed_[key] = monotonic_us();

    while (frames_.size() > 64) {
        frames_.erase(frames_.begin());
    }
    return result;
}

AssemblerCleanup FrameAssembler::expire(int64_t now_us, int64_t timeout_us) {
    AssemblerCleanup result;
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (now_us - it->second.first_seen_us < timeout_us) {
            ++it;
            continue;
        }
        ++result.expired_frames;
        result.keyframe_lost =
            result.keyframe_lost || it->second.metadata.keyframe;
        it = frames_.erase(it);
    }
    for (auto it = completed_.begin(); it != completed_.end();) {
        if (now_us - it->second > 2'000'000) {
            it = completed_.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}

void FrameAssembler::reset() {
    frames_.clear();
    completed_.clear();
}
