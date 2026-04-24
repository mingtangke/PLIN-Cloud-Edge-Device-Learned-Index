#include "transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef PLIN_ENABLE_RDMA
#include "rdma_transport.h"
#endif

namespace plin::transport {

namespace {

class TcpTransport final : public Transport {
public:
    explicit TcpTransport(int fd) : fd_(fd) {}
    ~TcpTransport() override { close(); }

    const char* name() const override { return "tcp"; }

    bool read_frame(rpc::Frame& out, int timeout_ms = -1) override {
        if (fd_ < 0) return false;
        if (timeout_ms >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd_, &rfds);
            timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int rc;
            do {
                rc = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
            } while (rc < 0 && errno == EINTR);
            if (rc <= 0) return false;
        }
        return rpc::read_frame(fd_, out);
    }

    bool write_frame(const rpc::Frame& f) override {
        return fd_ >= 0 && rpc::write_frame(fd_, f);
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

}  // namespace

Mode parse_mode(const std::string& raw) {
    if (raw == "tcp") return Mode::TCP;
    if (raw == "rdma") return Mode::RDMA;
    if (raw == "auto") return Mode::AUTO;
    return Mode::AUTO;
}

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::TCP: return "tcp";
        case Mode::RDMA: return "rdma";
        case Mode::AUTO: return "auto";
    }
    return "auto";
}

uint16_t rdma_port_for(uint16_t tcp_port, int offset) {
    int port = static_cast<int>(tcp_port) + offset;
    if (port < 1) port = tcp_port;
    if (port > 65535) port = tcp_port;
    return static_cast<uint16_t>(port);
}

int listen_tcp(uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::unique_ptr<Transport> accept_tcp(int listen_fd) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) return nullptr;
    return std::make_unique<TcpTransport>(fd);
}

std::unique_ptr<Transport> connect_tcp(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return nullptr;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<TcpTransport>(fd);
}

std::unique_ptr<Transport> connect_transport(const std::string& host,
                                             uint16_t tcp_port,
                                             Mode mode,
                                             int rdma_port_offset,
                                             std::string* err) {
    if (mode == Mode::RDMA || mode == Mode::AUTO) {
#ifdef PLIN_ENABLE_RDMA
        std::string rdma_err;
        uint16_t rdma_port = rdma_port_for(tcp_port, rdma_port_offset);
        auto rdma = connect_rdma(host, rdma_port, &rdma_err);
        if (rdma) return rdma;
        if (err) *err = rdma_err;
        if (mode == Mode::RDMA) return nullptr;
#else
        if (err) *err = "RDMA support was not compiled in";
        if (mode == Mode::RDMA) return nullptr;
#endif
    }

    auto tcp = connect_tcp(host, tcp_port);
    if (!tcp && err && err->empty()) {
        *err = std::strerror(errno);
    }
    return tcp;
}

}  // namespace plin::transport
