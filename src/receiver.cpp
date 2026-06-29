#include "receiver.hpp"

#include "common/network_quality.hpp"
#include "common/utils.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

std::string format_latency(const std::optional<double> &milliseconds) {
    if (!milliseconds) {
        return "n/a";
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(1)
           << *milliseconds << "ms";
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
                         int minimum_output_width,
                         int minimum_output_height,
                         int minimum_output_fps,
                         UpscaleMode upscale_mode,
                         InterpolationMode interpolation_mode,
                         bool write_h264,
                         LatencyConfig latency_config,
                         TransportConfig transport_config)
    : local_(std::move(local)),
      latency_config_(latency_config),
      transport_config_(transport_config),
      latency_stats_(latency_config.window_seconds),
      renderer_(minimum_output_width,
                minimum_output_height,
                minimum_output_fps,
                upscale_mode,
                interpolation_mode,
                &latency_stats_),
      decoder_(renderer_),
      display_enabled_(display_enabled),
      write_h264_(write_h264) {
    if (write_h264_) {
        output_.open(output_file, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw std::runtime_error(
                "cannot open receiver output file");
        }
    }
    renderer_.start(display_enabled_);
}

ReceiverApp::~ReceiverApp() {
    renderer_.stop();
}

void ReceiverApp::run() {
    receiver_started_at_ = std::chrono::steady_clock::now();
    if (transport_config_.mode == TransportMode::Udp) {
        run_udp();
    } else {
        run_srt();
    }
}

void ReceiverApp::run_srt() {
    auto listener = SrtSocket::create_listener(local_);
    std::cerr << "srt_listening="
              << local_.host << ":" << local_.port << std::endl;

    while (!g_stop_requested.load()) {
        try {
            auto socket = listener.accept();
            std::cerr << "srt_connected=1"
                      << std::endl;
            reset_media_session();
            run_srt_connection(socket);
            std::cerr << "srt_connected=0"
                      << std::endl;
        } catch (const std::exception &error) {
            if (!g_stop_requested.load()) {
                std::cerr << "srt_accept_failed="
                          << error.what() << std::endl;
            }
        }
    }
}

void ReceiverApp::run_srt_connection(SrtSocket &socket) {
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
                      << " state=" << socket.state_name()
                      << std::endl;
            break;
        }
        if (result == ReceiveResult::Timeout) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        }
        if (result == ReceiveResult::Data) {
            const auto parsed =
                parse_message(message.data(), message.size());
            if (parsed && parsed->shard) {
                if (auto frame =
                        assembler_.push(std::move(*parsed->shard))) {
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

        const auto cleanup =
            assembler_.expire(monotonic_us(), 450'000);
        if (cleanup.expired_frames > 0) {
            dropped_frames_ += cleanup.expired_frames;
            synchronized_ = false;
            keyframe_request_pending_ = true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_stats) {
            const auto network =
                socket.receiver_network_snapshot(false);
            if (network.valid) {
                if (network.sent_packets > last_resolved_packets) {
                    if (receive_stalled) {
                        loss_estimator.reset(
                            network.sent_packets,
                            network.lost_packets);
                    }
                    last_resolved_packets = network.sent_packets;
                    last_receive_progress = now;
                    receive_stalled = false;
                } else if (now - last_receive_progress >=
                           std::chrono::seconds(3)) {
                    receive_stalled = true;
                }

                if (const auto loss_percent =
                        loss_estimator.update(
                            network.sent_packets,
                            network.lost_packets)) {
                    reliable_loss_basis_points =
                        static_cast<uint32_t>(
                            std::clamp(
                                std::lround(
                                    *loss_percent * 100.0),
                                0L, 10'000L));
                }

                if (receive_stalled ||
                    reliable_loss_basis_points) {
                    NetworkReport report;
                    report.sequence =
                        ++network_report_sequence;
                    report.loss_basis_points =
                        receive_stalled
                            ? 10'000
                            : *reliable_loss_basis_points;
                    socket.send(
                        encode_network_report(report), 1500);
                }
            }
            const auto latency =
                latency_stats_.snapshot(monotonic_us());
            const auto renderer_stats = renderer_.take_stats();
            std::cout
                << "bandwidth=" << network.bandwidth_kbps
                << "kbps "
                << "rtt=" << network.rtt_ms << "ms "
                << "latency_avg="
                << format_latency(latency.average_ms) << " "
                << "latency_p95="
                << format_latency(latency.p95_ms) << " "
                << "latency_max="
                << format_latency(latency.max_ms) << " "
                << "latency_samples="
                << latency.sample_count << " "
                << "latency_invalid="
                << latency.invalid_samples << " "
                << "frames=" << completed_frames_ << " "
                << "dropped=" << dropped_frames_ << " "
                << "decoder_errors=" << decoder_errors_ << " "
                << "sync=" << (synchronized_ ? 1 : 0) << " "
                << "rendered=" << renderer_.rendered_frames()
                << " "
                << "decoded_fps="
                << renderer_stats.decoded_frames << " "
                << "last_frame_age_ms=" << last_frame_age_ms()
                << " "
                << "max_frame_gap_ms=" << maximum_frame_gap_ms()
                << std::endl;
            next_stats = now + std::chrono::seconds(1);
        }
    }
}

void ReceiverApp::run_udp() {
    auto socket = UdpSocket::create_receiver(local_);
    UdpPeer feedback_peer;
    uint64_t feedback_sequence = 0;
    uint64_t highest_packet_sequence = 0;
    uint64_t unique_packets = 0;
    uint64_t received_bytes = 0;
    uint64_t latest_probe_sender_monotonic_us = 0;
    std::unordered_set<uint64_t> recent_sequences;
    std::deque<uint64_t> sequence_order;
    constexpr std::size_t kSequenceHistory = 20'000;
    auto next_feedback_report = std::chrono::steady_clock::now();
    auto next_feedback_copy = std::chrono::steady_clock::now();
    std::vector<uint8_t> pending_feedback;
    int pending_feedback_copies = 0;
    auto next_stats =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    uint64_t stats_bytes = 0;
    uint64_t stats_highest = 0;
    uint64_t stats_unique = 0;
    auto stats_at = std::chrono::steady_clock::now();

    std::cerr << "udp_listening="
              << local_.host << ":" << local_.port << std::endl;

    auto accept_session = [&](uint64_t session_id,
                              uint64_t session_started_unix_us) {
        const bool newer_session =
            active_session_id_ == 0 ||
            session_started_unix_us >
                active_session_started_unix_us_ ||
            (session_started_unix_us ==
                 active_session_started_unix_us_ &&
             session_id > active_session_id_);
        if (session_id == active_session_id_) {
            return true;
        }
        if (!newer_session) {
            return false;
        }
        active_session_id_ = session_id;
        active_session_started_unix_us_ =
            session_started_unix_us;
        reset_media_session();
        highest_packet_sequence = 0;
        unique_packets = 0;
        received_bytes = 0;
        latest_probe_sender_monotonic_us = 0;
        recent_sequences.clear();
        sequence_order.clear();
        feedback_sequence = 0;
        std::cerr
            << "udp_session=1 session_id="
            << active_session_id_ << std::endl;
        return true;
    };

    while (!g_stop_requested.load()) {
        std::vector<uint8_t> message;
        UdpPeer source;
        bool force_feedback_report = false;
        const auto result = socket.receive(message, &source);
        if (result == UdpResult::Data) {
            const auto parsed =
                parse_message(message.data(), message.size());
            if (parsed && parsed->shard) {
                auto packet = std::move(*parsed->shard);
                if (!accept_session(packet.session_id,
                                    packet.session_started_unix_us)) {
                    continue;
                }

                feedback_peer = source;
                if (recent_sequences
                        .insert(packet.packet_sequence).second) {
                    sequence_order.push_back(
                        packet.packet_sequence);
                    ++unique_packets;
                    received_bytes += message.size();
                    if (packet.packet_sequence >=
                        highest_packet_sequence) {
                        highest_packet_sequence =
                            packet.packet_sequence;
                    }
                    while (sequence_order.size() >
                           kSequenceHistory) {
                        recent_sequences.erase(
                            sequence_order.front());
                        sequence_order.pop_front();
                    }
                }

                if (auto frame =
                        assembler_.push(std::move(packet))) {
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
            } else if (parsed && parsed->udp_probe) {
                const auto &probe = *parsed->udp_probe;
                if (!accept_session(probe.session_id,
                                    probe.session_started_unix_us)) {
                    continue;
                }
                feedback_peer = source;
                latest_probe_sender_monotonic_us =
                    probe.sender_monotonic_us;
                force_feedback_report = true;
            }
        } else if (result == UdpResult::WouldBlock) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        } else {
            std::cerr << "udp_receive_failed=socket closed"
                      << std::endl;
            break;
        }

        const auto cleanup =
            assembler_.expire(monotonic_us(), 450'000);
        if (cleanup.expired_frames > 0) {
            dropped_frames_ += cleanup.expired_frames;
            synchronized_ = false;
            keyframe_request_pending_ = true;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool periodic_feedback_due =
            now >= next_feedback_report;
        if ((force_feedback_report || periodic_feedback_due) &&
            feedback_peer.valid &&
            active_session_id_ != 0) {
            UdpFeedback feedback;
            feedback.session_id = active_session_id_;
            feedback.sequence = ++feedback_sequence;
            feedback.highest_packet_sequence =
                highest_packet_sequence;
            feedback.unique_packets = unique_packets;
            feedback.received_bytes = received_bytes;
            feedback.last_complete_frame_id =
                have_decoded_frame_id_
                    ? last_decoded_frame_id_
                    : 0;
            feedback.echoed_sender_monotonic_us =
                latest_probe_sender_monotonic_us;
            feedback.last_frame_age_ms = last_frame_age_ms();
            feedback.request_keyframe =
                keyframe_request_pending_ ||
                feedback.last_frame_age_ms > 1500;
            feedback.rtt_probe_response = force_feedback_report;
            pending_feedback = encode_udp_feedback(feedback);
            pending_feedback_copies = feedback.rtt_probe_response
                ? 1
                : transport_config_.feedback_redundancy;
            next_feedback_copy = now;
            if (!feedback.rtt_probe_response) {
                next_feedback_report =
                    now + std::chrono::milliseconds(
                              transport_config_.feedback_interval_ms);
            }
        }
        if (pending_feedback_copies > 0 &&
            now >= next_feedback_copy &&
            feedback_peer.valid) {
            socket.send_to(pending_feedback, feedback_peer);
            --pending_feedback_copies;
            const int copy_interval_ms = std::max(
                1,
                transport_config_.feedback_interval_ms /
                    transport_config_.feedback_redundancy);
            next_feedback_copy =
                now + std::chrono::milliseconds(copy_interval_ms);
        }

        if (now >= next_stats) {
            const double elapsed =
                std::chrono::duration<double>(
                    now - stats_at).count();
            const double bandwidth_kbps =
                elapsed > 0.0
                    ? static_cast<double>(
                          received_bytes - stats_bytes) *
                          8.0 / 1000.0 / elapsed
                    : 0.0;
            const uint64_t sent =
                highest_packet_sequence >= stats_highest
                    ? highest_packet_sequence - stats_highest
                    : 0;
            const uint64_t received =
                unique_packets >= stats_unique
                    ? std::min(
                          sent,
                          unique_packets - stats_unique)
                    : 0;
            const double loss_percent =
                sent > 0
                    ? static_cast<double>(sent - received) *
                          100.0 / static_cast<double>(sent)
                    : 0.0;
            const auto latency =
                latency_stats_.snapshot(monotonic_us());
            const auto renderer_stats = renderer_.take_stats();
            std::cout
                << "bandwidth=" << bandwidth_kbps << "kbps "
                << "loss=" << loss_percent << "% "
                << "latency_avg="
                << format_latency(latency.average_ms) << " "
                << "latency_p95="
                << format_latency(latency.p95_ms) << " "
                << "latency_max="
                << format_latency(latency.max_ms) << " "
                << "latency_samples="
                << latency.sample_count << " "
                << "latency_invalid="
                << latency.invalid_samples << " "
                << "frames=" << completed_frames_ << " "
                << "dropped=" << dropped_frames_ << " "
                << "decoder_errors=" << decoder_errors_ << " "
                << "sync=" << (synchronized_ ? 1 : 0) << " "
                << "rendered=" << renderer_.rendered_frames()
                << " "
                << "decoded_fps="
                << renderer_stats.decoded_frames << " "
                << "last_frame_age_ms=" << last_frame_age_ms()
                << " "
                << "max_frame_gap_ms=" << maximum_frame_gap_ms()
                << std::endl;
            stats_bytes = received_bytes;
            stats_highest = highest_packet_sequence;
            stats_unique = unique_packets;
            stats_at = now;
            next_stats = now + std::chrono::seconds(1);
        }
    }
}

void ReceiverApp::handle_frame(RecoveredFrame frame) {
    if (stream_epoch_ != 0 &&
        frame.stream_epoch < stream_epoch_) {
        ++dropped_frames_;
        return;
    }

    if (frame.stream_epoch != stream_epoch_) {
        stream_epoch_ = frame.stream_epoch;
        synchronized_ = false;
        keyframe_request_pending_ = true;
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
        keyframe_request_pending_ = true;
    }

    if (!synchronized_ && !frame.keyframe) {
        ++dropped_frames_;
        keyframe_request_pending_ = true;
        return;
    }

    if (!synchronized_ && frame.keyframe) {
        decoder_.reset();
    }
    const uint64_t display_start =
        latency_config_.metric ==
                LatencyMetric::SourceToDisplay
            ? source_timestamp(frame)
            : 0;
    uint64_t decoded_at_unix_us = 0;
    if (!decoder_.decode(
            frame.data.data(), frame.data.size(),
            display_start, latency_generation_,
            &decoded_at_unix_us)) {
        ++decoder_errors_;
        synchronized_ = false;
        keyframe_request_pending_ = true;
        return;
    }
    if (latency_config_.metric ==
        LatencyMetric::EncodeToDecode) {
        latency_stats_.record(
            latency_generation_,
            frame.encoded_at_unix_us,
            decoded_at_unix_us,
            monotonic_us());
    }

    synchronized_ = true;
    keyframe_request_pending_ = false;
    have_decoded_frame_id_ = true;
    last_decoded_frame_id_ = frame.frame_id;
    ++completed_frames_;
    const auto now = std::chrono::steady_clock::now();
    const auto previous =
        last_complete_frame_at_.time_since_epoch().count() == 0
            ? receiver_started_at_
            : last_complete_frame_at_;
    const uint64_t gap_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - previous).count());
    maximum_complete_frame_gap_ms_ =
        std::max(maximum_complete_frame_gap_ms_, gap_ms);
    last_complete_frame_at_ = now;

    if (write_h264_ && !output_started_ && frame.keyframe) {
        output_started_ = true;
    }
    if (write_h264_ && output_started_) {
        output_.write(
            reinterpret_cast<const char *>(frame.data.data()),
            static_cast<std::streamsize>(frame.data.size()));
        output_.flush();
    }
}

void ReceiverApp::reset_media_session() {
    assembler_.reset();
    renderer_.discard_pending();
    latency_generation_ = latency_stats_.reset();
    synchronized_ = false;
    keyframe_request_pending_ = true;
    have_decoded_frame_id_ = false;
    stream_epoch_ = 0;
    receiver_started_at_ = std::chrono::steady_clock::now();
    last_complete_frame_at_ = {};
    decoder_.reset();
}

uint32_t ReceiverApp::last_frame_age_ms() const {
    const auto reference =
        last_complete_frame_at_.time_since_epoch().count() == 0
            ? receiver_started_at_
            : last_complete_frame_at_;
    const auto age = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - reference).count();
    return static_cast<uint32_t>(
        std::clamp<int64_t>(
            age, 0, static_cast<int64_t>(UINT32_MAX)));
}

uint64_t ReceiverApp::maximum_frame_gap_ms() const {
    return std::max<uint64_t>(
        maximum_complete_frame_gap_ms_,
        last_frame_age_ms());
}
