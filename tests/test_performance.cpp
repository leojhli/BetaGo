#include "betago/profile.hpp"
#include "betago/replay.hpp"
#include "betago/selfplay.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <string>

namespace {
using namespace betago;

void check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("Expected profiling overlap rejection");
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS PERFORMANCE " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL PERFORMANCE " << name << ": " << error.what() << '\n';
        }
    }
};

// Frozen, independent M1/M5 reference algorithms. These intentionally keep
// set-based flood fills and fresh successor probes rather than consuming any
// optimized legality cache or group implementation under test.
std::vector<Point> slow_neighbors(Point point, int size) {
    std::vector<Point> result;
    for (Point neighbor : {Point{point.row - 1, point.column}, Point{point.row + 1, point.column},
                           Point{point.row, point.column - 1}, Point{point.row, point.column + 1}})
        if (neighbor.row >= 0 && neighbor.column >= 0 && neighbor.row < size && neighbor.column < size)
            result.push_back(neighbor);
    return result;
}
Group slow_group(const Board& board, Point point) {
    const int size = static_cast<int>(board.size());
    const int color = board[point.row][point.column];
    if (color == EMPTY) throw std::invalid_argument("A group must start at a stone");
    Group result{{point}, {}};
    std::vector<Point> pending{point};
    while (!pending.empty()) {
        const Point current = pending.back(); pending.pop_back();
        for (Point neighbor : slow_neighbors(current, size)) {
            const int adjacent = board[neighbor.row][neighbor.column];
            if (adjacent == EMPTY) result.liberties.insert(neighbor);
            else if (adjacent == color && result.stones.insert(neighbor).second) pending.push_back(neighbor);
        }
    }
    return result;
}
GameState slow_play(const GameState& state, Move move) {
    if (state.is_terminal()) throw IllegalMove("The game has ended");
    const int opponent = state.to_play() == BLACK ? WHITE : BLACK;
    if (!move) return GameState(state.board(), opponent, state.komi(),
                               state.consecutive_passes() + 1, state.board());
    if (move->row < 0 || move->column < 0 || move->row >= state.size() || move->column >= state.size())
        throw IllegalMove("Point is outside the board");
    if (state.board()[move->row][move->column] != EMPTY) throw IllegalMove("Intersection occupied");
    Board board = state.board();
    board[move->row][move->column] = state.to_play();
    std::set<Point> captured;
    for (Point neighbor : slow_neighbors(*move, state.size())) {
        if (board[neighbor.row][neighbor.column] == opponent) {
            const Group group = slow_group(board, neighbor);
            if (group.liberties.empty()) captured.insert(group.stones.begin(), group.stones.end());
        }
    }
    for (Point stone : captured) board[stone.row][stone.column] = EMPTY;
    if (slow_group(board, *move).liberties.empty()) throw IllegalMove("Suicide forbidden");
    if (state.previous_board() && board == *state.previous_board()) throw KoViolation("Simple ko");
    return GameState(std::move(board), opponent, state.komi(), 0, state.board());
}
std::vector<Move> slow_legal(const GameState& state) {
    std::vector<Move> result;
    if (state.is_terminal()) return result;
    for (int row = 0; row < state.size(); ++row) for (int column = 0; column < state.size(); ++column) {
        const Point point{row, column};
        if (state.board()[row][column] != EMPTY) continue;
        try { slow_play(state, point); result.push_back(point); } catch (const IllegalMove&) {}
    }
    result.push_back(PASS);
    return result;
}
EncodedPosition slow_encode(const GameState& state) {
    const int area = state.size() * state.size();
    EncodedPosition result{state.size(), std::vector<double>(FEATURE_CHANNELS * area, 0),
                           std::vector<bool>(area + 1, false), state.is_terminal()};
    const int opponent = state.to_play() == BLACK ? WHITE : BLACK;
    const double signed_komi = state.to_play() == WHITE ? state.komi() : -state.komi();
    for (int row = 0; row < state.size(); ++row) for (int column = 0; column < state.size(); ++column) {
        const int point = row * state.size() + column;
        const int color = state.board()[row][column];
        result.features[point] = color == state.to_play() ? 1 : 0;
        result.features[area + point] = color == opponent ? 1 : 0;
        result.features[3 * area + point] = state.consecutive_passes() == 1 ? 1 : 0;
        result.features[4 * area + point] = state.is_terminal() ? 1 : 0;
        result.features[5 * area + point] = std::tanh(signed_komi / area);
        if (!state.is_terminal() && color == EMPTY) {
            try {
                slow_play(state, Point{row, column});
                result.legal_actions[point] = true; result.features[6 * area + point] = 1;
            } catch (const KoViolation&) { result.features[2 * area + point] = 1; }
            catch (const IllegalMove&) {}
        }
    }
    result.legal_actions[area] = !state.is_terminal();
    result.validate();
    return result;
}
void same_encoding(const EncodedPosition& first, const EncodedPosition& second) {
    check(first.board_size == second.board_size && first.terminal == second.terminal &&
          first.features == second.features && first.legal_actions == second.legal_actions,
          "Optimized feature encoding differs from independent reference");
}
template<class Function> std::pair<int, std::optional<GameState>> play_result(Function function) {
    try { return {0, function()}; }
    catch (const KoViolation&) { return {2, std::nullopt}; }
    catch (const IllegalMove&) { return {1, std::nullopt}; }
    catch (const std::invalid_argument&) { return {3, std::nullopt}; }
}
void same_successor(const GameState& state, Move move) {
    const auto slow = play_result([&] { return slow_play(state, move); });
    const auto fast = play_result([&] { return state.play(move); });
    check(slow.first == fast.first && slow.second == fast.second,
          "Optimized successor or exception type differs from independent reference");
}
void compare_rules(const GameState& state, bool features, bool all_successors) {
    const GameState original = state;
    const auto legal = slow_legal(state);
    check(state.legal_moves() == legal, "Legal action order differs from independent reference");
    if (features) same_encoding(encode_position(state), slow_encode(state));
    if (all_successors) {
        same_successor(state, PASS);
        for (int row = 0; row < state.size(); ++row) for (int column = 0; column < state.size(); ++column)
            same_successor(state, Point{row, column});
        same_successor(state, Point{-1, 0}); same_successor(state, Point{state.size(), 0});
    }
    for (int row = 0; row < state.size(); ++row) for (int column = 0; column < state.size(); ++column) {
        if (state.board()[row][column] == EMPTY) continue;
        const auto expected = slow_group(state.board(), {row, column});
        const auto actual = state.group_and_liberties({row, column});
        check(expected.stones == actual.stones && expected.liberties == actual.liberties,
              "Group stones or unique liberties differ from independent reference");
    }
    check(state == original, "Queries changed the semantic GameState");
}
GameState diagram(std::initializer_list<std::string> rows, int player = BLACK,
                  double komi = .5, int passes = 0, std::optional<Board> previous = std::nullopt) {
    Board board;
    for (const auto& row : rows) {
        std::vector<int> cells;
        for (char value : row) cells.push_back(value == 'X' ? BLACK : value == 'O' ? WHITE : EMPTY);
        board.push_back(std::move(cells));
    }
    return GameState(std::move(board), player, komi, passes, std::move(previous));
}
void same_prediction(const Prediction& first, const Prediction& second) {
    check(first.logits == second.logits && first.policy == second.policy && first.value == second.value,
          "Profiling changed a neural prediction");
}
void same_loss(const LossMetrics& first, const LossMetrics& second) {
    check(first.policy == second.policy && first.value == second.value &&
          first.regularization == second.regularization && first.total == second.total,
          "Profiling changed a neural objective");
}
void same_search(const SearchStatistics& first, const SearchStatistics& second) {
    check(first.algorithm == second.algorithm && first.simulations == second.simulations &&
          first.root_visits == second.root_visits && first.root_value_sum == second.root_value_sum &&
          first.completed_rollouts == second.completed_rollouts && first.truncated_rollouts == second.truncated_rollouts &&
          first.network_evaluations == second.network_evaluations &&
          first.terminal_evaluations == second.terminal_evaluations && first.children.size() == second.children.size(),
          "Profiling changed search counts or backup values");
    for (std::size_t index = 0; index < first.children.size(); ++index) {
        const auto& a = first.children[index]; const auto& b = second.children[index];
        check(a.move == b.move && a.visits == b.visits && a.value_sum == b.value_sum && a.prior == b.prior,
              "Profiling changed root move statistics");
    }
}
Json without_elapsed(Json data) {
    if (data.is_object()) {
        data.erase("elapsed_seconds");
        for (auto& item : data.items()) item.value() = without_elapsed(std::move(item.value()));
    } else if (data.is_array()) for (auto& child : data) child = without_elapsed(std::move(child));
    return data;
}
std::vector<TrainingExample> tiny_batch() {
    std::vector<TrainingExample> result;
    GameState state = GameState::new_game(3, .5);
    for (Move move : {Move{Point{0, 0}}, Move{Point{2, 2}}, Move{Point{0, 1}}}) {
        auto input = encode_position(state);
        std::vector<double> policy(input.legal_actions.size(), 0);
        policy[action_index(move, 3)] = 1;
        result.push_back({std::move(input), std::move(policy), state.to_play() == BLACK ? 1.0 : -1.0});
        state = state.play(move);
    }
    return result;
}
const ProfileMetric& metric(const ProfileSession& session, ProfileWork work) {
    return session.metrics()[static_cast<std::size_t>(work)];
}
void sane_metrics(const ProfileSession& session) {
    for (const auto& value : session.metrics()) {
        check(std::isfinite(value.inclusive_seconds) && std::isfinite(value.exclusive_seconds) &&
              value.inclusive_seconds >= 0 && value.exclusive_seconds >= -1e-12 &&
              value.exclusive_seconds <= value.inclusive_seconds + 1e-12,
              "Profiler produced invalid inclusive or exclusive durations");
    }
}
} // namespace

int run_performance_tests() {
    Suite suite;
    suite.test("disabled scope is a no-op and sessions begin with fresh counters", [] {
        check(active_profile == nullptr);
        { ProfileScope inactive(ProfileWork::Group); }
        check(active_profile == nullptr);
        { ProfileSession session; check(active_profile == &session);
          for (const auto& value : session.metrics()) check(value.calls == 0 && value.inclusive_seconds == 0 && value.exclusive_seconds == 0); }
        { ProfileSession session; for (const auto& value : session.metrics()) check(value.calls == 0); }
        check(active_profile == nullptr);
    });
    suite.test("nested and recursive scopes report calls with exclusive decomposition", [] {
        ProfileSession session;
        { ProfileScope outer(ProfileWork::StatePlay);
          { ProfileScope child(ProfileWork::Group);
            { ProfileScope repeated(ProfileWork::Group); } } }
        check(metric(session, ProfileWork::StatePlay).calls == 1 && metric(session, ProfileWork::Group).calls == 2);
        const auto& outer = metric(session, ProfileWork::StatePlay);
        const auto& group = metric(session, ProfileWork::Group);
        check(std::abs(outer.exclusive_seconds + group.exclusive_seconds - outer.inclusive_seconds) < 1e-9,
              "Exclusive nested durations do not reconstruct outer duration");
        sane_metrics(session);
    });
    suite.test("overlap rejection and exception unwinding preserve the active scope", [] {
        ProfileSession session;
        { ProfileScope outer(ProfileWork::StatePlay);
          rejects([] { ProfileSession overlap; });
          check(active_profile == &session);
          try { ProfileScope child(ProfileWork::Group); throw std::runtime_error("probe"); }
          catch (const std::runtime_error&) {}
          { ProfileScope another(ProfileWork::Group); } }
        check(metric(session, ProfileWork::StatePlay).calls == 1 && metric(session, ProfileWork::Group).calls == 2);
        sane_metrics(session);
    });
    suite.test("98 migration positions preserve independent rules and feature outputs", [] {
        auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures/migration.json";
        std::ifstream input(path); check(static_cast<bool>(input), "Cannot read migration feature reference fixture");
        Json fixtures = Json::parse(input); check(fixtures.is_array() && fixtures.size() == 98);
        for (const auto& item : fixtures) {
            std::optional<Board> previous;
            if (!item.at("previous_board").is_null()) previous = item.at("previous_board").get<Board>();
            GameState state(item.at("board").get<Board>(), item.at("to_play").get<int>(),
                            item.at("komi").get<double>(), item.at("passes").get<int>(), std::move(previous));
            compare_rules(state, true, true);
        }
    });
    suite.test("seeded 3 5 9 and 19 boards preserve rules and all feature planes", [] {
        for (int size : {3, 5, 9, 19}) {
            for (std::int64_t seed : {7, 59}) {
                GameState state = GameState::new_game(size, .5);
                Random random(seed);
                for (int ply = 0; ply < 45; ++ply) {
                    compare_rules(state, true, size <= 5 || ply % 10 == 0);
                    if (state.is_terminal()) break;
                    const auto legal = slow_legal(state);
                    const Move move = legal[random.below(legal.size())];
                    same_successor(state, move); state = state.play(move);
                }
                if (!state.is_terminal()) {
                    state = state.play(PASS);
                    if (!state.is_terminal()) state = state.play(PASS);
                }
                compare_rules(state, true, true);
            }
        }
    });
    suite.test("capture ko suicide pass and copied sibling queries preserve state identity", [] {
        const auto ko = diagram({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        const std::vector<GameState> positions{
            diagram({".O.", "O.O", ".O."}), diagram({"XOX", "O.O", "XOX"}), ko,
            ko.play(PASS), ko.play(PASS).play(Point{4, 0}),
            GameState(ko.board(), BLACK, ko.komi(), 0, ko.previous_board()),
            GameState(ko.board(), ko.to_play(), ko.komi(), 0),
            ko.play(PASS).play(PASS)};
        for (const auto& state : positions) {
            const auto early_copy = state;
            compare_rules(state, true, true);
            const auto late_copy = state;
            check(early_copy == late_copy && state == early_copy, "Query caches leaked into semantic equality");
            compare_rules(early_copy, true, true); compare_rules(late_copy, true, true);
            const auto legal = slow_legal(state);
            for (Move move : legal) {
                const auto child = state.play(move);
                compare_rules(child, true, false);
                check(state == early_copy, "Sibling expansion changed its source state");
            }
        }
    });
    suite.test("multi-group and large connected captures preserve reference results", [] {
        const auto four_groups = diagram({"..X..", ".XOX.", "XO.OX", ".XOX.", "..X.."});
        same_successor(four_groups, Point{2, 2});
        const auto four_result = four_groups.play(Point{2, 2});
        for (Point neighbor : slow_neighbors({2, 2}, 5)) check(four_result.at(neighbor) == EMPTY);
        compare_rules(four_groups, true, true); compare_rules(four_result, true, true);
        const auto shared_group = diagram({"XXXXX", "XOOOX", "XO.OX", "XOOOX", "XXXXX"});
        same_successor(shared_group, Point{2, 2});
        const auto ring_result = shared_group.play(Point{2, 2});
        check(std::all_of(ring_result.board()[1].begin() + 1, ring_result.board()[1].end() - 1,
                          [](int value) { return value == EMPTY; }));
        compare_rules(shared_group, true, true); compare_rules(ring_result, true, true);
        for (int size : {19, 20}) {
            const Point center{size / 2, size / 2};
            Board board(size, std::vector<int>(size, BLACK)); board[center.row][center.column] = EMPTY;
            const GameState opponent(board, WHITE, .5), friendly(board, BLACK, .5);
            same_successor(opponent, center); same_successor(friendly, center);
            const auto captured = opponent.play(center);
            int stones = 0;
            for (const auto& row : captured.board()) for (int stone : row) stones += stone != EMPTY;
            check(stones == 1 && captured.at(center) == WHITE, "Connected opponent group was not fully captured");
            const auto expected = slow_group(opponent.board(), {0, 0});
            const auto actual = opponent.group_and_liberties({0, 0});
            check(expected.stones == actual.stones && expected.liberties == actual.liberties);
            check(expected.stones.size() == static_cast<std::size_t>(size * size - 1) && expected.liberties == std::set<Point>{center});
            if (size <= 19) { same_encoding(encode_position(opponent), slow_encode(opponent));
                              same_encoding(encode_position(friendly), slow_encode(friendly)); }
        }
    });
    suite.test("rules retain support for board sizes beyond the neural limit", [] {
        GameState state = GameState::new_game(20, 0);
        check(state.legal_moves().size() == 401);
        state = state.play(Point{19, 19}).play(Point{0, 0});
        compare_rules(state, false, true);
        try { encode_position(state); throw std::runtime_error("Neural board limit was removed"); }
        catch (const std::invalid_argument&) {}
    });
    suite.test("profiling preserves owned predictions exact objectives and updates", [] {
        PolicyValueNetwork ordinary({3, 2, 3}, 73), profiled = ordinary;
        const auto batch = tiny_batch();
        std::vector<EncodedPosition> inputs;
        for (const auto& example : batch) inputs.push_back(example.input);
        const auto predictions = ordinary.predict_batch(inputs);
        const auto gradient = ordinary.loss_and_gradient(batch, .03);
        const auto loss = ordinary.evaluate_batch(batch, .03);
        ordinary.train(); profiled.train();
        const OptimizerSettings optimizer{.015, .8, .03};
        const auto update = ordinary.train_batch(batch, optimizer);
        { ProfileSession session;
          const auto actual = profiled.predict_batch(inputs);
          for (std::size_t index = 0; index < actual.size(); ++index) {
              same_prediction(actual[index], predictions[index]);
              same_prediction(actual[index], profiled.predict(inputs[index]));
          }
          const auto first_owned = actual.front();
          profiled.predict(inputs.back()); same_prediction(first_owned, actual.front());
          const auto actual_gradient = profiled.loss_and_gradient(batch, .03);
          same_loss(actual_gradient.loss, gradient.loss); check(actual_gradient.gradient == gradient.gradient);
          same_loss(profiled.evaluate_batch(batch, .03), loss);
          same_loss(profiled.train_batch(batch, optimizer), update);
          check(metric(session, ProfileWork::Forward).calls > 0 && metric(session, ProfileWork::Objective).calls > 0);
          sane_metrics(session); }
        check(ordinary.parameters() == profiled.parameters() && ordinary.velocity() == profiled.velocity() &&
              ordinary.training_steps() == profiled.training_steps() && ordinary.last_optimizer() == profiled.last_optimizer());
    });
    suite.test("batched predictions own outputs across terminal and nonterminal positions", [] {
        PolicyValueNetwork network({3, 2, 3}, 79);
        const auto state = GameState::new_game(3, .5);
        const std::vector<EncodedPosition> inputs{
            encode_position(state), encode_position(state.play(Point{0, 0})),
            encode_position(state.play(PASS).play(PASS)), encode_position(state)};
        std::vector<Prediction> expected;
        for (const auto& input : inputs) expected.push_back(network.predict(input));
        { ProfileSession session;
          auto actual = network.predict_batch(inputs);
          for (std::size_t index = 0; index < inputs.size(); ++index) same_prediction(actual[index], expected[index]);
          check(std::all_of(actual[2].policy.begin(), actual[2].policy.end(), [](double value) { return value == 0; }),
                "Terminal batch sample retained legal probability mass");
          network.predict_batch({inputs[1], inputs[0]});
          for (std::size_t index = 0; index < inputs.size(); ++index) same_prediction(actual[index], expected[index]);
          check(metric(session, ProfileWork::Forward).calls == inputs.size() + 2); sane_metrics(session); }
    });
    suite.test("profiled checkpoint continuation preserves the exact next optimizer step", [] {
        auto path = std::filesystem::path("results/performance_tests/continuation.json");
        PolicyValueNetwork original({3, 2, 3}, 89);
        const auto batch = tiny_batch(); const OptimizerSettings optimizer{.012, .7, .002};
        original.train(); original.train_batch(batch, optimizer); original.save(path);
        auto restored = PolicyValueNetwork::load(path);
        const auto expected = original.train_batch(batch, optimizer);
        { ProfileSession session; same_loss(restored.train_batch(batch, optimizer), expected); sane_metrics(session); }
        check(restored.parameters() == original.parameters() && restored.velocity() == original.velocity() &&
              restored.training_steps() == original.training_steps() && restored.last_optimizer() == original.last_optimizer());
    });
    suite.test("neural choices and expansions each scan legal moves only once", [] {
        // A synthetic evaluator keeps feature encoding out of the work counts.
        const auto evaluator = [](const GameState& state) {
            return Prediction{{}, std::vector<double>(state.size() * state.size() + 1, 1.0), 0.25};
        };
        for (const auto& state : {GameState::new_game(3, .5).play(Point{1, 1}),
                                  GameState::new_game(1, .5)}) {
            const auto legal = state.legal_moves();
            {
                ProfileSession session;
                PolicyAgent policy(evaluator);
                check(policy.choose_move(state) == legal.front(), "Uniform policy changed legal tie order");
                check(metric(session, ProfileWork::LegalMoves).calls == 1,
                      "Policy choice repeated its legality sweep");
            }
            {
                ProfileSession session;
                NeuralMctsSearch search(state, {12, 1.5}, evaluator);
                search.step(12);
                const auto stats = search.statistics();
                check(stats.network_evaluations > 1 && stats.simulations == 12);
                check(metric(session, ProfileWork::LegalMoves).calls ==
                      static_cast<std::uint64_t>(stats.network_evaluations),
                      "Neural expansion repeated its legality sweep");
                check(std::find(legal.begin(), legal.end(), search.best_move()) != legal.end());
            }
        }
    });
    suite.test("profiling preserves exact classical and neural search statistics", [] {
        const auto state = GameState::new_game(3, .5).play(Point{1, 1});
        auto network = std::make_shared<PolicyValueNetwork>(NetworkSettings{3, 2, 3}, 91);
        NeuralMctsAgent ordinary(network, {32, 1.5});
        const Move expected = ordinary.choose_move(state); const auto search = ordinary.last_search();
        MctsAgent uct({32, 1.4, 30}, 91);
        const Move uct_move = uct.choose_move(state); const auto uct_search = uct.last_search();
        { ProfileSession session;
          NeuralMctsAgent profiled(network, {32, 1.5}); check(profiled.choose_move(state) == expected);
          same_search(profiled.last_search(), search);
          MctsAgent profiled_uct({32, 1.4, 30}, 91); check(profiled_uct.choose_move(state) == uct_move);
          same_search(profiled_uct.last_search(), uct_search);
          check(metric(session, ProfileWork::Simulation).calls >= 32 && metric(session, ProfileWork::Forward).calls > 0);
          sane_metrics(session); }
    });
    suite.test("profiling preserves seeded self-play targets and training labels", [] {
        auto network = std::make_shared<PolicyValueNetwork>(NetworkSettings{3, 2, 3}, 101);
        SelfPlaySettings settings;
        settings.board_size = 3; settings.komi = .5; settings.search = {16, 1.5}; settings.max_moves = 60;
        const auto ordinary = run_self_play(network, settings, 17);
        { ProfileSession session;
          const auto profiled = run_self_play(network, settings, 17);
          check(without_elapsed(ordinary.record) == without_elapsed(profiled.record), "Profiling changed self-play evidence");
          check(ordinary.examples.size() == profiled.examples.size());
          for (std::size_t index = 0; index < ordinary.examples.size(); ++index) {
              same_encoding(ordinary.examples[index].input, profiled.examples[index].input);
              check(ordinary.examples[index].policy == profiled.examples[index].policy &&
                    ordinary.examples[index].value == profiled.examples[index].value);
          }
          check(metric(session, ProfileWork::SelfPlay).calls > 0); sane_metrics(session); }
    });
    std::cout << suite.passed << " PERFORMANCE passed, " << suite.failed << " failed\n";
    return suite.failed;
}
