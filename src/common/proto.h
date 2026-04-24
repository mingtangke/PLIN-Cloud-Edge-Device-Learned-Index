#pragma once
#include <cstdint>

namespace plin::proto {

enum class MsgType : uint8_t {
    QUERY_REQ        = 1,
    QUERY_RESP       = 2,
    EDGE_FETCH_REQ   = 3,
    EDGE_FETCH_RESP  = 4,
    PLIN_PARAM_PUSH  = 5,
    HOT_UPDATE       = 6,
    LSTM_TRAIN_TRIGGER = 7,
    CROSS_EDGE_REQ   = 8,
    HEARTBEAT        = 9,
    HEARTBEAT_ACK    = 10,
    RDMA_SNAPSHOT_INFO = 11,
};

enum class Status : uint8_t {
    OK                = 0,
    NOT_FOUND         = 1,
    PARAM_STALE       = 2,
    NOT_IMPLEMENTED   = 3,
    ERROR             = 255,
};

}  // namespace plin::proto
