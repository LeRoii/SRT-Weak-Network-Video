#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <srt.h>

enum class ReceiveResult {
    Data,
    Timeout,
    Closed,
};

enum class SendResult {
    Sent,
    WouldBlock,
    Closed,
};

class SrtRuntime {
public:
    SrtRuntime();
    ~SrtRuntime();

    SrtRuntime(const SrtRuntime &) = delete;
    SrtRuntime &operator=(const SrtRuntime &) = delete;
};

class SrtSocket {
public:
    SrtSocket() = default;
    explicit SrtSocket(SRTSOCKET socket);
    ~SrtSocket();

    SrtSocket(SrtSocket &&other) noexcept;
    SrtSocket &operator=(SrtSocket &&other) noexcept;
    SrtSocket(const SrtSocket &) = delete;
    SrtSocket &operator=(const SrtSocket &) = delete;

    static SrtSocket create_listener(const Endpoint &endpoint);
    static SrtSocket connect_to(const Endpoint &endpoint);

    SrtSocket accept();
    SendResult send(const std::vector<uint8_t> &message, int ttl_ms = 320);
    ReceiveResult receive(std::vector<uint8_t> &message);
    NetworkSnapshot network_snapshot(bool clear_interval);
    bool valid() const;
    void close();

private:
    static SRTSOCKET create_configured_socket();
    static void configure_connected_socket(SRTSOCKET socket);

    SRTSOCKET socket_ = SRT_INVALID_SOCK;
};

std::string srt_last_error();
