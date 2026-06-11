#include "common/runtime_config.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "receiver.hpp"
#include "sender.hpp"
#include "transport/srt_transport.hpp"

#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char *program) {
    std::cerr
        << "Usage: " << program << " --role <sender|receiver>\n";
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        std::optional<Role> role;

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
            } else if (argument == "--help" || argument == "-h") {
                print_usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error(
                    "unknown or incomplete argument: " + argument);
            }
        }

        if (!role) {
            print_usage(argv[0]);
            return 1;
        }

        SrtRuntime runtime;
        const auto config = load_runtime_config(
            default_runtime_config_path(), true);
        if (*role == Role::Sender) {
            SenderApp(parse_endpoint(config.sender.connect),
                      config.sender.video_file,
                      config.sender.max_video_kbps).run();
        } else {
            ReceiverApp(parse_endpoint(config.receiver.listen),
                        config.receiver.output_file,
                        config.receiver.display,
                        config.receiver.write_h264,
                        config.latency).run();
        }
    } catch (const std::exception &error) {
        std::cerr << "fatal: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
