#pragma once

#include "mcts.hpp"
#include "neural_mcts.hpp"
#include "runner.hpp"

namespace betago {
struct AgentConfiguration {
    std::string kind = "random";
    MctsSettings search{};
    std::string checkpoint{};
    NeuralMctsSettings neural{};
    void validate() const;
    Json to_json() const;
};

struct ArenaSettings {
    AgentConfiguration a{"mcts", {}};
    AgentConfiguration b{"random", {}};
    int pairs = 5;
    int size = 3;
    int max_moves = 100;
    double komi = 7.5;
    std::int64_t seed = 0;
    void validate() const;
};

struct ArenaDecision {
    Move move;
    std::optional<SearchStatistics> search;
    std::optional<Prediction> prediction{};
};
using ArenaAgent = std::function<ArenaDecision(const GameState&)>;
using ArenaAgentFactory = std::function<ArenaAgent(const AgentConfiguration&, std::int64_t)>;
using ArenaProgress = std::function<void(int game_index, const Json& record)>;

Json search_statistics_json(const SearchStatistics& statistics);
Json mcts_settings_json(const MctsSettings& settings);
Json neural_mcts_settings_json(const NeuralMctsSettings& settings);
Json prediction_json(const Prediction& prediction);

// A checkpoint is loaded and checked once before any game starts. Every fresh
// agent then borrows the same immutable model snapshot throughout the run.
struct PreparedAgent {
    AgentConfiguration configuration;
    std::shared_ptr<const PolicyValueNetwork> network;
    Json checkpoint_metadata = nullptr;
    ArenaAgent create(std::int64_t seed) const;
    Json to_json() const;
};
PreparedAgent prepare_agent(const AgentConfiguration& configuration, int board_size);

// Each independent seed pair plays both color assignments. Seeds belong to
// agent identities, and each game creates fresh agents with those same seeds.
Json run_arena(const ArenaSettings& settings, const Json& metadata = Json::object(),
               const ArenaProgress& progress = {}, const ArenaAgentFactory& factory = {});

// Aggregate outcomes only when a game ends by two passes. A color pair enters
// the uncertainty bounds only when both of its games completed.
Json summarize_arena(const Json& games);
} // namespace betago
