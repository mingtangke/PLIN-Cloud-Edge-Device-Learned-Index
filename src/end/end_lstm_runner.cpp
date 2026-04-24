// libtorch-backed end LSTM runner. Real implementation arrives in M5.
#include <iostream>
#include <string>

#include <torch/script.h>

namespace {
torch::jit::Module g_module;
bool g_loaded = false;
}  // namespace

void end_lstm_runner_init(const std::string& model_path) {
    try {
        g_module = torch::jit::load(model_path);
        g_loaded = true;
        std::cout << "[end_lstm_runner] loaded " << model_path << std::endl;
    } catch (const c10::Error& e) {
        g_loaded = false;
        std::cerr << "[end_lstm_runner] failed to load " << model_path
                  << ": " << e.what() << std::endl;
    }
}
