#pragma once

#include "random.hpp"
#include <memory>

namespace betago {
struct MctsSettings {
    int simulations = 128;
    double exploration = 1.4142135623730951;
    int rollout_limit = 200;
    void validate() const;
};

struct MoveStatistics {
    Move move;
    int visits;
    // Value is measured for the child state's player to move.
    double value_sum;
    double prior = 0;
};

struct SearchStatistics {
    int simulations = 0;
    int root_visits = 0;
    int completed_rollouts = 0;
    int truncated_rollouts = 0;
    double root_value_sum = 0;
    double elapsed_seconds = 0;
    std::vector<MoveStatistics> children;
    std::string algorithm = "uct";
    int network_evaluations = 0;
    int terminal_evaluations = 0;
    double simulations_per_second() const;
};

// Exposed to let small, known-outcome tree tests check the value convention.
namespace mcts_detail {
struct Node {
    GameState state;
    Move incoming_move;
    Node* parent;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<Move> unexpanded_moves;
    int visits = 0;
    double value_sum = 0;

    explicit Node(GameState state, Move incoming_move = PASS, Node* parent = nullptr);
};

Node& select_child(Node& parent, double exploration);
void backpropagate(Node& leaf, double leaf_value);
double terminal_value(const GameState& state, int perspective);
} // namespace mcts_detail

// A search owns a fresh tree, but borrows its random generator. Keep the
// generator (or the MctsAgent that owns it) alive until this search is destroyed.
class MctsSearch {
public:
    MctsSearch(const GameState& state, MctsSettings settings, Random& random);
    MctsSearch(const MctsSearch&) = delete;
    MctsSearch& operator=(const MctsSearch&) = delete;

    // Incremental work lets the visual board run its event loop between batches.
    void step(int count = 1);
    bool finished() const;
    Move best_move() const;
    SearchStatistics statistics() const;

private:
    std::unique_ptr<mcts_detail::Node> root_;
    MctsSettings settings_;
    Random& random_;
    int completed_rollouts_ = 0;
    int truncated_rollouts_ = 0;
    double elapsed_seconds_ = 0;

    void simulate();
};

class MctsAgent {
public:
    explicit MctsAgent(MctsSettings settings = {}, std::int64_t seed = 0);
    Move choose_move(const GameState& state);
    std::unique_ptr<MctsSearch> start_search(const GameState& state);
    const SearchStatistics& last_search() const { return last_search_; }

private:
    MctsSettings settings_;
    Random random_;
    SearchStatistics last_search_;
};
} // namespace betago
