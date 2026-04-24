#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "transport.h"

namespace plin::transport {

class RdmaListener {
public:
    virtual ~RdmaListener() = default;
    virtual std::unique_ptr<Transport> accept(std::string* err = nullptr) = 0;
    virtual void close() = 0;
};

std::unique_ptr<RdmaListener> listen_rdma(uint16_t port, int backlog, std::string* err = nullptr);
std::unique_ptr<Transport> connect_rdma(const std::string& host, uint16_t port, std::string* err = nullptr);

}  // namespace plin::transport
