#pragma once

#include "neural_mcts.hpp"
#include "runner.hpp"
#include <functional>
#include <memory>

namespace betago {
struct SelfPlaySettings {
    int board_size = 9;
    double komi = 7.5;
    int max_moves = 200;
    NeuralMctsSettings search{64, 1.5};
    double temperature = 1.0;
    int temperature_moves = 20;
    double root_uniform_mix = 0.25;
    void validate() const;
    Json to_json() const;
};

struct SelfPlayGame {
    Json record;
    std::vector<TrainingExample> examples;
};
using SelfPlayProgress = std::function<void(int, const GameState&, const SearchStatistics&)>;

// Targets always use raw normalized root visits, independently of the
// temperature used to select a self-play action.
std::vector<double> visit_policy(const GameState& state, const SearchStatistics& statistics);
std::size_t sample_policy(const std::vector<double>& policy, Random& random);

SelfPlayGame run_self_play(std::shared_ptr<const PolicyValueNetwork> network,
                          const SelfPlaySettings& settings, std::int64_t seed,
                          SelfPlayProgress progress = {});
std::vector<TrainingExample> validate_self_play_game(const Json& record);
Json self_play_dataset(const Json& games);
} // namespace betago
