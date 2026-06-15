#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <sys/socket.h>
#include <vector>

enum class UdpResult {
    Data,
    WouldBlock,
    Closed,
};

struct UdpPeer {
    sockaddr_storage address{};
    socklen_t address_size = 0;
    bool valid = false;
};

class UdpSocket {
public:
    UdpSocket() = default;
    explicit UdpSocket(int descriptor);
    ~UdpSocket();

    UdpSocket(UdpSocket &&other) noexcept;
    UdpSocket &operator=(UdpSocket &&other) noexcept;
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    static UdpSocket create_sender(const Endpoint &peer);
    static UdpSocket create_receiver(const Endpoint &local);

    UdpResult send(const std::vector<uint8_t> &message);
    UdpResult send_to(const std::vector<uint8_t> &message,
                      const UdpPeer &peer);
    UdpResult receive(std::vector<uint8_t> &message,
                      UdpPeer *peer = nullptr);
    bool valid() const;
    void close();

private:
    int descriptor_ = -1;
};
