#include "betago/neural_mcts.hpp"
#include "betago/arena.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>

namespace {
using namespace betago;
void check_puct(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
void close_puct(double actual, double expected, double tolerance = 1e-10) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error("Expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
template<class Function> void rejects_puct(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid input was accepted");
}
struct PuctSuite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS PUCT " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL PUCT " << name << ": " << error.what() << '\n';
        }
    }
};
Prediction constant_prediction(const GameState& state, double value = 0.0) {
    return {{}, std::vector<double>(static_cast<std::size_t>(state.size() * state.size() + 1), 1.0), value};
}
GameState diagram(std::initializer_list<std::string> rows, int player = BLACK) {
    Board board;
    for (const auto& row : rows) {
        std::vector<int> cells;
        for (char cell : row) cells.push_back(cell == 'X' ? BLACK : cell == 'O' ? WHITE : EMPTY);
        board.push_back(std::move(cells));
    }
    return GameState(std::move(board), player, 0.5);
}
void same_search(const SearchStatistics& first, const SearchStatistics& second) {
    check_puct(first.algorithm == second.algorithm && first.simulations == second.simulations &&
        first.root_visits == second.root_visits && first.root_value_sum == second.root_value_sum &&
        first.network_evaluations == second.network_evaluations && first.terminal_evaluations == second.terminal_evaluations &&
        first.children.size() == second.children.size());
    for (std::size_t i = 0; i < first.children.size(); ++i) {
        const auto& a = first.children[i]; const auto& b = second.children[i];
        check_puct(a.move == b.move && a.visits == b.visits && a.value_sum == b.value_sum && a.prior == b.prior);
    }
}
std::shared_ptr<PolicyValueNetwork> pass_network(int size) {
    auto network = std::make_shared<PolicyValueNetwork>(NetworkSettings{size, 2, 3}, 10);
    auto parameters = network->parameters(); std::fill(parameters.begin(), parameters.end(), 0.0);
    for (const auto& section : network->parameter_blocks())
        if (section.name == "policy_bias") parameters[section.offset + size * size] = 8.0;
    network->set_parameters(std::move(parameters));
    return network;
}
} // namespace

int run_neural_mcts_tests() {
    using namespace betago;
    using namespace neural_mcts_detail;
    PuctSuite suite;
    suite.test("legal priors mask occupation suicide and immediate ko using the actual rules", [] {
        auto ko = diagram({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        for (const auto& state : {GameState::new_game(9), diagram({".O.", "O.O", ".O."}), ko,
                                  ko.play(PASS).play(Point{4, 0})}) {
            auto prediction = constant_prediction(state); auto legal = state.legal_moves();
            auto priors = normalize_priors(state, prediction);
            close_puct(std::accumulate(priors.begin(), priors.end(), 0.0), 1.0);
            for (std::size_t i = 0; i < priors.size(); ++i) {
                bool allowed = std::find(legal.begin(), legal.end(), action_from_index(static_cast<int>(i), state.size())) != legal.end();
                close_puct(priors[i], allowed ? 1.0 / static_cast<double>(legal.size()) : 0.0);
            }
        }
    });
    suite.test("illegal mass is removed before normalization and zero legal mass falls back uniformly", [] {
        auto state = GameState::new_game(3).play(Point{0, 0}); auto prediction = constant_prediction(state);
        prediction.policy[0] = 1e300; prediction.policy[9] = 9;
        auto priors = normalize_priors(state, prediction);
        close_puct(priors[0], 0); close_puct(priors.back(), 9.0 / 17.0);
        std::fill(prediction.policy.begin(), prediction.policy.end(), 0); prediction.policy[0] = 100;
        priors = normalize_priors(state, prediction);
        close_puct(priors[0], 0); close_puct(priors.back(), 1.0 / 9.0);
    });
    suite.test("normalizing huge finite legal weights avoids overflow", [] {
        auto state = GameState::new_game(3); auto prediction = constant_prediction(state);
        std::fill(prediction.policy.begin(), prediction.policy.end(), 1e308);
        auto priors = normalize_priors(state, prediction);
        for (double prior : priors) close_puct(prior, 0.1);
    });
    suite.test("malformed policy shapes weights and value estimates reject", [] {
        auto state = GameState::new_game(3); const auto valid = constant_prediction(state);
        for (int mutation = 0; mutation < 6; ++mutation) {
            auto bad = valid;
            if (mutation == 0) bad.policy.pop_back();
            if (mutation == 1) bad.policy[0] = -1;
            if (mutation == 2) bad.policy[0] = std::numeric_limits<double>::quiet_NaN();
            if (mutation == 3) bad.policy[0] = std::numeric_limits<double>::infinity();
            if (mutation == 4) bad.value = 1.01;
            if (mutation == 5) bad.value = std::numeric_limits<double>::quiet_NaN();
            rejects_puct([&] { normalize_priors(state, bad); });
            rejects_puct([&] { NeuralMctsSearch search(state, {}, [bad](const GameState&) { return bad; }); });
        }
    });
    suite.test("PUCT negates child value and includes prior weighted visit bonus", [] {
        Node parent(GameState::new_game(3)); parent.visits = 24; parent.expanded = true;
        auto first = std::make_unique<Node>(parent.state.play(Point{0, 0}), Point{0, 0}, &parent, 0.1);
        first->visits = 4; first->value_sum = -2;
        auto second = std::make_unique<Node>(parent.state.play(Point{0, 1}), Point{0, 1}, &parent, 0.9);
        second->visits = 9; second->value_sum = -4.5;
        parent.children.push_back(std::move(first)); parent.children.push_back(std::move(second));
        check_puct(select_child(parent, 1.5).incoming_move == Move(Point{0, 1}));
        check_puct(select_child(parent, 0).incoming_move == Move(Point{0, 0}), "Value tie keeps legal order");
        parent.children[0]->value_sum = 2;
        check_puct(select_child(parent, 0).incoming_move == Move(Point{0, 1}), "Child positive value is bad for parent");
    });
    suite.test("PUCT does not force every unvisited move ahead of a promising visited move", [] {
        Node parent(GameState::new_game(3)); parent.visits = 1; parent.expanded = true;
        auto unseen = std::make_unique<Node>(parent.state.play(Point{0, 0}), Point{0, 0}, &parent, 0);
        auto good = std::make_unique<Node>(parent.state.play(Point{0, 1}), Point{0, 1}, &parent, 1);
        good->visits = 1; good->value_sum = -1;
        parent.children.push_back(std::move(unseen)); parent.children.push_back(std::move(good));
        check_puct(select_child(parent, 1.5).incoming_move == Move(Point{0, 1}));
    });
    suite.test("neural backup alternates the player perspective across two and three levels", [] {
        Node root(GameState::new_game(3));
        Node child(root.state.play(Point{0, 0}), Point{0, 0}, &root);
        Node leaf(child.state.play(Point{0, 1}), Point{0, 1}, &child);
        backpropagate(leaf, 0.75);
        close_puct(leaf.value_sum, 0.75); close_puct(child.value_sum, -0.75); close_puct(root.value_sum, 0.75);
        backpropagate(child, 0.25);
        close_puct(child.value_sum, -0.5); close_puct(root.value_sum, 0.5);
        check_puct(root.visits == 2 && child.visits == 2 && leaf.visits == 1);
    });
    suite.test("first simulation follows the highest legal policy prior", [] {
        auto evaluator = [](const GameState& state) {
            auto result = constant_prediction(state);
            std::fill(result.policy.begin(), result.policy.end(), 0.0); result.policy[4] = 0.9; result.policy.back() = 0.1;
            return result;
        };
        PolicyAgent policy(evaluator); NeuralMctsAgent search(evaluator, {1, 1.5});
        check_puct(policy.choose_move(GameState::new_game(3)) == Move(Point{1, 1}));
        check_puct(search.choose_move(GameState::new_game(3)) == Move(Point{1, 1}));
        auto stats = search.last_search(); check_puct(stats.root_visits == 1 && stats.network_evaluations == 2);
        close_puct(stats.children[4].prior, 0.9);
    });
    suite.test("neural leaf value is backed up rather than replaced by a random rollout", [] {
        int calls = 0;
        NeuralMctsSearch search(GameState::new_game(1, 0.5), {2, 1.5}, [&](const GameState& state) {
            ++calls; check_puct(!state.is_terminal(), "Terminal position must bypass network");
            return constant_prediction(state, 0.7);
        });
        check_puct(calls == 1 && search.statistics().root_visits == 0);
        search.step(); auto first = search.statistics();
        close_puct(first.root_value_sum, -0.7); close_puct(first.children.front().value_sum, 0.7);
        search.step(); auto second = search.statistics();
        close_puct(second.root_value_sum, -1.7); close_puct(second.children.front().value_sum, 1.7);
        check_puct(calls == 2 && second.network_evaluations == 2 && second.terminal_evaluations == 1);
        check_puct(second.completed_rollouts == 0 && second.truncated_rollouts == 0);
    });
    suite.test("terminal winning losing and drawn leaves bypass contradictory network values", [] {
        for (double komi : {-0.5, 0.0, 0.5}) {
            auto state = GameState::new_game(1, komi).play(PASS); int calls = 0;
            NeuralMctsSearch search(state, {3, 1.5}, [&](const GameState& position) {
                ++calls; check_puct(!position.is_terminal()); return constant_prediction(position, -0.9);
            });
            search.step(10); auto stats = search.statistics();
            const double expected = komi < 0 ? -1 : komi > 0 ? 1 : 0;
            close_puct(stats.root_value_sum, 3 * expected);
            check_puct(calls == 1 && stats.network_evaluations == 1 && stats.terminal_evaluations == 3);
            check_puct(search.best_move() == PASS);
        }
    });
    suite.test("simulation budget and evaluation counters are exact and root children include unvisited legal moves", [] {
        NeuralMctsSearch search(GameState::new_game(3), {17, 1.5}, [](const GameState& state) { return constant_prediction(state); });
        rejects_puct([&] { search.best_move(); }); search.step(3); check_puct(!search.finished());
        search.step(100); auto stats = search.statistics();
        check_puct(search.finished() && stats.algorithm == "puct" && stats.simulations == 17 && stats.root_visits == 17);
        check_puct(stats.network_evaluations + stats.terminal_evaluations == 18 && stats.children.size() == 10);
        int visits = 0; double value = 0, priors = 0;
        for (const auto& child : stats.children) { visits += child.visits; value += child.value_sum; priors += child.prior; }
        check_puct(visits == 17); close_puct(stats.root_value_sum, -value); close_puct(priors, 1);
        search.step(); same_search(stats, search.statistics()); rejects_puct([&] { search.step(0); });
    });
    suite.test("visit count selects the final move with prior then legal order breaking ties", [] {
        auto evaluator = [](const GameState& state) { return constant_prediction(state); };
        NeuralMctsSearch search(GameState::new_game(3), {3, 1.5}, evaluator); search.step(3);
        auto stats = search.statistics(); auto best = search.best_move();
        auto found = std::find_if(stats.children.begin(), stats.children.end(), [&](const auto& child) { return child.move == best; });
        check_puct(found != stats.children.end());
        for (const auto& child : stats.children) check_puct(found->visits >= child.visits);
        check_puct(best == Move(Point{0, 0}));
    });
    suite.test("incremental partitions reproduce completed search moves priors visits and values", [] {
        auto state = GameState::new_game(3).play(Point{0, 0});
        auto evaluator = [](const GameState& position) { return constant_prediction(position, 0.2); };
        NeuralMctsSearch whole(state, {25, 1.5}, evaluator), split(state, {25, 1.5}, evaluator);
        whole.step(25); split.step(2); split.step(7); while (!split.finished()) split.step();
        check_puct(whole.best_move() == split.best_move()); same_search(whole.statistics(), split.statistics());
    });
    suite.test("neural search preserves input board and ko history while choosing a legal move", [] {
        auto state = diagram({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        auto original = state;
        NeuralMctsAgent agent([](const GameState& position) { return constant_prediction(position); }, {5, 1.5});
        auto move = agent.choose_move(state); state.play(move); check_puct(state == original);
        for (const auto& child : agent.last_search().children) check_puct(child.move != Move(Point{1, 1}));
    });
    suite.test("checkpoint-backed policy and search perform inference without changing model training state", [] {
        auto network = pass_network(3); network->train(); const auto parameters = network->parameters();
        const auto velocity = network->velocity(); const auto steps = network->training_steps();
        PolicyAgent policy(network); NeuralMctsAgent search(network, {2, 1.5});
        auto state = GameState::new_game(3); check_puct(policy.choose_move(state) == PASS);
        auto move = search.choose_move(state); state.play(move);
        check_puct(network->parameters() == parameters && network->velocity() == velocity &&
                   network->training_steps() == steps && network->training());
    });
    suite.test("search evaluator owns its checkpoint after the originating agent is destroyed", [] {
        std::unique_ptr<NeuralMctsSearch> search;
        { NeuralMctsAgent agent(pass_network(3), {4, 1.5}); search = agent.start_search(GameState::new_game(3)); }
        search->step(4); check_puct(search->finished()); GameState::new_game(3).play(search->best_move());
    });
    suite.test("invalid settings empty evaluators terminal inputs and board mismatch reject", [] {
        auto evaluator = [](const GameState& state) { return constant_prediction(state); };
        for (auto settings : {NeuralMctsSettings{0, 1.5}, NeuralMctsSettings{std::numeric_limits<int>::max(), 1.5}, NeuralMctsSettings{1, -1},
                              NeuralMctsSettings{1, std::numeric_limits<double>::infinity()}})
            rejects_puct([&] { NeuralMctsSearch search(GameState::new_game(3), settings, evaluator); });
        rejects_puct([&] { NeuralMctsSearch search(GameState::new_game(3), {}, {}); });
        rejects_puct([&] { NeuralMctsSearch search(GameState::new_game(3).play(PASS).play(PASS), {}, evaluator); });
        rejects_puct([&] { PolicyAgent policy(std::shared_ptr<const PolicyValueNetwork>{}); });
        rejects_puct([&] { NeuralMctsAgent agent(pass_network(3)); agent.choose_move(GameState::new_game(9)); });
        rejects_puct([&] { PolicyAgent policy(evaluator); policy.choose_move(GameState::new_game(3).play(PASS).play(PASS)); });
    });
    std::cout << "PUCT: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed;
}
