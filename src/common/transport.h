#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "rpc.h"

namespace plin::transport {

enum class Mode {
    TCP,
    RDMA,
    AUTO,
};

struct RegisteredMemory {
    uint64_t addr = 0;
    uint32_t rkey = 0;
    size_t size = 0;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual const char* name() const = 0;
    virtual bool read_frame(rpc::Frame& out, int timeout_ms = -1) = 0;
    virtual bool write_frame(const rpc::Frame& f) = 0;
    virtual void close() = 0;

    virtual bool supports_remote_read() const { return false; }
    virtual bool register_memory(const void* addr, size_t size, RegisteredMemory& out) {
        (void)addr;
        (void)size;
        (void)out;
        return false;
    }
    virtual bool read_remote(uint64_t remote_addr, uint32_t rkey, void* dst, size_t size) {
        (void)remote_addr;
        (void)rkey;
        (void)dst;
        (void)size;
        return false;
    }
};

Mode parse_mode(const std::string& raw);
const char* mode_name(Mode mode);
uint16_t rdma_port_for(uint16_t tcp_port, int offset);

int listen_tcp(uint16_t port, int backlog = 16);
std::unique_ptr<Transport> accept_tcp(int listen_fd);
std::unique_ptr<Transport> connect_tcp(const std::string& host, uint16_t port);

std::unique_ptr<Transport> connect_transport(const std::string& host,
                                             uint16_t tcp_port,
                                             Mode mode,
                                             int rdma_port_offset,
                                             std::string* err = nullptr);

}  // namespace plin::transport
