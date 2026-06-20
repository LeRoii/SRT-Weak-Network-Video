#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using UdpSocketHandle = SOCKET;
using UdpSockLen = int;
#else
#include <sys/socket.h>
using UdpSocketHandle = int;
using UdpSockLen = socklen_t;
#endif

enum class UdpResult {
    Data,
    WouldBlock,
    Closed,
};

struct UdpPeer {
    sockaddr_storage address{};
    UdpSockLen address_size = 0;
    bool valid = false;
};

class UdpSocket {
public:
    UdpSocket() = default;
    explicit UdpSocket(UdpSocketHandle descriptor);
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
    UdpSocketHandle descriptor_
#ifdef _WIN32
        = INVALID_SOCKET;
#else
        = -1;
#endif
};
