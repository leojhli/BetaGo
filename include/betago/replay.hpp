#pragma once

#include "network.hpp"
#include "random.hpp"
#include "runner.hpp"
#include <deque>
#include <filesystem>
#include <functional>
#include <vector>

namespace betago {
// Keep whole completed games, removing the oldest game when capacity is
// exceeded. A move-limit game has no outcome and never supplies labels.
class ReplayBuffer {
public:
    explicit ReplayBuffer(int capacity = 100, int board_size = 9, double komi = 7.5);
    bool add(const Json& selfplay_game);
    std::vector<TrainingExample> examples() const;
    int game_count() const { return static_cast<int>(games_.size()); }
    int capacity() const { return capacity_; }
    int board_size() const { return board_size_; }
    double komi() const { return komi_; }

    Json to_json() const;
    static ReplayBuffer from_json(const Json& data);
    static ReplayBuffer load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

private:
    struct SavedGame {
        Json record;
        std::vector<TrainingExample> examples;
    };
    int capacity_, board_size_;
    double komi_;
    std::deque<SavedGame> games_;
};

struct TrainingSettings {
    int updates = 100;
    int batch_size = 32;
    OptimizerSettings optimizer{0.01, 0.9, 1e-4};
    void validate() const;
    Json to_json() const;
};

// Uniform sampling of individual positions, with replacement. Portable Random
// keeps the same replay/settings/seed reproducible across native builds.
std::vector<TrainingExample> sample_replay_batch(const std::vector<TrainingExample>& examples,
                                               int batch_size, Random& random);

using TrainingProgress = std::function<void(int update, const LossMetrics& before_update)>;
// Continue candidate weights and momentum. The caller freezes the incumbent
// separately and saves this candidate only after the function succeeds.
Json train_candidate(PolicyValueNetwork& candidate, const ReplayBuffer& replay,
                     const TrainingSettings& settings, std::int64_t seed,
                     TrainingProgress progress = {});
} // namespace betago
