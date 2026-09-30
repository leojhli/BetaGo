#pragma once

#include "mcts.hpp"
#include "network.hpp"
#include <functional>
#include <memory>

namespace betago {
struct NeuralMctsSettings {
    int simulations = 128;
    double c_puct = 1.5;
    void validate() const;
};

// The returned value describes the evaluated state's player to move.
// A search owns a copy of this callback; captured data must also be owned.
using NeuralEvaluator = std::function<Prediction(const GameState&)>;

namespace neural_mcts_detail {
struct Node {
    GameState state;
    Move incoming_move;
    Node* parent;
    double prior;
    int visits = 0;
    double value_sum = 0;
    bool expanded = false;
    std::vector<std::unique_ptr<Node>> children;

    explicit Node(GameState state, Move incoming_move = PASS,
                  Node* parent = nullptr, double prior = 1.0);
};

// Full row-major action vector, with pass last and illegal actions zeroed.
// Masking uses the rules engine; an all-zero legal mass becomes uniform.
std::vector<double> normalize_priors(const GameState& state, const Prediction& prediction);
Node& select_child(Node& parent, double c_puct);
void backpropagate(Node& leaf, double leaf_value);
} // namespace neural_mcts_detail

// Direct policy inference provides a baseline for comparison with PUCT.
class PolicyAgent {
public:
    explicit PolicyAgent(std::shared_ptr<const PolicyValueNetwork> network);
    explicit PolicyAgent(NeuralEvaluator evaluator);
    Move choose_move(const GameState& state);
    const Prediction& last_prediction() const { return last_prediction_; }

private:
    NeuralEvaluator evaluator_;
    Prediction last_prediction_;
};

class NeuralMctsSearch {
public:
    NeuralMctsSearch(const GameState& state, NeuralMctsSettings settings,
                     NeuralEvaluator evaluator);
    NeuralMctsSearch(const NeuralMctsSearch&) = delete;
    NeuralMctsSearch& operator=(const NeuralMctsSearch&) = delete;

    // The root is inferred and expanded at construction, with zero visits.
    // Each step evaluates one selected leaf and backs up one result.
    void step(int count = 1);
    bool finished() const;
    Move best_move() const;
    SearchStatistics statistics() const;

private:
    std::unique_ptr<neural_mcts_detail::Node> root_;
    NeuralMctsSettings settings_;
    NeuralEvaluator evaluator_;
    int network_evaluations_ = 0;
    int terminal_evaluations_ = 0;
    double elapsed_seconds_ = 0;

    double evaluate_and_expand(neural_mcts_detail::Node& node);
    void simulate();
};

class NeuralMctsAgent {
public:
    explicit NeuralMctsAgent(std::shared_ptr<const PolicyValueNetwork> network,
                             NeuralMctsSettings settings = {});
    explicit NeuralMctsAgent(NeuralEvaluator evaluator, NeuralMctsSettings settings = {});
    Move choose_move(const GameState& state);
    std::unique_ptr<NeuralMctsSearch> start_search(const GameState& state);
    const SearchStatistics& last_search() const { return last_search_; }

private:
    NeuralEvaluator evaluator_;
    NeuralMctsSettings settings_;
    SearchStatistics last_search_;
};
} // namespace betago
