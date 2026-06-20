#include "fec/reed_solomon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

extern "C" {
#if __has_include(<isa-l/crc.h>)
#include <isa-l/crc.h>
#include <isa-l/erasure_code.h>
#else
#include <isal/crc.h>
#include <isal/erasure_code.h>
#endif
}

namespace {

std::vector<uint8_t> make_coding_matrix(int total, int data) {
    std::vector<uint8_t> matrix(static_cast<std::size_t>(total * data));
    gf_gen_cauchy1_matrix(matrix.data(), total, data);
    return matrix;
}

int maximum_data_shards(double parity_ratio) {
    for (int data_shards = ReedSolomon::kMaxShards - 1;
         data_shards > 0;
         --data_shards) {
        const int parity_shards = std::max(
            1, static_cast<int>(std::ceil(data_shards * parity_ratio)));
        if (data_shards + parity_shards <= ReedSolomon::kMaxShards) {
            return data_shards;
        }
    }
    return 0;
}

FecBlock encode_block(const uint8_t *data,
                      std::size_t size,
                      double parity_ratio,
                      int minimum_data_shards) {
    if (!data || size == 0) {
        throw std::runtime_error("cannot FEC-encode an empty block");
    }

    const int maximum_data = maximum_data_shards(parity_ratio);
    int data_shards = static_cast<int>(
        (size + ReedSolomon::kTargetShardBytes - 1) /
        ReedSolomon::kTargetShardBytes);
    data_shards = std::max(data_shards, minimum_data_shards);
    if (data_shards <= 0 || data_shards > maximum_data) {
        throw std::runtime_error("FEC block exceeds the shard limit");
    }

    const std::size_t shard_size =
        (size + static_cast<std::size_t>(data_shards) - 1) /
        static_cast<std::size_t>(data_shards);
    const int parity_shards = std::max(
        1, static_cast<int>(std::ceil(data_shards * parity_ratio)));
    const int total_shards = data_shards + parity_shards;

    FecBlock result;
    result.original_size = static_cast<uint32_t>(size);
    result.data_shards = static_cast<uint16_t>(data_shards);
    result.parity_shards = static_cast<uint16_t>(parity_shards);
    result.shard_size = static_cast<uint16_t>(shard_size);
    result.shards.assign(
        static_cast<std::size_t>(total_shards),
        std::vector<uint8_t>(shard_size, 0));

    for (int index = 0; index < data_shards; ++index) {
        const std::size_t offset = static_cast<std::size_t>(index) * shard_size;
        const std::size_t bytes =
            std::min(shard_size, size - std::min(offset, size));
        if (bytes > 0) {
            std::memcpy(result.shards[static_cast<std::size_t>(index)].data(),
                        data + offset, bytes);
        }
    }

    const auto matrix = make_coding_matrix(total_shards, data_shards);
    std::vector<uint8_t> tables(
        static_cast<std::size_t>(32 * data_shards * parity_shards));
    ec_init_tables(data_shards, parity_shards,
                   const_cast<uint8_t *>(matrix.data()) +
                       data_shards * data_shards,
                   tables.data());

    std::vector<uint8_t *> inputs;
    std::vector<uint8_t *> outputs;
    for (int index = 0; index < data_shards; ++index) {
        inputs.push_back(result.shards[static_cast<std::size_t>(index)].data());
    }
    for (int index = data_shards; index < total_shards; ++index) {
        outputs.push_back(
            result.shards[static_cast<std::size_t>(index)].data());
    }
    ec_encode_data(static_cast<int>(shard_size), data_shards, parity_shards,
                   tables.data(), inputs.data(), outputs.data());
    return result;
}

} // namespace

FecBlock ReedSolomon::encode(const std::vector<uint8_t> &data,
                             double parity_ratio) const {
    if (data.empty()) {
        throw std::runtime_error("cannot FEC-encode an empty frame");
    }
    return encode_block(data.data(), data.size(), parity_ratio, 1);
}

std::vector<FecBlock> ReedSolomon::encode_blocks(
    const std::vector<uint8_t> &data,
    double parity_ratio,
    int minimum_data_shards) const {
    if (data.empty()) {
        throw std::runtime_error("cannot FEC-encode an empty frame");
    }
    if (parity_ratio < 0.0 || minimum_data_shards <= 0) {
        throw std::runtime_error("invalid FEC block parameters");
    }
    const int maximum_data = maximum_data_shards(parity_ratio);
    if (maximum_data < minimum_data_shards) {
        throw std::runtime_error("FEC ratio leaves too few data shards");
    }
    const std::size_t maximum_block_bytes =
        static_cast<std::size_t>(maximum_data) * kTargetShardBytes;

    std::vector<FecBlock> blocks;
    for (std::size_t offset = 0; offset < data.size();) {
        const std::size_t bytes =
            std::min(maximum_block_bytes, data.size() - offset);
        blocks.push_back(encode_block(
            data.data() + offset, bytes, parity_ratio, minimum_data_shards));
        offset += bytes;
    }
    return blocks;
}

std::optional<std::vector<uint8_t>> ReedSolomon::decode(
    uint16_t data_shards_u16,
    uint16_t parity_shards_u16,
    uint16_t shard_size_u16,
    uint32_t original_size,
    const std::vector<std::optional<std::vector<uint8_t>>> &shards) const {
    const int data_shards = data_shards_u16;
    const int parity_shards = parity_shards_u16;
    const int total_shards = data_shards + parity_shards;
    const int shard_size = shard_size_u16;
    if (data_shards <= 0 || parity_shards <= 0 ||
        total_shards > kMaxShards || shard_size <= 0 ||
        shards.size() != static_cast<std::size_t>(total_shards) ||
        original_size > static_cast<uint32_t>(data_shards * shard_size)) {
        return std::nullopt;
    }

    std::vector<int> selected_indices;
    for (int index = 0;
         index < total_shards &&
         selected_indices.size() < static_cast<std::size_t>(data_shards);
         ++index) {
        if (shards[static_cast<std::size_t>(index)] &&
            shards[static_cast<std::size_t>(index)]->size() ==
                static_cast<std::size_t>(shard_size)) {
            selected_indices.push_back(index);
        }
    }
    if (selected_indices.size() < static_cast<std::size_t>(data_shards)) {
        return std::nullopt;
    }

    const auto matrix = make_coding_matrix(total_shards, data_shards);
    std::vector<uint8_t> selected_matrix(
        static_cast<std::size_t>(data_shards * data_shards));
    for (int row = 0; row < data_shards; ++row) {
        std::memcpy(
            selected_matrix.data() + row * data_shards,
            matrix.data() + selected_indices[static_cast<std::size_t>(row)] *
                                data_shards,
            static_cast<std::size_t>(data_shards));
    }

    std::vector<uint8_t> inverse(
        static_cast<std::size_t>(data_shards * data_shards));
    if (gf_invert_matrix(selected_matrix.data(), inverse.data(),
                         data_shards) < 0) {
        return std::nullopt;
    }

    std::vector<std::vector<uint8_t>> recovered(
        static_cast<std::size_t>(data_shards),
        std::vector<uint8_t>(static_cast<std::size_t>(shard_size)));
    std::vector<uint8_t *> inputs;
    std::vector<uint8_t *> outputs;
    for (const int index : selected_indices) {
        inputs.push_back(const_cast<uint8_t *>(
            shards[static_cast<std::size_t>(index)]->data()));
    }
    for (auto &shard : recovered) {
        outputs.push_back(shard.data());
    }

    std::vector<uint8_t> tables(
        static_cast<std::size_t>(32 * data_shards * data_shards));
    ec_init_tables(data_shards, data_shards, inverse.data(), tables.data());
    ec_encode_data(shard_size, data_shards, data_shards, tables.data(),
                   inputs.data(), outputs.data());

    std::vector<uint8_t> result;
    result.reserve(original_size);
    for (const auto &shard : recovered) {
        const std::size_t remaining =
            original_size - std::min<std::size_t>(result.size(), original_size);
        const std::size_t bytes = std::min(remaining, shard.size());
        result.insert(result.end(), shard.begin(), shard.begin() + bytes);
        if (result.size() == original_size) {
            break;
        }
    }
    return result;
}

uint32_t frame_crc32(const uint8_t *data, std::size_t size) {
    return crc32_gzip_refl(0, data, size);
}
