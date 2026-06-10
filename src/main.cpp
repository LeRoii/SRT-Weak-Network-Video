#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "receiver.hpp"
#include "sender.hpp"
#include "transport/srt_transport.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char *program) {
    std::cerr
        << "Usage:\n"
        << "  " << program
        << " --role sender --connect <host:port> --video-file <path>"
           " [--max-video-kbps <8-2000>] [--config <path>]\n"
        << "  " << program
        << " --role receiver --listen <host:port>"
           " [--output-file <path>] [--no-display] [--config <path>]\n";
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        std::optional<Role> role;
        std::optional<Endpoint> endpoint;
        std::string video_file;
        std::string output_file = "received.h264";
        int max_video_kbps = 2000;
        bool display_enabled = true;
        std::optional<std::filesystem::path> config_path;

        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--role" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "sender") {
                    role = Role::Sender;
                } else if (value == "receiver") {
                    role = Role::Receiver;
                } else {
                    throw std::runtime_error(
                        "--role must be sender or receiver");
                }
            } else if ((argument == "--connect" ||
                        argument == "--listen") &&
                       i + 1 < argc) {
                endpoint = parse_endpoint(argv[++i]);
            } else if (argument == "--video-file" && i + 1 < argc) {
                video_file = argv[++i];
            } else if (argument == "--output-file" && i + 1 < argc) {
                output_file = argv[++i];
            } else if (argument == "--max-video-kbps" && i + 1 < argc) {
                max_video_kbps = std::stoi(argv[++i]);
                if (max_video_kbps < 8 || max_video_kbps > 2000) {
                    throw std::runtime_error(
                        "--max-video-kbps must be between 8 and 2000");
                }
            } else if (argument == "--no-display") {
                display_enabled = false;
            } else if (argument == "--config" && i + 1 < argc) {
                config_path = argv[++i];
            } else if (argument == "--help" || argument == "-h") {
                print_usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error(
                    "unknown or incomplete argument: " + argument);
            }
        }

        if (!role || !endpoint) {
            print_usage(argv[0]);
            return 1;
        }
        if (*role == Role::Sender && video_file.empty()) {
            throw std::runtime_error(
                "--video-file is required for sender");
        }

        SrtRuntime runtime;
        const auto effective_config_path =
            config_path.value_or(default_runtime_config_path());
        const auto config = load_runtime_config(
            effective_config_path, !config_path.has_value());
        if (*role == Role::Sender) {
            SenderApp(*endpoint, video_file, max_video_kbps).run();
        } else {
            ReceiverApp(*endpoint, output_file, display_enabled,
                        config.latency).run();
        }
    } catch (const std::exception &error) {
        std::cerr << "fatal: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
