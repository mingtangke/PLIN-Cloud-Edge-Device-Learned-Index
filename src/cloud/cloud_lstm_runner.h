#pragma once

#include <string>
#include <vector>

class CloudLstmRunner {
 public:
    explicit CloudLstmRunner(const std::string& model_path);

    // counts_60x10: 60 timesteps, each with 10 end access counts.
    // Returns 0-based end indices sorted by predicted probability.
    std::vector<int> predict_top_ends(
        const std::vector<std::vector<float>>& counts_60x10, int k);
};
