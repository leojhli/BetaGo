#include "betago/mcts.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace betago {
void MctsSettings::validate() const {
    if (simulations <= 0)
        throw std::invalid_argument("MCTS simulations must be positive");
    if (!std::isfinite(exploration) || exploration < 0)
        throw std::invalid_argument("MCTS exploration must be finite and nonnegative");
    if (rollout_limit < 0)
        throw std::invalid_argument("MCTS rollout limit must be nonnegative");
}

double SearchStatistics::simulations_per_second() const {
    return elapsed_seconds > 0 ? simulations / elapsed_seconds : 0;
}

namespace mcts_detail {
Node::Node(GameState position, Move move, Node* parent_node)
    : state(std::move(position)), incoming_move(move), parent(parent_node),
      unexpanded_moves(state.legal_moves()) {}

Node& select_child(Node& parent, double exploration) {
    if (parent.children.empty())
        throw std::invalid_argument("Cannot select a child from an empty tree node");
    if (!std::isfinite(exploration) || exploration < 0)
        throw std::invalid_argument("MCTS exploration must be finite and nonnegative");

    // Try an unvisited child before computing an average or dividing by N.
    for (auto& child : parent.children)
        if (child->visits == 0) return *child;

    Node* best = nullptr;
    double best_score = -std::numeric_limits<double>::infinity();
    double parent_log = std::log(static_cast<double>(std::max(1, parent.visits)));
    for (auto& child : parent.children) {
        // Child values describe the opponent's turn. Negation converts that
        // average to this parent's perspective; the second term explores.
        double score = -child->value_sum / child->visits
                     + exploration * std::sqrt(parent_log / child->visits);
        if (score > best_score) {
            best_score = score;
            best = child.get();
        }
    }
    return *best;
}

void backpropagate(Node& leaf, double leaf_value) {
    if (!std::isfinite(leaf_value) || leaf_value < -1 || leaf_value > 1)
        throw std::invalid_argument("MCTS result must be a finite value between -1 and 1");
    for (Node* node = &leaf; node; node = node->parent) {
        ++node->visits;
        node->value_sum += leaf_value;
        leaf_value = -leaf_value;
    }
}

double terminal_value(const GameState& state, int perspective) {
    if (perspective != BLACK && perspective != WHITE)
        throw std::invalid_argument("Unknown MCTS value perspective");
    auto winner = state.winner(); // Also rejects evaluation of an unfinished game.
    if (!winner) return 0;
    return *winner == perspective ? 1 : -1;
}
} // namespace mcts_detail

MctsSearch::MctsSearch(const GameState& state, MctsSettings settings, Random& random)
    : settings_(settings), random_(random) {
    settings_.validate();
    if (state.is_terminal())
        throw std::invalid_argument("Cannot search for a move after the game ends");
    root_ = std::make_unique<mcts_detail::Node>(state);
}

bool MctsSearch::finished() const { return root_->visits >= settings_.simulations; }

void MctsSearch::simulate() {
    using namespace mcts_detail;
    Node* leaf = root_.get();

    // Selection follows UCT through fully expanded nodes. Expansion adds
    // exactly one uniformly chosen legal action, including pass.
    while (!leaf->state.is_terminal()) {
        if (!leaf->unexpanded_moves.empty()) {
            auto index = random_.below(leaf->unexpanded_moves.size());
            Move move = leaf->unexpanded_moves[index];
            auto child = std::make_unique<Node>(leaf->state.play(move), move, leaf);
            leaf->unexpanded_moves.erase(leaf->unexpanded_moves.begin() + index);
            leaf->children.push_back(std::move(child));
            leaf = leaf->children.back().get();
            break;
        }
        leaf = &select_child(*leaf, settings_.exploration);
    }

    // Simulation uses the same uniform legal-move policy as the random agent.
    // Retain the leaf player's perspective even if the rollout ends on a
    // different player's turn. Tree backup, not rollout length, flips signs.
    GameState rollout = leaf->state;
    for (int moves = 0; moves < settings_.rollout_limit && !rollout.is_terminal(); ++moves) {
        auto actions = rollout.legal_moves();
        rollout = rollout.play(actions[random_.below(actions.size())]);
    }

    double value = 0;
    if (rollout.is_terminal()) {
        ++completed_rollouts_;
        value = terminal_value(rollout, leaf->state.to_play());
    } else {
        // Zero at the cutoff is an approximation, not an adjudicated draw.
        ++truncated_rollouts_;
    }
    backpropagate(*leaf, value);
}

void MctsSearch::step(int count) {
    if (count <= 0) throw std::invalid_argument("MCTS step count must be positive");
    if (finished()) return;
    int remaining = std::min(count, settings_.simulations - root_->visits);
    auto started = std::chrono::steady_clock::now();
    for (int simulation = 0; simulation < remaining; ++simulation) simulate();
    // Only active search work counts: time between GUI calls is excluded.
    elapsed_seconds_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
}

Move MctsSearch::best_move() const {
    if (root_->visits == 0)
        throw std::invalid_argument("Run at least one MCTS simulation before choosing a move");
    // The first expanded child wins a visit-count tie. No extra random draw is
    // consumed, so asking for this result repeatedly cannot alter the search.
    const mcts_detail::Node* best = root_->children.front().get();
    for (const auto& child : root_->children)
        if (child->visits > best->visits) best = child.get();
    return best->incoming_move;
}

SearchStatistics MctsSearch::statistics() const {
    SearchStatistics result;
    result.simulations = result.root_visits = root_->visits;
    result.completed_rollouts = completed_rollouts_;
    result.truncated_rollouts = truncated_rollouts_;
    result.root_value_sum = root_->value_sum;
    result.elapsed_seconds = elapsed_seconds_;
    for (const auto& child : root_->children)
        result.children.push_back({child->incoming_move, child->visits, child->value_sum});
    return result;
}

MctsAgent::MctsAgent(MctsSettings settings, std::int64_t seed)
    : settings_(settings), random_(seed) {
    settings_.validate();
}

std::unique_ptr<MctsSearch> MctsAgent::start_search(const GameState& state) {
    return std::make_unique<MctsSearch>(state, settings_, random_);
}

Move MctsAgent::choose_move(const GameState& state) {
    auto search = start_search(state);
    search->step(settings_.simulations);
    last_search_ = search->statistics();
    return search->best_move();
}
} // namespace betago
