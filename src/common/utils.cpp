#include "common/utils.hpp"

#include <chrono>
#include <stdexcept>

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true);
}

int64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t unix_time_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

Endpoint parse_endpoint(const std::string &text) {
    const auto separator = text.rfind(':');
    if (separator == std::string::npos ||
        separator == 0 ||
        separator + 1 >= text.size()) {
        throw std::runtime_error("endpoint must be host:port");
    }

    const int port = std::stoi(text.substr(separator + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port is out of range");
    }
    return {text.substr(0, separator), static_cast<uint16_t>(port)};
}
