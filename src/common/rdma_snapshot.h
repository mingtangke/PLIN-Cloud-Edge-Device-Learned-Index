#pragma once

#include <cstdint>

namespace plin::rdma {

constexpr uint32_t kSnapshotMagic = 0x504c5244;  // PLRD
constexpr uint32_t kSnapshotVersion = 1;

struct SnapshotInfo {
    uint32_t magic = kSnapshotMagic;
    uint32_t version = kSnapshotVersion;
    uint64_t desc_addr = 0;
    uint32_t desc_rkey = 0;
    uint32_t leaf_count = 0;
    uint64_t record_addr = 0;
    uint32_t record_rkey = 0;
    uint64_t record_count = 0;
};

struct LeafDescriptor {
    uint64_t offset = 0;
    uint32_t count = 0;
    uint32_t version = kSnapshotVersion;
    double first_key = 0;
};

struct KeyPayloadRecord {
    double key = 0;
    uint64_t payload = 0;
};

}  // namespace plin::rdma
