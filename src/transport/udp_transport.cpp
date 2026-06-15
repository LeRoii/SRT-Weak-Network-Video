#include "transport/udp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

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
        throw std::runtime_error(
            "cannot resolve UDP endpoint " + endpoint.host + ":" + port +
            ": " + gai_strerror(error));
    }

    sockaddr_storage address{};
    std::memcpy(&address, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return address;
}

void configure_socket(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(
            "cannot make UDP socket nonblocking: " +
            std::string(std::strerror(errno)));
    }
    const int buffer_bytes = 4 * 1024 * 1024;
    setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF,
               &buffer_bytes, sizeof(buffer_bytes));
    setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF,
               &buffer_bytes, sizeof(buffer_bytes));
}

UdpResult result_for_error() {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
        return UdpResult::WouldBlock;
    }
    return UdpResult::Closed;
}

} // namespace

UdpSocket::UdpSocket(int descriptor) : descriptor_(descriptor) {}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

UdpSocket UdpSocket::create_sender(const Endpoint &peer) {
    const int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create UDP socket: " +
            std::string(std::strerror(errno)));
    }
    try {
        configure_socket(descriptor);
        const sockaddr_storage address = resolve(peer, 0);
        if (connect(descriptor,
                    reinterpret_cast<const sockaddr *>(&address),
                    sizeof(sockaddr_in)) < 0) {
            throw std::runtime_error(
                "cannot connect UDP socket: " +
                std::string(std::strerror(errno)));
        }
        return UdpSocket(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
}

UdpSocket UdpSocket::create_receiver(const Endpoint &local) {
    const int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create UDP socket: " +
            std::string(std::strerror(errno)));
    }
    try {
        configure_socket(descriptor);
        const int reuse = 1;
        setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse));
        const sockaddr_storage address = resolve(local, AI_PASSIVE);
        if (bind(descriptor,
                 reinterpret_cast<const sockaddr *>(&address),
                 sizeof(sockaddr_in)) < 0) {
            throw std::runtime_error(
                "cannot bind UDP socket: " +
                std::string(std::strerror(errno)));
        }
        return UdpSocket(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
}

UdpResult UdpSocket::send(const std::vector<uint8_t> &message) {
    if (!valid() || message.empty()) {
        return UdpResult::Closed;
    }
    const ssize_t sent = ::send(
        descriptor_, message.data(), message.size(), MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(message.size())) {
        return UdpResult::Data;
    }
    return result_for_error();
}

UdpResult UdpSocket::send_to(const std::vector<uint8_t> &message,
                             const UdpPeer &peer) {
    if (!valid() || !peer.valid || message.empty()) {
        return UdpResult::Closed;
    }
    const ssize_t sent = sendto(
        descriptor_, message.data(), message.size(), MSG_DONTWAIT,
        reinterpret_cast<const sockaddr *>(&peer.address),
        peer.address_size);
    if (sent == static_cast<ssize_t>(message.size())) {
        return UdpResult::Data;
    }
    return result_for_error();
}

UdpResult UdpSocket::receive(std::vector<uint8_t> &message,
                             UdpPeer *peer) {
    message.resize(2048);
    sockaddr_storage address{};
    socklen_t address_size = sizeof(address);
    const ssize_t received = recvfrom(
        descriptor_, message.data(), message.size(), MSG_DONTWAIT,
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
    return descriptor_ >= 0;
}

void UdpSocket::close() {
    if (valid()) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}
