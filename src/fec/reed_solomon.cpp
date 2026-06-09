#include "fec/reed_solomon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <isa-l/crc.h>
#include <isa-l/erasure_code.h>
}

namespace {

std::vector<uint8_t> make_coding_matrix(int total, int data) {
    std::vector<uint8_t> matrix(static_cast<std::size_t>(total * data));
    gf_gen_cauchy1_matrix(matrix.data(), total, data);
    return matrix;
}

} // namespace

FecBlock ReedSolomon::encode(const std::vector<uint8_t> &data,
                             double parity_ratio) const {
    if (data.empty()) {
        throw std::runtime_error("cannot FEC-encode an empty frame");
    }

    const int data_shards = static_cast<int>(
        (data.size() + kTargetShardBytes - 1) / kTargetShardBytes);
    if (data_shards <= 0 || data_shards >= kMaxShards) {
        throw std::runtime_error("encoded frame is too large for one FEC block");
    }

    const std::size_t shard_size =
        (data.size() + static_cast<std::size_t>(data_shards) - 1) /
        static_cast<std::size_t>(data_shards);
    int parity_shards = std::max(
        1, static_cast<int>(std::ceil(data_shards * parity_ratio)));
    parity_shards = std::min(parity_shards, kMaxShards - data_shards);
    const int total_shards = data_shards + parity_shards;

    FecBlock result;
    result.data_shards = static_cast<uint16_t>(data_shards);
    result.parity_shards = static_cast<uint16_t>(parity_shards);
    result.shard_size = static_cast<uint16_t>(shard_size);
    result.shards.assign(
        static_cast<std::size_t>(total_shards),
        std::vector<uint8_t>(shard_size, 0));

    for (int index = 0; index < data_shards; ++index) {
        const std::size_t offset = static_cast<std::size_t>(index) * shard_size;
        const std::size_t bytes =
            std::min(shard_size, data.size() - std::min(offset, data.size()));
        if (bytes > 0) {
            std::memcpy(result.shards[static_cast<std::size_t>(index)].data(),
                        data.data() + offset, bytes);
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
