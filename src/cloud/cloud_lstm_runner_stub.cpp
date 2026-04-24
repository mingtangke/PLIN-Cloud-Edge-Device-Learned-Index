#include "cloud_lstm_runner.h"

#include <algorithm>
#include <iostream>
#include <string>

CloudLstmRunner::CloudLstmRunner(const std::string& model_path) {
    std::cout << "[cloud_lstm_runner] stub (no libtorch); model_path="
              << model_path << std::endl;
}

std::vector<int> CloudLstmRunner::predict_top_ends(
    const std::vector<std::vector<float>>& counts_60x10, int k) {
    if (counts_60x10.empty() || k <= 0) return {};
    std::vector<std::pair<float, int>> scores;
    int dims = static_cast<int>(counts_60x10.front().size());
    for (int i = 0; i < dims; ++i) scores.push_back({0.0f, i});
    for (const auto& row : counts_60x10) {
        for (int i = 0; i < static_cast<int>(row.size()) && i < dims; ++i) {
            scores[static_cast<size_t>(i)].first += row[static_cast<size_t>(i)];
        }
    }
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (int i = 0; i < k && i < static_cast<int>(scores.size()); ++i) {
        out.push_back(scores[static_cast<size_t>(i)].second);
    }
    return out;
}

void cloud_lstm_runner_init(const std::string& model_path) {
    std::cout << "[cloud_lstm_runner] stub (no libtorch); model_path="
              << model_path << std::endl;
}
