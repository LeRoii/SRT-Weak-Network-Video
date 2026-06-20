#include "transport/udp_transport.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <cstdlib>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
constexpr UdpSocketHandle kInvalidSocket = INVALID_SOCKET;
#else
constexpr UdpSocketHandle kInvalidSocket = -1;
#endif

#ifdef _WIN32
struct WinsockRuntime {
    WinsockRuntime() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            throw std::runtime_error(
                "WSAStartup failed: " + std::to_string(error));
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }
};

void ensure_winsock() {
    static WinsockRuntime runtime;
}

std::string socket_error_text() {
    return std::to_string(WSAGetLastError());
}

void close_socket(UdpSocketHandle descriptor) {
    closesocket(descriptor);
}
#else
void ensure_winsock() {}

std::string socket_error_text() {
    return std::strerror(errno);
}

void close_socket(UdpSocketHandle descriptor) {
    ::close(descriptor);
}
#endif

sockaddr_storage resolve(const Endpoint &endpoint, int flags) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = flags;

    addrinfo *result = nullptr;
    const std::string port = std::to_string(endpoint.port);
    const int error = getaddrinfo(
        endpoint.host.empty() ? nullptr : endpoint.host.c_str(),
        port.c_str(), &hints, &result);
    if (error != 0 || !result) {
#ifdef _WIN32
        const std::string error_text = std::to_string(error);
#else
        const std::string error_text = gai_strerror(error);
#endif
        throw std::runtime_error(
            "cannot resolve UDP endpoint " + endpoint.host + ":" + port +
            ": " + error_text);
    }

    sockaddr_storage address{};
    std::memcpy(&address, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return address;
}

void configure_socket(UdpSocketHandle descriptor) {
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(descriptor, FIONBIO, &nonblocking) != 0) {
        throw std::runtime_error(
            "cannot make UDP socket nonblocking: " + socket_error_text());
    }
#else
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(
            "cannot make UDP socket nonblocking: " +
            std::string(std::strerror(errno)));
    }
#endif
    const int buffer_bytes = 4 * 1024 * 1024;
    setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char *>(&buffer_bytes),
               sizeof(buffer_bytes));
    setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char *>(&buffer_bytes),
               sizeof(buffer_bytes));
}

UdpResult result_for_error() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAENOBUFS) {
        return UdpResult::WouldBlock;
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
        return UdpResult::WouldBlock;
    }
#endif
    return UdpResult::Closed;
}

int nonblocking_send_flags() {
#ifdef _WIN32
    return 0;
#else
    return MSG_DONTWAIT;
#endif
}

} // namespace

UdpSocket::UdpSocket(UdpSocketHandle descriptor) : descriptor_(descriptor) {}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, kInvalidSocket)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, kInvalidSocket);
    }
    return *this;
}

UdpSocket UdpSocket::create_sender(const Endpoint &peer) {
    ensure_winsock();
    const UdpSocketHandle descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor == kInvalidSocket) {
        throw std::runtime_error(
            "cannot create UDP socket: " + socket_error_text());
    }
    try {
        configure_socket(descriptor);
        const sockaddr_storage address = resolve(peer, 0);
        if (connect(descriptor,
                    reinterpret_cast<const sockaddr *>(&address),
                    sizeof(sockaddr_in)) < 0) {
            throw std::runtime_error(
                "cannot connect UDP socket: " + socket_error_text());
        }
        return UdpSocket(descriptor);
    } catch (...) {
        close_socket(descriptor);
        throw;
    }
}

UdpSocket UdpSocket::create_receiver(const Endpoint &local) {
    ensure_winsock();
    const UdpSocketHandle descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor == kInvalidSocket) {
        throw std::runtime_error(
            "cannot create UDP socket: " + socket_error_text());
    }
    try {
        configure_socket(descriptor);
        const int reuse = 1;
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        const sockaddr_storage address = resolve(local, AI_PASSIVE);
        if (bind(descriptor,
                 reinterpret_cast<const sockaddr *>(&address),
                 sizeof(sockaddr_in)) < 0) {
            throw std::runtime_error(
                "cannot bind UDP socket: " + socket_error_text());
        }
        return UdpSocket(descriptor);
    } catch (...) {
        close_socket(descriptor);
        throw;
    }
}

UdpResult UdpSocket::send(const std::vector<uint8_t> &message) {
    if (!valid() || message.empty()) {
        return UdpResult::Closed;
    }
    const int sent = ::send(
        descriptor_, reinterpret_cast<const char *>(message.data()),
        static_cast<int>(message.size()), nonblocking_send_flags());
    if (sent == static_cast<int>(message.size())) {
        return UdpResult::Data;
    }
    return result_for_error();
}

UdpResult UdpSocket::send_to(const std::vector<uint8_t> &message,
                             const UdpPeer &peer) {
    if (!valid() || !peer.valid || message.empty()) {
        return UdpResult::Closed;
    }
    const int sent = sendto(
        descriptor_, reinterpret_cast<const char *>(message.data()),
        static_cast<int>(message.size()), nonblocking_send_flags(),
        reinterpret_cast<const sockaddr *>(&peer.address),
        peer.address_size);
    if (sent == static_cast<int>(message.size())) {
        return UdpResult::Data;
    }
    return result_for_error();
}

UdpResult UdpSocket::receive(std::vector<uint8_t> &message,
                             UdpPeer *peer) {
    message.resize(2048);
    sockaddr_storage address{};
    UdpSockLen address_size = sizeof(address);
    const int received = recvfrom(
        descriptor_, reinterpret_cast<char *>(message.data()),
        static_cast<int>(message.size()), nonblocking_send_flags(),
        reinterpret_cast<sockaddr *>(&address), &address_size);
    if (received >= 0) {
        message.resize(static_cast<std::size_t>(received));
        if (peer) {
            peer->address = address;
            peer->address_size = address_size;
            peer->valid = true;
        }
        return UdpResult::Data;
    }
    message.clear();
    return result_for_error();
}

bool UdpSocket::valid() const {
    return descriptor_ != kInvalidSocket;
}

void UdpSocket::close() {
    if (valid()) {
        close_socket(descriptor_);
        descriptor_ = kInvalidSocket;
    }
}
