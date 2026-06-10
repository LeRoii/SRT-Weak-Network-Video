#include "common/utils.hpp"
#include "fec/reed_solomon.hpp"
#include "protocol/frame_assembler.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

int main() {
    std::vector<uint8_t> data(37'321);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 31U + 7U) & 0xffU);
    }

    ReedSolomon fec;
    const auto encoded = fec.encode(data, 1.0);
    std::vector<std::optional<std::vector<uint8_t>>> received;
    for (const auto &shard : encoded.shards) {
        received.emplace_back(shard);
    }
    for (std::size_t i = 0; i < encoded.data_shards; i += 2) {
        received[i].reset();
    }

    const auto decoded = fec.decode(
        encoded.data_shards, encoded.parity_shards, encoded.shard_size,
        static_cast<uint32_t>(data.size()), received);
    assert(decoded);
    assert(*decoded == data);

    ShardPacket shard;
    shard.stream_epoch = 42;
    shard.frame_id = 99;
    shard.pts_us = 1234567;
    shard.original_size = static_cast<uint32_t>(data.size());
    shard.frame_crc = frame_crc32(data.data(), data.size());
    shard.bitrate_kbps = 600;
    shard.width = 640;
    shard.height = 360;
    shard.data_shards = encoded.data_shards;
    shard.parity_shards = encoded.parity_shards;
    shard.shard_index = 3;
    shard.shard_size = encoded.shard_size;
    shard.keyframe = true;
    shard.payload = encoded.shards[3];

    const auto wire = encode_shard_packet(shard);
    const auto parsed = parse_message(wire.data(), wire.size());
    assert(parsed && parsed->shard);
    assert(parsed->shard->frame_id == shard.frame_id);
    assert(parsed->shard->payload == shard.payload);
    assert(parsed->shard->keyframe);

    NetworkReport report;
    report.sequence = 17;
    report.loss_basis_points = 9123;
    const auto report_wire = encode_network_report(report);
    const auto parsed_report =
        parse_message(report_wire.data(), report_wire.size());
    assert(parsed_report && parsed_report->network_report);
    assert(parsed_report->network_report->sequence == report.sequence);
    assert(parsed_report->network_report->loss_basis_points ==
           report.loss_basis_points);

    FrameAssembler assembler;
    std::optional<RecoveredFrame> recovered;
    for (std::size_t i = 0; i < encoded.shards.size(); ++i) {
        shard.shard_index = static_cast<uint16_t>(i);
        shard.payload = encoded.shards[i];
        if (auto frame = assembler.push(shard)) {
            recovered = std::move(frame);
        }
    }
    assert(recovered);
    assert(recovered->data == data);
    assert(assembler.expire(monotonic_us() + 1'000'000, 450'000)
               .expired_frames == 0);

    std::cout << "protocol_tests=passed" << std::endl;
    return 0;
}
