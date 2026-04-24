#include "rpc.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace plin::rpc {

namespace {

// Fully read exactly n bytes into buf. Returns false on EOF or error.
bool read_all(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Fully write exactly n bytes from buf. Returns false on error.
bool write_all(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

bool read_frame(int fd, Frame& out) {
    // Header: 4-byte big-endian length (type + body), 1-byte msg_type.
    uint32_t len_be = 0;
    if (!read_all(fd, &len_be, 4)) return false;
    uint32_t payload_len = ntohl(len_be);
    if (payload_len < 1) return false;  // must have at least msg_type byte

    uint8_t type_byte = 0;
    if (!read_all(fd, &type_byte, 1)) return false;
    out.type = static_cast<proto::MsgType>(type_byte);

    uint32_t body_len = payload_len - 1;
    out.body.resize(body_len);
    if (body_len > 0 && !read_all(fd, out.body.data(), body_len)) return false;
    return true;
}

bool write_frame(int fd, const Frame& f) {
    // payload_len = 1 (type byte) + body
    uint32_t payload_len = 1 + static_cast<uint32_t>(f.body.size());
    uint32_t len_be = htonl(payload_len);
    uint8_t type_byte = static_cast<uint8_t>(f.type);

    if (!write_all(fd, &len_be, 4)) return false;
    if (!write_all(fd, &type_byte, 1)) return false;
    if (!f.body.empty() && !write_all(fd, f.body.data(), f.body.size())) return false;
    return true;
}

}  // namespace plin::rpc
