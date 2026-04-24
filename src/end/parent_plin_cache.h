#pragma once
// Parent Edge's PLIN parameter replica on End.
// Receives serialised Param[][] via PLIN_PARAM_PUSH, then predicts B+ leaf slot
// for any key so End can send EDGE_FETCH_REQ with the predicted position.
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <sstream>
#include <vector>

#include "cache_model.h"   // Param, local_model::predict_pos
#include "serialize.h"     // deserialize_parameter

namespace plin::end {

class ParentPlinCache {
 public:
    using key_t = double;

    bool loaded() const {
        std::shared_lock lk(mu_);
        return !model_.parameter.empty();
    }

    // Called when PLIN_PARAM_PUSH frame body arrives.
    void load_from_push(const uint8_t* data, size_t n) {
        std::istringstream is(std::string(reinterpret_cast<const char*>(data), n));
        std::vector<std::vector<Param>> params;
        deserialize_parameter(params, is);
        std::unique_lock lk(mu_);
        model_.parameter = std::move(params);
    }

    // Returns predicted leaf slot, or -1 if not loaded.
    int predict_pos(key_t k) const {
        std::shared_lock lk(mu_);
        if (model_.parameter.empty()) return -1;
        return model_.predict_pos(static_cast<_key_t>(k));
    }

 private:
    mutable std::shared_mutex mu_;
    mutable local_model model_;  // predict_pos is not const in local_model
};

}  // namespace plin::end
