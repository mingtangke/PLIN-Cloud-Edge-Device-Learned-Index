#include "cloud_lstm_runner.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <mutex>

#include <torch/script.h>

namespace {
torch::jit::Module g_module;
std::mutex g_mu;
}  // namespace

CloudLstmRunner::CloudLstmRunner(const std::string& model_path) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_module = torch::jit::load(model_path);
    g_module.eval();
    std::cout << "[cloud_lstm_runner] loaded " << model_path << std::endl;
}

std::vector<int> CloudLstmRunner::predict_top_ends(
    const std::vector<std::vector<float>>& counts_60x10, int k) {
    if (counts_60x10.empty() || k <= 0) return {};
    const int64_t steps = static_cast<int64_t>(counts_60x10.size());
    const int64_t dims = static_cast<int64_t>(counts_60x10.front().size());
    if (dims <= 0) return {};

    std::vector<float> flat;
    flat.reserve(static_cast<size_t>(steps * dims));
    for (const auto& row : counts_60x10) {
        if (static_cast<int64_t>(row.size()) != dims) return {};
        flat.insert(flat.end(), row.begin(), row.end());
    }

    k = std::min<int>(k, static_cast<int>(dims));
    auto opts = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU);
    torch::Tensor x = torch::from_blob(flat.data(), {1, steps, dims}, opts).clone();

    std::lock_guard<std::mutex> lk(g_mu);
    torch::NoGradGuard no_grad;
    torch::Tensor indices = g_module.get_method("predict_top_k")({x, k}).toTensor().to(torch::kCPU);
    std::vector<int> result;
    result.reserve(static_cast<size_t>(k));
    auto acc = indices.accessor<int64_t, 2>();
    for (int i = 0; i < k; ++i) {
        result.push_back(static_cast<int>(acc[0][i]));
    }
    return result;
}

void cloud_lstm_runner_init(const std::string& model_path) {
    try {
        CloudLstmRunner runner(model_path);
        std::cout << "[cloud_lstm_runner] loaded " << model_path << std::endl;
    } catch (const c10::Error& e) {
        std::cerr << "[cloud_lstm_runner] failed to load " << model_path
                  << ": " << e.what() << std::endl;
    }
}
