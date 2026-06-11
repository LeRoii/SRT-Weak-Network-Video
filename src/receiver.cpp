#include "receiver.hpp"

#include "common/network_quality.hpp"
#include "common/utils.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

namespace {

std::string format_latency(const std::optional<double> &milliseconds) {
    if (!milliseconds) {
        return "n/a";
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << *milliseconds << "ms";
    return output.str();
}

uint64_t source_timestamp(const RecoveredFrame &frame) {
    if (frame.encoded_at_unix_us < frame.source_to_encoded_us) {
        return 0;
    }
    return frame.encoded_at_unix_us - frame.source_to_encoded_us;
}

} // namespace

ReceiverApp::ReceiverApp(Endpoint local,
                         std::string output_file,
                         bool display_enabled,
                         bool write_h264,
                         LatencyConfig latency_config)
    : local_(std::move(local)),
      latency_config_(latency_config),
      latency_stats_(latency_config.window_seconds),
      renderer_(&latency_stats_),
      decoder_(renderer_),
      display_enabled_(display_enabled),
      write_h264_(write_h264) {
    if (write_h264_) {
        output_.open(output_file, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("cannot open receiver output file");
        }
    }
    renderer_.start(display_enabled_);
}

ReceiverApp::~ReceiverApp() {
    renderer_.stop();
}

void ReceiverApp::run() {
    auto listener = SrtSocket::create_listener(local_);
    std::cerr << "srt_listening=" << local_.host << ":"
              << local_.port << std::endl;

    while (!g_stop_requested.load()) {
        try {
            auto socket = listener.accept();
            std::cerr << "srt_connected=1" << std::endl;
            assembler_.reset();
            renderer_.discard_pending();
            latency_generation_ = latency_stats_.reset();
            run_connection(socket);
            std::cerr << "srt_connected=0" << std::endl;
        } catch (const std::exception &error) {
            if (!g_stop_requested.load()) {
                std::cerr << "srt_accept_failed=" << error.what() << std::endl;
            }
        }
    }
}

void ReceiverApp::run_connection(SrtSocket &socket) {
    auto next_stats = std::chrono::steady_clock::now() +
                      std::chrono::seconds(1);
    uint64_t network_report_sequence = 0;
    NetworkLossEstimator loss_estimator;
    std::optional<uint32_t> reliable_loss_basis_points;
    uint64_t last_resolved_packets = 0;
    auto last_receive_progress = std::chrono::steady_clock::now();
    bool receive_stalled = false;
    while (!g_stop_requested.load()) {
        std::vector<uint8_t> message;
        const auto result = socket.receive(message);
        if (result == ReceiveResult::Closed) {
            std::cerr << "srt_receive_failed=" << srt_last_error()
                      << " state=" << socket.state_name() << std::endl;
            break;
        }
        if (result == ReceiveResult::Timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (result == ReceiveResult::Data) {
            const auto parsed = parse_message(message.data(), message.size());
            if (parsed && parsed->shard) {
                if (auto frame = assembler_.push(std::move(*parsed->shard))) {
                    if (latency_config_.metric ==
                        LatencyMetric::EncodeToAssemble) {
                        latency_stats_.record(
                            latency_generation_,
                            frame->encoded_at_unix_us,
                            unix_time_us(),
                            monotonic_us());
                    }
                    handle_frame(std::move(*frame));
                }
            }
        }

        const auto cleanup = assembler_.expire(monotonic_us(), 450'000);
        if (cleanup.expired_frames > 0) {
            dropped_frames_ += cleanup.expired_frames;
            synchronized_ = false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_stats) {
            const auto network = socket.receiver_network_snapshot(false);
            if (network.valid) {
                if (network.sent_packets > last_resolved_packets) {
                    if (receive_stalled) {
                        loss_estimator.reset(network.sent_packets,
                                             network.lost_packets);
                    }
                    last_resolved_packets = network.sent_packets;
                    last_receive_progress = now;
                    receive_stalled = false;
                } else if (now - last_receive_progress >=
                           std::chrono::seconds(3)) {
                    receive_stalled = true;
                }

                if (const auto loss_percent = loss_estimator.update(
                        network.sent_packets, network.lost_packets)) {
                    reliable_loss_basis_points = static_cast<uint32_t>(
                        std::clamp(std::lround(*loss_percent * 100.0),
                                   0L, 10'000L));
                }

                if (receive_stalled || reliable_loss_basis_points) {
                    NetworkReport report;
                    report.sequence = ++network_report_sequence;
                    report.loss_basis_points = receive_stalled
                        ? 10'000
                        : *reliable_loss_basis_points;
                    socket.send(encode_network_report(report), 1500);
                }
            }
            const auto latency =
                latency_stats_.snapshot(monotonic_us());
            std::cout << "bandwidth=" << network.bandwidth_kbps << "kbps "
                      << "rtt=" << network.rtt_ms << "ms "
                      << "latency_type="
                      << latency_metric_name(latency_config_.metric) << " "
                      << "latency_avg="
                      << format_latency(latency.average_ms) << " "
                      << "latency_p95="
                      << format_latency(latency.p95_ms) << " "
                      << "latency_invalid="
                      << latency.invalid_samples << " "
                      << "frames=" << completed_frames_ << " "
                      << "dropped=" << dropped_frames_ << " "
                      << "decoder_errors=" << decoder_errors_ << " "
                      << "sync=" << (synchronized_ ? 1 : 0) << " "
                      << "rendered=" << renderer_.rendered_frames()
                      << std::endl;
            next_stats = now + std::chrono::seconds(1);
        }
    }
}

void ReceiverApp::handle_frame(RecoveredFrame frame) {
    if (stream_epoch_ != 0 && frame.stream_epoch < stream_epoch_) {
        ++dropped_frames_;
        return;
    }

    if (frame.stream_epoch != stream_epoch_) {
        stream_epoch_ = frame.stream_epoch;
        synchronized_ = false;
        have_decoded_frame_id_ = false;
        decoder_.reset();
    }

    if (have_decoded_frame_id_ &&
        frame.frame_id <= last_decoded_frame_id_) {
        ++dropped_frames_;
        return;
    }
    if (synchronized_ && have_decoded_frame_id_ &&
        frame.frame_id != last_decoded_frame_id_ + 1) {
        synchronized_ = false;
    }

    if (!synchronized_ && !frame.keyframe) {
        ++dropped_frames_;
        return;
    }

    if (!synchronized_ && frame.keyframe) {
        decoder_.reset();
    }
    const uint64_t display_start =
        latency_config_.metric == LatencyMetric::SourceToDisplay
            ? source_timestamp(frame)
            : 0;
    uint64_t decoded_at_unix_us = 0;
    if (!decoder_.decode(frame.data.data(), frame.data.size(),
                         display_start, latency_generation_,
                         &decoded_at_unix_us)) {
        ++decoder_errors_;
        synchronized_ = false;
        return;
    }
    if (latency_config_.metric == LatencyMetric::EncodeToDecode) {
        latency_stats_.record(
            latency_generation_,
            frame.encoded_at_unix_us,
            decoded_at_unix_us,
            monotonic_us());
    }

    synchronized_ = true;
    have_decoded_frame_id_ = true;
    last_decoded_frame_id_ = frame.frame_id;
    ++completed_frames_;
    if (write_h264_ && !output_started_ && frame.keyframe) {
        output_started_ = true;
    }
    if (write_h264_ && output_started_) {
        output_.write(reinterpret_cast<const char *>(frame.data.data()),
                      static_cast<std::streamsize>(frame.data.size()));
        output_.flush();
    }
}
