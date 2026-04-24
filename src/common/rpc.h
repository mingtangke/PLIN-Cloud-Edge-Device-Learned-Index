#pragma once
// Minimal length-prefixed RPC framing. Implementation deferred to M2.
#include <cstdint>
#include <string>
#include <vector>

#include "proto.h"

namespace plin::rpc {

// Framed message: [u32 length_be][u8 msg_type][body]. length counts msg_type + body.

struct Frame {
    proto::MsgType type;
    std::vector<uint8_t> body;
};

// Blocking read / write of a single frame on a connected TCP socket fd.
// Return false on EOF or socket error. Implemented in M2.
bool read_frame(int fd, Frame& out);
bool write_frame(int fd, const Frame& f);

}  // namespace plin::rpc
