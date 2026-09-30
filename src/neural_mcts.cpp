#include "betago/neural_mcts.hpp"
#include "betago/profile.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace betago {
namespace {
NeuralEvaluator network_evaluator(std::shared_ptr<const PolicyValueNetwork> network) {
    if (!network) throw std::invalid_argument("Neural agent requires a network");
    // The shared ownership travels into each search, so the search can safely
    // finish even after the agent that created it has been destroyed.
    return [network = std::move(network)](const GameState& state) {
        return network->predict(encode_position(state));
    };
}

void validate_evaluator(const NeuralEvaluator& evaluator) {
    if (!evaluator) throw std::invalid_argument("Neural agent requires an evaluator");
}

void validate_value(double value) {
    if (!std::isfinite(value) || value < -1 || value > 1)
        throw std::invalid_argument("Neural value must be finite and between -1 and 1");
}

void validate_node_values(const neural_mcts_detail::Node& node) {
    if (node.visits < 0 || !std::isfinite(node.value_sum)
        || std::abs(node.value_sum) > static_cast<double>(node.visits) + 1e-9
        || !std::isfinite(node.prior) || node.prior < 0 || node.prior > 1)
        throw std::invalid_argument("Neural tree statistics must be finite and consistent");
}
} // namespace

void NeuralMctsSettings::validate() const {
    if (simulations <= 0)
        throw std::invalid_argument("Neural MCTS simulations must be positive");
    if (simulations == std::numeric_limits<int>::max())
        throw std::invalid_argument("Neural MCTS simulation budget must leave room for the root evaluation count");
    if (!std::isfinite(c_puct) || c_puct < 0)
        throw std::invalid_argument("PUCT exploration must be finite and nonnegative");
}

namespace neural_mcts_detail {
Node::Node(GameState position, Move move, Node* parent_node, double move_prior)
    : state(std::move(position)), incoming_move(move), parent(parent_node), prior(move_prior) {
    if (!std::isfinite(prior) || prior < 0 || prior > 1)
        throw std::invalid_argument("Neural move prior must be finite and between 0 and 1");
}

std::vector<double> normalize_priors(const GameState& state, const Prediction& prediction) {
    ProfileScope scope(ProfileWork::Normalize);
    const auto action_count = static_cast<std::size_t>(state.size() * state.size() + 1);
    if (prediction.policy.size() != action_count)
        throw std::invalid_argument("Neural policy action count does not match the board");
    validate_value(prediction.value);
    for (double weight : prediction.policy)
        if (!std::isfinite(weight) || weight < 0)
            throw std::invalid_argument("Neural policy weights must be finite and nonnegative");

    std::vector<double> priors(action_count, 0.0);
    const auto legal = state.legal_moves();
    if (legal.empty()) return priors;
    double largest = 0;
    for (Move move : legal)
        largest = std::max(largest, prediction.policy[action_index(move, state.size())]);
    if (largest == 0) {
        const double uniform = 1.0 / static_cast<double>(legal.size());
        for (Move move : legal) priors[action_index(move, state.size())] = uniform;
        return priors;
    }

    // Scaling by the largest legal weight avoids overflowing the sum even
    // when an evaluator supplies unnormalized weights near DBL_MAX.
    double mass = 0;
    for (Move move : legal) {
        const auto action = action_index(move, state.size());
        priors[action] = prediction.policy[action] / largest;
        mass += priors[action];
    }
    for (double& prior : priors) prior /= mass;
    return priors;
}

Node& select_child(Node& parent, double c_puct) {
    if (parent.children.empty())
        throw std::invalid_argument("Cannot select a child from an empty neural tree node");
    if (!std::isfinite(c_puct) || c_puct < 0)
        throw std::invalid_argument("PUCT exploration must be finite and nonnegative");
    validate_node_values(parent);

    Node* best = nullptr;
    long double best_score = -std::numeric_limits<long double>::infinity();
    const long double visit_scale = std::sqrt(static_cast<long double>(parent.visits) + 1);
    for (auto& child : parent.children) {
        if (!child) throw std::invalid_argument("Neural tree child must not be null");
        validate_node_values(*child);
        // W/N belongs to the child's player to move, so the parent's expected
        // reward is its negation. An unvisited action starts with Q = 0.
        const long double value = child->visits > 0
            ? -static_cast<long double>(child->value_sum) / child->visits : 0;
        const long double score = value + static_cast<long double>(c_puct) * child->prior
            * visit_scale / (static_cast<long double>(child->visits) + 1);
        if (score > best_score) {
            best_score = score;
            best = child.get();
        }
    }
    return *best;
}

void backpropagate(Node& leaf, double leaf_value) {
    validate_value(leaf_value);
    for (Node* node = &leaf; node; node = node->parent) {
        validate_node_values(*node);
        if (node->visits == std::numeric_limits<int>::max())
            throw std::overflow_error("Neural tree visit count is too large");
    }
    for (Node* node = &leaf; node; node = node->parent) {
        ++node->visits;
        node->value_sum += leaf_value;
        leaf_value = -leaf_value;
    }
}
} // namespace neural_mcts_detail

PolicyAgent::PolicyAgent(std::shared_ptr<const PolicyValueNetwork> network)
    : PolicyAgent(network_evaluator(std::move(network))) {}

PolicyAgent::PolicyAgent(NeuralEvaluator evaluator) : evaluator_(std::move(evaluator)) {
    validate_evaluator(evaluator_);
}

Move PolicyAgent::choose_move(const GameState& state) {
    if (state.is_terminal())
        throw std::invalid_argument("Cannot choose a policy move after the game ends");
    Prediction prediction = evaluator_(state);
    prediction.policy = neural_mcts_detail::normalize_priors(state, prediction);
    const auto legal = state.legal_moves();
    Move best = legal.front();
    for (Move move : legal)
        if (prediction.policy[action_index(move, state.size())]
            > prediction.policy[action_index(best, state.size())]) best = move;
    last_prediction_ = std::move(prediction);
    return best;
}

NeuralMctsSearch::NeuralMctsSearch(const GameState& state, NeuralMctsSettings settings,
                                 NeuralEvaluator evaluator)
    : settings_(settings), evaluator_(std::move(evaluator)) {
    settings_.validate();
    validate_evaluator(evaluator_);
    if (state.is_terminal())
        throw std::invalid_argument("Cannot search for a neural move after the game ends");
    const auto started = std::chrono::steady_clock::now();
    root_ = std::make_unique<neural_mcts_detail::Node>(state);
    evaluate_and_expand(*root_);
    elapsed_seconds_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
}

double NeuralMctsSearch::evaluate_and_expand(neural_mcts_detail::Node& node) {
    ProfileScope scope(ProfileWork::Expand);
    if (node.state.is_terminal()) {
        const double value = mcts_detail::terminal_value(node.state, node.state.to_play());
        ++terminal_evaluations_;
        return value;
    }

    const Prediction prediction = evaluator_(node.state);
    const auto priors = neural_mcts_detail::normalize_priors(node.state, prediction);
    std::vector<std::unique_ptr<neural_mcts_detail::Node>> children;
    for (Move move : node.state.legal_moves()) {
        children.push_back(std::make_unique<neural_mcts_detail::Node>(
            node.state.play(move), move, &node, priors[action_index(move, node.state.size())]));
    }
    node.children = std::move(children);
    node.expanded = true;
    ++network_evaluations_;
    return prediction.value;
}

void NeuralMctsSearch::simulate() {
    ProfileScope scope(ProfileWork::Simulation);
    auto* leaf = root_.get();
    while (!leaf->state.is_terminal() && leaf->expanded)
        leaf = &neural_mcts_detail::select_child(*leaf, settings_.c_puct);
    const double value = evaluate_and_expand(*leaf);
    neural_mcts_detail::backpropagate(*leaf, value);
}

void NeuralMctsSearch::step(int count) {
    if (count <= 0) throw std::invalid_argument("Neural MCTS step count must be positive");
    if (finished()) return;
    const int remaining = std::min(count, settings_.simulations - root_->visits);
    const auto started = std::chrono::steady_clock::now();
    for (int simulation = 0; simulation < remaining; ++simulation) simulate();
    // Include root inference and active steps, excluding pauses between GUI batches.
    elapsed_seconds_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
}

bool NeuralMctsSearch::finished() const { return root_->visits >= settings_.simulations; }

Move NeuralMctsSearch::best_move() const {
    if (root_->visits == 0)
        throw std::invalid_argument("Run at least one neural MCTS simulation before choosing a move");
    const neural_mcts_detail::Node* best = root_->children.front().get();
    for (const auto& child : root_->children)
        if (child->visits > best->visits
            || (child->visits == best->visits && child->prior > best->prior)) best = child.get();
    return best->incoming_move;
}

SearchStatistics NeuralMctsSearch::statistics() const {
    SearchStatistics result;
    result.algorithm = "puct";
    result.simulations = result.root_visits = root_->visits;
    result.root_value_sum = root_->value_sum;
    result.elapsed_seconds = elapsed_seconds_;
    result.network_evaluations = network_evaluations_;
    result.terminal_evaluations = terminal_evaluations_;
    for (const auto& child : root_->children)
        result.children.push_back({child->incoming_move, child->visits,
                                   child->value_sum, child->prior});
    return result;
}

NeuralMctsAgent::NeuralMctsAgent(std::shared_ptr<const PolicyValueNetwork> network,
                               NeuralMctsSettings settings)
    : NeuralMctsAgent(network_evaluator(std::move(network)), settings) {}

NeuralMctsAgent::NeuralMctsAgent(NeuralEvaluator evaluator, NeuralMctsSettings settings)
    : evaluator_(std::move(evaluator)), settings_(settings) {
    validate_evaluator(evaluator_);
    settings_.validate();
}

std::unique_ptr<NeuralMctsSearch> NeuralMctsAgent::start_search(const GameState& state) {
    return std::make_unique<NeuralMctsSearch>(state, settings_, evaluator_);
}

Move NeuralMctsAgent::choose_move(const GameState& state) {
    auto search = start_search(state);
    search->step(settings_.simulations);
    last_search_ = search->statistics();
    return search->best_move();
}
} // namespace betago
