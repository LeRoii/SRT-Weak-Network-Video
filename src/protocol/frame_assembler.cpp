#include "protocol/frame_assembler.hpp"

#include "common/utils.hpp"

#include <algorithm>

namespace {
constexpr uint16_t kMaximumBlocksPerFrame = 64;

bool same_frame_metadata(const ShardPacket &left,
                         const ShardPacket &right) {
    return left.session_id == right.session_id &&
           left.session_started_unix_us == right.session_started_unix_us &&
           left.stream_epoch == right.stream_epoch &&
           left.frame_id == right.frame_id &&
           left.encoded_at_unix_us == right.encoded_at_unix_us &&
           left.source_to_encoded_us == right.source_to_encoded_us &&
           left.original_size == right.original_size &&
           left.frame_crc == right.frame_crc &&
           left.bitrate_kbps == right.bitrate_kbps &&
           left.width == right.width &&
           left.height == right.height &&
           left.block_count == right.block_count &&
           left.keyframe == right.keyframe;
}

bool same_block_metadata(const ShardPacket &left,
                         const ShardPacket &right) {
    return left.block_index == right.block_index &&
           left.block_original_size == right.block_original_size &&
           left.data_shards == right.data_shards &&
           left.parity_shards == right.parity_shards &&
           left.shard_size == right.shard_size;
}
} // namespace

std::optional<RecoveredFrame> FrameAssembler::push(ShardPacket packet) {
    const int total_shards = packet.data_shards + packet.parity_shards;
    if (packet.data_shards == 0 || packet.parity_shards == 0 ||
        total_shards > ReedSolomon::kMaxShards ||
        packet.shard_index >= total_shards ||
        packet.payload.size() != packet.shard_size ||
        packet.original_size == 0 ||
        packet.block_original_size == 0 ||
        packet.block_original_size >
            static_cast<uint32_t>(packet.data_shards) * packet.shard_size ||
        packet.block_count == 0 ||
        packet.block_count > kMaximumBlocksPerFrame ||
        packet.block_index >= packet.block_count) {
        return std::nullopt;
    }

    const FrameKey key{
        packet.session_id, packet.stream_epoch, packet.frame_id};
    if (completed_.count(key) != 0) {
        return std::nullopt;
    }
    auto [it, inserted] = frames_.try_emplace(key);
    auto &frame = it->second;
    if (inserted) {
        frame.metadata = packet;
        frame.first_seen_us = monotonic_us();
        frame.blocks.resize(packet.block_count);
    } else if (!same_frame_metadata(frame.metadata, packet)) {
        frames_.erase(it);
        return std::nullopt;
    }

    auto &block = frame.blocks[packet.block_index];
    if (block.shards.empty()) {
        block.metadata = packet;
        block.shards.resize(static_cast<std::size_t>(total_shards));
    } else if (!same_block_metadata(block.metadata, packet)) {
        frames_.erase(it);
        return std::nullopt;
    }
    if (block.recovered) {
        return std::nullopt;
    }

    auto &slot = block.shards[packet.shard_index];
    if (!slot) {
        slot = std::move(packet.payload);
        ++block.received;
    }
    if (block.received < block.metadata.data_shards) {
        return std::nullopt;
    }

    auto data = fec_.decode(
        block.metadata.data_shards,
        block.metadata.parity_shards,
        block.metadata.shard_size,
        block.metadata.block_original_size,
        block.shards);
    if (!data) {
        return std::nullopt;
    }
    block.recovered = std::move(*data);
    block.shards.clear();
    ++frame.recovered_blocks;
    if (frame.recovered_blocks < frame.blocks.size()) {
        return std::nullopt;
    }

    RecoveredFrame result;
    result.session_id = frame.metadata.session_id;
    result.session_started_unix_us =
        frame.metadata.session_started_unix_us;
    result.stream_epoch = frame.metadata.stream_epoch;
    result.frame_id = frame.metadata.frame_id;
    result.encoded_at_unix_us = frame.metadata.encoded_at_unix_us;
    result.source_to_encoded_us = frame.metadata.source_to_encoded_us;
    result.bitrate_kbps = frame.metadata.bitrate_kbps;
    result.width = frame.metadata.width;
    result.height = frame.metadata.height;
    result.keyframe = frame.metadata.keyframe;
    result.data.reserve(frame.metadata.original_size);
    for (auto &complete_block : frame.blocks) {
        if (!complete_block.recovered) {
            frames_.erase(key);
            return std::nullopt;
        }
        result.data.insert(
            result.data.end(),
            complete_block.recovered->begin(),
            complete_block.recovered->end());
    }
    if (result.data.size() != frame.metadata.original_size ||
        frame_crc32(result.data.data(), result.data.size()) !=
            frame.metadata.frame_crc) {
        frames_.erase(key);
        return std::nullopt;
    }
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
