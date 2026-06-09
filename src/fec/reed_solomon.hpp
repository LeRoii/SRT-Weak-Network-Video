#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct FecBlock {
    uint16_t data_shards = 0;
    uint16_t parity_shards = 0;
    uint16_t shard_size = 0;
    std::vector<std::vector<uint8_t>> shards;
};

class ReedSolomon {
public:
    static constexpr std::size_t kTargetShardBytes = 1000;
    static constexpr int kMaxShards = 255;

    FecBlock encode(const std::vector<uint8_t> &data,
                    double parity_ratio) const;

    std::optional<std::vector<uint8_t>> decode(
        uint16_t data_shards,
        uint16_t parity_shards,
        uint16_t shard_size,
        uint32_t original_size,
        const std::vector<std::optional<std::vector<uint8_t>>> &shards) const;
};

uint32_t frame_crc32(const uint8_t *data, std::size_t size);
