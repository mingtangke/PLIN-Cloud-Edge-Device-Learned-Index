#pragma once
// Device-side hot-key cache backed by libcuckoo cuckoohash_map.
// Populated from Cloud HOT_UPDATE and End-local LSTM predictions.
#include <cstdint>
#include <vector>
#include <utility>

#include <libcuckoo/cuckoohash_map.hh>

namespace plin::end {

class HotCache {
 public:
    using key_t     = double;
    using payload_t = uint64_t;

    bool find(key_t k, payload_t& out) const {
        return map_.find(k, out);
    }

    void upsert(key_t k, payload_t v) {
        map_.insert_or_assign(k, v);
    }

    void batch_upsert(const std::vector<std::pair<key_t, payload_t>>& kv) {
        for (const auto& [k, v] : kv) {
            map_.insert_or_assign(k, v);
        }
    }

    size_t size() const { return map_.size(); }
    void clear() { map_.clear(); }

 private:
    mutable libcuckoo::cuckoohash_map<key_t, payload_t> map_;
};

}  // namespace plin::end
