#pragma once
// Topology + key-range lookup. Loaded from src/common/topology.yaml at startup.
// Implementation deferred to M2.
#include <cstdint>
#include <string>
#include <vector>

namespace plin {

// Matches _key_t = double from parameters.h
using key_t = double;

struct EndInfo {
    int id;
    int edge_id;
    std::string host;
    uint16_t port;
    key_t key_lo;
    key_t key_hi;  // inclusive
};

struct EdgeInfo {
    int id;
    std::string host;
    uint16_t port;
    std::vector<int> end_ids;
};

struct CloudInfo {
    std::string host;
    uint16_t port;
};

class RangeMap {
 public:
    // Load topology from yaml file. Returns false on parse error.
    bool load(const std::string& yaml_path);

    const CloudInfo& cloud() const { return cloud_; }
    const std::vector<EdgeInfo>& edges() const { return edges_; }
    const std::vector<EndInfo>& ends() const { return ends_; }

    // Returns end_id whose key range contains k, or -1 if none.
    int locate_end(key_t k) const;
    int edge_of(int end_id) const;
    std::vector<int> siblings_of(int end_id) const;  // same edge, excluding self
    bool same_edge(int end_a, int end_b) const;

 private:
    CloudInfo cloud_;
    std::vector<EdgeInfo> edges_;
    std::vector<EndInfo> ends_;
};

}  // namespace plin
