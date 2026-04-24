#pragma once
// Minimal stub satisfying plin_index.h's DatabaseLogger usage.
// Full implementation lives in legacy/hot_key.h (edge does not run Python LSTM).
#include <string>
#include "parameters.h"

class DatabaseLogger {
public:
    _key_t* keys = nullptr;
    DatabaseLogger(const std::string& /*log_path*/,
                   const std::string& /*host*/,
                   int /*port*/) {}
    void start() {}
};
