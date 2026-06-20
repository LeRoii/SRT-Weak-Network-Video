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
    shard.session_id = 1234;
    shard.session_started_unix_us = 1'765'432'000'000'000;
    shard.packet_sequence = 88;
    shard.sent_monotonic_us = 987'654'321;
    shard.stream_epoch = 42;
    shard.frame_id = 99;
    shard.encoded_at_unix_us = 1'765'432'100'123'456;
    shard.source_to_encoded_us = 12'345;
    shard.original_size = static_cast<uint32_t>(data.size());
    shard.frame_crc = frame_crc32(data.data(), data.size());
    shard.bitrate_kbps = 600;
    shard.width = 640;
    shard.height = 360;
    shard.block_index = 0;
    shard.block_count = 1;
    shard.block_original_size =
        static_cast<uint32_t>(data.size());
    shard.data_shards = encoded.data_shards;
    shard.parity_shards = encoded.parity_shards;
    shard.shard_index = 3;
    shard.shard_size = encoded.shard_size;
    shard.keyframe = true;
    shard.payload = encoded.shards[3];

    const auto wire = encode_shard_packet(shard);
    const auto parsed = parse_message(wire.data(), wire.size());
    assert(parsed && parsed->shard);
    assert(parsed->shard->session_id == shard.session_id);
    assert(parsed->shard->packet_sequence ==
           shard.packet_sequence);
    assert(parsed->shard->frame_id == shard.frame_id);
    assert(parsed->shard->encoded_at_unix_us ==
           shard.encoded_at_unix_us);
    assert(parsed->shard->source_to_encoded_us ==
           shard.source_to_encoded_us);
    assert(parsed->shard->payload == shard.payload);
    assert(parsed->shard->keyframe);
    auto old_version_wire = wire;
    old_version_wire[4] = 1;
    assert(!parse_message(old_version_wire.data(),
                          old_version_wire.size()));
    assert(!parse_message(wire.data(), wire.size() - 1));

    UdpFeedback feedback;
    feedback.session_id = shard.session_id;
    feedback.sequence = 11;
    feedback.highest_packet_sequence = 900;
    feedback.unique_packets = 270;
    feedback.received_bytes = 123'456;
    feedback.last_complete_frame_id = 77;
    feedback.echoed_sender_monotonic_us = 999'000;
    feedback.last_frame_age_ms = 240;
    feedback.request_keyframe = true;
    const auto feedback_wire = encode_udp_feedback(feedback);
    const auto parsed_feedback =
        parse_message(feedback_wire.data(), feedback_wire.size());
    assert(parsed_feedback && parsed_feedback->udp_feedback);
    assert(parsed_feedback->udp_feedback->session_id ==
           feedback.session_id);
    assert(parsed_feedback->udp_feedback->unique_packets ==
           feedback.unique_packets);
    assert(parsed_feedback->udp_feedback->request_keyframe);

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
    assert(recovered->encoded_at_unix_us ==
           shard.encoded_at_unix_us);
    assert(recovered->source_to_encoded_us ==
           shard.source_to_encoded_us);
    assert(assembler.expire(monotonic_us() + 1'000'000, 450'000)
               .expired_frames == 0);

    std::vector<uint8_t> large_data(700'000);
    for (std::size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] =
            static_cast<uint8_t>((i * 17U + 5U) & 0xffU);
    }
    const auto blocks = fec.encode_blocks(large_data, 15.0, 4);
    assert(blocks.size() > 1);
    for (const auto &block : blocks) {
        assert(block.data_shards >= 4);
        assert(block.data_shards + block.parity_shards <=
               ReedSolomon::kMaxShards);
    }

    FrameAssembler multi_block_assembler;
    const uint32_t large_crc = frame_crc32(
        large_data.data(), large_data.size());
    std::optional<RecoveredFrame> multi_recovered;
    for (std::size_t block_index = 0;
         block_index < blocks.size();
         ++block_index) {
        const auto &block = blocks[block_index];
        for (std::size_t shard_index = 0;
             shard_index < block.shards.size();
             ++shard_index) {
            ShardPacket packet = shard;
            packet.frame_id = 100;
            packet.original_size =
                static_cast<uint32_t>(large_data.size());
            packet.frame_crc = large_crc;
            packet.block_index =
                static_cast<uint16_t>(block_index);
            packet.block_count =
                static_cast<uint16_t>(blocks.size());
            packet.block_original_size = block.original_size;
            packet.data_shards = block.data_shards;
            packet.parity_shards = block.parity_shards;
            packet.shard_index =
                static_cast<uint16_t>(shard_index);
            packet.shard_size = block.shard_size;
            packet.payload = block.shards[shard_index];
            if (auto frame =
                    multi_block_assembler.push(std::move(packet))) {
                multi_recovered = std::move(frame);
            }
        }
    }
    assert(multi_recovered);
    assert(multi_recovered->data == large_data);

    std::cout << "protocol_tests=passed" << std::endl;
    return 0;
}
