#include "transport/srt_transport.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <stdexcept>
#include <syslog.h>
#include <utility>

namespace {

template <typename T>
void set_option(SRTSOCKET socket, SRT_SOCKOPT option,
                const T &value, const char *name) {
    if (srt_setsockflag(socket, option, &value, sizeof(value)) == SRT_ERROR) {
        throw std::runtime_error(
            std::string("srt_setsockflag(") + name + ") failed: " +
            srt_last_error());
    }
}

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
            "cannot resolve endpoint " + endpoint.host + ":" + port +
            ": " + gai_strerror(error));
    }

    sockaddr_storage address{};
    std::memcpy(&address, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return address;
}

} // namespace

SrtRuntime::SrtRuntime() {
    if (srt_startup() != 0) {
        throw std::runtime_error("srt_startup failed");
    }
    srt_setloglevel(LOG_ERR);
}

SrtRuntime::~SrtRuntime() {
    srt_cleanup();
}

SrtSocket::SrtSocket(SRTSOCKET socket) : socket_(socket) {}

SrtSocket::~SrtSocket() {
    close();
}

SrtSocket::SrtSocket(SrtSocket &&other) noexcept
    : socket_(std::exchange(other.socket_, SRT_INVALID_SOCK)) {}

SrtSocket &SrtSocket::operator=(SrtSocket &&other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, SRT_INVALID_SOCK);
    }
    return *this;
}

SRTSOCKET SrtSocket::create_configured_socket() {
    const SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        throw std::runtime_error("srt_create_socket failed: " +
                                 srt_last_error());
    }

    try {
        const SRT_TRANSTYPE transport_type = SRTT_LIVE;
        const bool enabled = true;
        const bool disabled = false;
        const int latency_ms = 350;
        const int timeout_ms = 200;
        const int idle_timeout_ms = 5000;
        const int payload_size = 1200;
        const int send_drop_delay_ms = 0;
        const int flow_control_packets = 512;
        const int send_buffer_bytes = 524'288;
        const int64_t max_bandwidth_bytes_per_second = 375'000;
        set_option(socket, SRTO_TRANSTYPE, transport_type, "TRANSTYPE");
        set_option(socket, SRTO_MESSAGEAPI, enabled, "MESSAGEAPI");
        set_option(socket, SRTO_TSBPDMODE, enabled, "TSBPDMODE");
        set_option(socket, SRTO_TLPKTDROP, enabled, "TLPKTDROP");
        set_option(socket, SRTO_NAKREPORT, disabled, "NAKREPORT");
        set_option(socket, SRTO_LATENCY, latency_ms, "LATENCY");
        set_option(socket, SRTO_RCVTIMEO, timeout_ms, "RCVTIMEO");
        set_option(socket, SRTO_SNDTIMEO, timeout_ms, "SNDTIMEO");
        set_option(socket, SRTO_PEERIDLETIMEO, idle_timeout_ms,
                   "PEERIDLETIMEO");
        set_option(socket, SRTO_PAYLOADSIZE, payload_size, "PAYLOADSIZE");
        set_option(socket, SRTO_SNDDROPDELAY, send_drop_delay_ms,
                   "SNDDROPDELAY");
        set_option(socket, SRTO_FC, flow_control_packets, "FC");
        set_option(socket, SRTO_SNDBUF, send_buffer_bytes, "SNDBUF");
        set_option(socket, SRTO_MAXBW, max_bandwidth_bytes_per_second,
                   "MAXBW");
    } catch (...) {
        srt_close(socket);
        throw;
    }
    return socket;
}

void SrtSocket::configure_connected_socket(SRTSOCKET socket) {
    const int timeout_ms = 200;
    const bool asynchronous = false;
    set_option(socket, SRTO_RCVTIMEO, timeout_ms, "RCVTIMEO");
    set_option(socket, SRTO_SNDTIMEO, timeout_ms, "SNDTIMEO");
    set_option(socket, SRTO_SNDSYN, asynchronous, "SNDSYN");
    set_option(socket, SRTO_RCVSYN, asynchronous, "RCVSYN");
}

SrtSocket SrtSocket::create_listener(const Endpoint &endpoint) {
    const SRTSOCKET socket = create_configured_socket();
    try {
        const bool reuse = true;
        set_option(socket, SRTO_REUSEADDR, reuse, "REUSEADDR");
        const sockaddr_storage address = resolve(endpoint, AI_PASSIVE);
        if (srt_bind(socket, reinterpret_cast<const sockaddr *>(&address),
                     sizeof(sockaddr_in)) == SRT_ERROR) {
            throw std::runtime_error("srt_bind failed: " + srt_last_error());
        }
        if (srt_listen(socket, 1) == SRT_ERROR) {
            throw std::runtime_error("srt_listen failed: " + srt_last_error());
        }
        return SrtSocket(socket);
    } catch (...) {
        srt_close(socket);
        throw;
    }
}

SrtSocket SrtSocket::connect_to(const Endpoint &endpoint) {
    const SRTSOCKET socket = create_configured_socket();
    try {
        const sockaddr_storage address = resolve(endpoint, 0);
        if (srt_connect(socket, reinterpret_cast<const sockaddr *>(&address),
                        sizeof(sockaddr_in)) == SRT_ERROR) {
            throw std::runtime_error("srt_connect failed: " +
                                     srt_last_error());
        }
        configure_connected_socket(socket);
        return SrtSocket(socket);
    } catch (...) {
        srt_close(socket);
        throw;
    }
}

SrtSocket SrtSocket::accept() {
    sockaddr_storage address{};
    int address_size = sizeof(address);
    const SRTSOCKET accepted =
        srt_accept(socket_, reinterpret_cast<sockaddr *>(&address),
                   &address_size);
    if (accepted == SRT_INVALID_SOCK) {
        throw std::runtime_error("srt_accept failed: " + srt_last_error());
    }
    configure_connected_socket(accepted);
    return SrtSocket(accepted);
}

SendResult SrtSocket::send(const std::vector<uint8_t> &message, int ttl_ms) {
    if (!valid() || message.empty()) {
        return SendResult::Closed;
    }
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgttl = ttl_ms;
    control.inorder = 0;
    const int sent = srt_sendmsg2(
        socket_, reinterpret_cast<const char *>(message.data()),
        static_cast<int>(message.size()), &control);
    if (sent == static_cast<int>(message.size())) {
        return SendResult::Sent;
    }
    const int error = srt_getlasterror(nullptr);
    if (error == SRT_EASYNCSND || error == SRT_ETIMEOUT) {
        return SendResult::WouldBlock;
    }
    return SendResult::Closed;
}

ReceiveResult SrtSocket::receive(std::vector<uint8_t> &message) {
    message.resize(2048);
    SRT_MSGCTRL control = srt_msgctrl_default;
    const int received = srt_recvmsg2(
        socket_, reinterpret_cast<char *>(message.data()),
        static_cast<int>(message.size()), &control);
    if (received >= 0) {
        message.resize(static_cast<std::size_t>(received));
        return ReceiveResult::Data;
    }

    const int error = srt_getlasterror(nullptr);
    message.clear();
    if (error == SRT_ETIMEOUT || error == SRT_EASYNCRCV) {
        return ReceiveResult::Timeout;
    }
    return ReceiveResult::Closed;
}

NetworkSnapshot SrtSocket::network_snapshot(bool clear_interval) {
    NetworkSnapshot result;
    if (!valid()) {
        return result;
    }

    SRT_TRACEBSTATS stats{};
    if (srt_bstats(socket_, &stats, clear_interval ? 1 : 0) ==
        SRT_ERROR) {
        return result;
    }
    const double expected =
        static_cast<double>(stats.pktSent + stats.pktSndLoss);
    result.loss_percent =
        expected > 0.0 ? stats.pktSndLoss * 100.0 / expected : 0.0;
    result.rtt_ms = stats.msRTT;
    result.bandwidth_kbps = stats.mbpsBandwidth * 1000.0;
    result.sent_packets = static_cast<uint64_t>(
        std::max<int64_t>(0, stats.pktSent));
    result.retransmitted_packets = static_cast<uint64_t>(
        std::max(0, stats.pktRetrans));
    result.valid = true;
    return result;
}

bool SrtSocket::valid() const {
    return socket_ != SRT_INVALID_SOCK;
}

void SrtSocket::close() {
    if (valid()) {
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
    }
}

std::string srt_last_error() {
    return srt_getlasterror_str();
}
