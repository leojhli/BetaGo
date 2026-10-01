#pragma once

#include "network.hpp"
#include "sgf.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace betago {
struct PretrainingSettings {
    int epochs = 20, batch_size = 32;
    double validation_fraction = 0.2;
    std::int64_t seed = 0;
    bool augment = true;
    NetworkSettings network{};
    OptimizerSettings optimizer{};
    void validate() const;
    Json to_json() const;
};
using PretrainingProgress = std::function<void(int epoch, const Json& metrics)>;
std::vector<TrainingExample> expert_examples(const ExpertGame& game);
// Symmetries 0..3 rotate clockwise; 4..7 reflect columns, then rotate.
TrainingExample augment_expert_example(const TrainingExample& example, int symmetry);

// Whole games are deduplicated and split before any symmetry augmentation.
// The output directory must exist; owned output files must not exist. The selected model is
// the smallest held-out inference policy CE + value MSE, including epoch zero.
Json pretrain_expert_games(std::vector<ExpertGame> games, const PretrainingSettings& settings,
    const std::optional<std::filesystem::path>& checkpoint,
    const std::filesystem::path& output_directory, PretrainingProgress progress = {});
} // namespace betago
