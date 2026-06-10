#include "receiver.hpp"

#include "common/utils.hpp"
#include "protocol/messages.hpp"

#include <chrono>
#include <iostream>
#include <thread>

ReceiverApp::ReceiverApp(Endpoint local,
                         std::string output_file,
                         bool display_enabled)
    : local_(std::move(local)),
      output_(std::move(output_file), std::ios::binary | std::ios::trunc),
      decoder_(renderer_),
      display_enabled_(display_enabled) {
    if (!output_) {
        throw std::runtime_error("cannot open receiver output file");
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
    while (!g_stop_requested.load()) {
        std::vector<uint8_t> message;
        const auto result = socket.receive(message);
        if (result == ReceiveResult::Closed) {
            break;
        }
        if (result == ReceiveResult::Timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (result == ReceiveResult::Data) {
            const auto parsed = parse_message(message.data(), message.size());
            if (parsed && parsed->shard) {
                if (auto frame = assembler_.push(std::move(*parsed->shard))) {
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
            const auto network = socket.network_snapshot(true);
            std::cout << "bandwidth=" << network.bandwidth_kbps << "kbps "
                      << "rtt=" << network.rtt_ms << "ms "
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
        decoder_.reset();
    }

    if (!synchronized_ && !frame.keyframe) {
        ++dropped_frames_;
        return;
    }

    if (!synchronized_ && frame.keyframe) {
        decoder_.reset();
    }
    if (!decoder_.decode(frame.data.data(), frame.data.size())) {
        ++decoder_errors_;
        synchronized_ = false;
        return;
    }

    synchronized_ = true;
    ++completed_frames_;
    if (!output_started_ && frame.keyframe) {
        output_started_ = true;
    }
    if (output_started_) {
        output_.write(reinterpret_cast<const char *>(frame.data.data()),
                      static_cast<std::streamsize>(frame.data.size()));
        output_.flush();
    }
}
