#include "betago/mcts.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace betago;
using mcts_detail::Node;

void mcts_check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void mcts_rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument was not thrown");
}

GameState mcts_position(std::initializer_list<std::string> rows, int to_play = BLACK,
                        double komi = 0, int passes = 0) {
    Board board;
    for (const auto& row : rows) {
        std::vector<int> cells;
        for (char cell : row) cells.push_back(cell == 'X' ? BLACK : cell == 'O' ? WHITE : EMPTY);
        board.push_back(std::move(cells));
    }
    return GameState(std::move(board), to_play, komi, passes);
}

Node& mcts_child(Node& parent, Move move) {
    parent.children.push_back(std::make_unique<Node>(parent.state.play(move), move, &parent));
    return *parent.children.back();
}

bool mcts_same_search(const SearchStatistics& first, const SearchStatistics& second) {
    if (first.simulations != second.simulations || first.root_visits != second.root_visits ||
        first.completed_rollouts != second.completed_rollouts ||
        first.truncated_rollouts != second.truncated_rollouts ||
        first.root_value_sum != second.root_value_sum || first.children.size() != second.children.size()) return false;
    for (std::size_t i = 0; i < first.children.size(); ++i) {
        const auto& a = first.children[i];
        const auto& b = second.children[i];
        if (a.move != b.move || a.visits != b.visits || a.value_sum != b.value_sum) return false;
    }
    return true;
}

struct MctsSuite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS MCTS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL MCTS " << name << ": " << error.what() << '\n';
        }
    }
};
} // namespace

int run_mcts_tests() {
    MctsSuite suite;
    suite.test("UCT evaluates a child from its parent's perspective", [] {
        Node root(GameState::new_game(2, 0));
        auto& opponent_win = mcts_child(root, Point{0, 0});
        auto& opponent_loss = mcts_child(root, Point{0, 1});
        root.visits = 20;
        opponent_win.visits = opponent_loss.visits = 10;
        opponent_win.value_sum = 10;
        opponent_loss.value_sum = -10;
        mcts_check(&mcts_detail::select_child(root, 0) == &opponent_loss,
                   "UCT must negate the child player's mean reward");
    });
    suite.test("UCT balances exploration and exploitation", [] {
        Node root(GameState::new_game(2, 0));
        auto& proven = mcts_child(root, Point{0, 0});
        auto& unexplored = mcts_child(root, Point{0, 1});
        root.visits = 101;
        proven.visits = 100; proven.value_sum = -100;
        unexplored.visits = 1; unexplored.value_sum = 0;
        mcts_check(&mcts_detail::select_child(root, 0) == &proven);
        // Hand scores: proven = 1 + sqrt(2)*sqrt(log(101)/100),
        // unexplored = sqrt(2)*sqrt(log(101)), so exploration selects the latter.
        mcts_check(&mcts_detail::select_child(root, std::sqrt(2.0)) == &unexplored);
    });
    suite.test("UCT visits an unvisited child before a scored child", [] {
        Node root(GameState::new_game(2, 0));
        auto& visited = mcts_child(root, Point{0, 0});
        auto& unvisited = mcts_child(root, Point{0, 1});
        root.visits = visited.visits = 5; visited.value_sum = -5;
        mcts_check(&mcts_detail::select_child(root, 0) == &unvisited);
    });
    suite.test("UCT ties are deterministic", [] {
        Node root(GameState::new_game(2, 0));
        auto& first = mcts_child(root, Point{0, 0});
        auto& second = mcts_child(root, Point{0, 1});
        root.visits = 4; first.visits = second.visits = 2;
        first.value_sum = second.value_sum = 0;
        const auto* chosen = &mcts_detail::select_child(root, 1);
        for (int i = 0; i < 5; ++i) mcts_check(&mcts_detail::select_child(root, 1) == chosen);
    });
    suite.test("two-level backup negates the known leaf outcome", [] {
        Node root(GameState::new_game(2, 0));
        auto& leaf = mcts_child(root, Point{0, 0});
        mcts_detail::backpropagate(leaf, 1);
        mcts_check(leaf.visits == 1 && leaf.value_sum == 1);
        mcts_check(root.visits == 1 && root.value_sum == -1);
        mcts_detail::backpropagate(leaf, -1);
        mcts_check(leaf.visits == 2 && leaf.value_sum == 0);
        mcts_check(root.visits == 2 && root.value_sum == 0);
    });
    suite.test("three-level backup alternates signs on every edge", [] {
        Node root(GameState::new_game(3, 0));
        auto& middle = mcts_child(root, Point{0, 0});
        auto& leaf = mcts_child(middle, Point{2, 2});
        auto& sibling = mcts_child(root, Point{0, 1});
        mcts_detail::backpropagate(leaf, -1);
        mcts_check(leaf.visits == 1 && leaf.value_sum == -1);
        mcts_check(middle.visits == 1 && middle.value_sum == 1);
        mcts_check(root.visits == 1 && root.value_sum == -1);
        mcts_check(sibling.visits == 0 && sibling.value_sum == 0);
        mcts_detail::backpropagate(leaf, 0);
        mcts_check(leaf.visits == 2 && middle.visits == 2 && root.visits == 2);
        mcts_check(leaf.value_sum == -1 && middle.value_sum == 1 && root.value_sum == -1);
    });
    suite.test("terminal wins losses and draws use the requested player", [] {
        for (int to_play : {BLACK, WHITE}) {
            const auto black_win = mcts_position({"XX", "X."}, to_play, 0, 2);
            const auto white_win = mcts_position({"OO", "O."}, to_play, 0, 2);
            const auto draw = mcts_position({"X.", ".O"}, to_play, 0, 2);
            mcts_check(mcts_detail::terminal_value(black_win, BLACK) == 1);
            mcts_check(mcts_detail::terminal_value(black_win, WHITE) == -1);
            mcts_check(mcts_detail::terminal_value(white_win, WHITE) == 1);
            mcts_check(mcts_detail::terminal_value(white_win, BLACK) == -1);
            mcts_check(mcts_detail::terminal_value(draw, BLACK) == 0);
            mcts_check(mcts_detail::terminal_value(draw, WHITE) == 0);
        }
        const auto komi_win = GameState::new_game(1, 7.5).play(PASS).play(PASS);
        mcts_check(mcts_detail::terminal_value(komi_win, WHITE) == 1);
        mcts_check(mcts_detail::terminal_value(komi_win, BLACK) == -1);
    });
    suite.test("simulation budget and root child accounting are exact", [] {
        Random random(4);
        MctsSearch search(GameState::new_game(3, 0), {17, std::sqrt(2.0), 12}, random);
        mcts_check(!search.finished() && search.statistics().simulations == 0);
        search.step(5);
        mcts_check(search.statistics().simulations == 5 && !search.finished());
        search.step(100);
        const auto stats = search.statistics();
        mcts_check(search.finished() && stats.simulations == 17 && stats.root_visits == 17);
        mcts_check(stats.completed_rollouts + stats.truncated_rollouts == 17);
        int child_visits = 0;
        for (const auto& child : stats.children) child_visits += child.visits;
        mcts_check(child_visits == stats.root_visits);
        mcts_check(std::isfinite(stats.elapsed_seconds) && stats.elapsed_seconds >= 0);
        search.step(2);
        mcts_check(mcts_same_search(stats, search.statistics()), "Finished search must not run extra simulations");
    });
    suite.test("zero rollout limit truncates without manufacturing an outcome", [] {
        Random random(10);
        MctsSearch search(GameState::new_game(3, 7.5), {1, 1, 0}, random);
        search.step();
        const auto stats = search.statistics();
        mcts_check(stats.completed_rollouts == 0 && stats.truncated_rollouts == 1);
        mcts_check(stats.root_value_sum == 0 && stats.children.size() == 1);
        mcts_check(stats.children.front().value_sum == 0);
    });
    suite.test("terminal position reached on the rollout cap is scored", [] {
        Random random(0);
        MctsSearch search(GameState::new_game(1, 7.5), {1, 1, 1}, random);
        search.step();
        const auto stats = search.statistics();
        mcts_check(stats.completed_rollouts == 1 && stats.truncated_rollouts == 0);
        mcts_check(stats.root_value_sum == -1 && stats.children.front().value_sum == 1);
        mcts_check(search.best_move() == PASS);
    });
    suite.test("terminal expanded leaf is scored even with no rollout moves", [] {
        Random random(0);
        const auto one_pass = GameState::new_game(1, 7.5).play(PASS);
        MctsSearch search(one_pass, {3, 1, 0}, random);
        search.step(3);
        const auto stats = search.statistics();
        mcts_check(stats.completed_rollouts == 3 && stats.truncated_rollouts == 0);
        mcts_check(stats.root_value_sum == 3 && stats.children.front().value_sum == -3);
        mcts_check(search.best_move() == PASS);
    });
    suite.test("only-pass positions keep normal two-pass termination", [] {
        const auto empty = GameState::new_game(1, 0);
        mcts_check(empty.legal_moves() == std::vector<Move>{PASS});
        MctsAgent agent({4, 1, 2}, 0);
        const auto first = empty.play(agent.choose_move(empty));
        mcts_check(first.consecutive_passes() == 1 && !first.is_terminal());
        const auto second = first.play(agent.choose_move(first));
        mcts_check(second.consecutive_passes() == 2 && second.is_terminal());
        mcts_check(agent.last_search().root_value_sum == 0);
    });
    suite.test("chosen moves are legal on small and 9 by 9 boards", [] {
        for (int size : {2, 3, 9}) {
            MctsAgent agent({6, std::sqrt(2.0), 4}, 12);
            auto state = GameState::new_game(size);
            for (int turn = 0; turn < 3 && !state.is_terminal(); ++turn) {
                const auto legal = state.legal_moves();
                const auto move = agent.choose_move(state);
                mcts_check(std::find(legal.begin(), legal.end(), move) != legal.end());
                state = state.play(move);
            }
        }
    });
    suite.test("search preserves its input position including ko history", [] {
        const auto state = GameState::new_game(3, 0).play(Point{0, 0}).play(Point{2, 2}).play(PASS);
        const auto original = state;
        MctsAgent agent({12, 1, 8}, 6);
        agent.choose_move(state);
        mcts_check(state == original);
        mcts_check(state.previous_board() == original.previous_board());
    });
    suite.test("seeded searches reproduce moves and tree statistics", [] {
        MctsAgent first({18, std::sqrt(2.0), 12}, 123);
        MctsAgent second({18, std::sqrt(2.0), 12}, 123);
        auto state = GameState::new_game(3, 0);
        for (int turn = 0; turn < 3 && !state.is_terminal(); ++turn) {
            const auto a = first.choose_move(state), b = second.choose_move(state);
            mcts_check(a == b && mcts_same_search(first.last_search(), second.last_search()));
            state = state.play(a);
        }
    });
    suite.test("incremental search partitions preserve random draws and results", [] {
        const auto state = GameState::new_game(3, 0);
        Random first_random(32), second_random(32);
        MctsSearch first(state, {19, std::sqrt(2.0), 10}, first_random);
        MctsSearch second(state, {19, std::sqrt(2.0), 10}, second_random);
        first.step(19);
        for (int count : {1, 4, 2, 7, 5}) second.step(count);
        mcts_check(first.best_move() == second.best_move());
        mcts_check(mcts_same_search(first.statistics(), second.statistics()));
        mcts_check(first_random.next_u32() == second_random.next_u32());
    });
    suite.test("recommended move has the most root visits", [] {
        Random random(42);
        MctsSearch search(GameState::new_game(2, 0), {31, std::sqrt(2.0), 8}, random);
        search.step(31);
        const auto stats = search.statistics();
        const auto move = search.best_move();
        const auto chosen = std::find_if(stats.children.begin(), stats.children.end(),
                                        [&](const MoveStatistics& child) { return child.move == move; });
        mcts_check(chosen != stats.children.end());
        for (const auto& child : stats.children) mcts_check(chosen->visits >= child.visits);
        for (int i = 0; i < 5; ++i) mcts_check(search.best_move() == move);
    });
    suite.test("agent search sessions use the configured budget", [] {
        MctsAgent agent({7, 0, 0}, 5);
        auto search = agent.start_search(GameState::new_game(2, 0));
        mcts_check(search && !search->finished());
        while (!search->finished()) search->step();
        mcts_check(search->statistics().simulations == 7);
        mcts_check(search->best_move() == search->best_move());
        agent.choose_move(GameState::new_game(2, 0));
        mcts_check(agent.last_search().simulations == 7);
    });
    suite.test("search throughput handles zero and known elapsed time", [] {
        SearchStatistics stats;
        mcts_check(stats.simulations_per_second() == 0);
        stats.simulations = 10;
        mcts_check(stats.simulations_per_second() == 0);
        stats.elapsed_seconds = 0.25;
        mcts_check(stats.simulations_per_second() == 40);
    });
    suite.test("invalid settings are rejected at every public entry", [] {
        const std::vector<MctsSettings> invalid = {
            {0, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {1, 1, -1},
            {1, std::numeric_limits<double>::infinity(), 1},
            {1, std::numeric_limits<double>::quiet_NaN(), 1}
        };
        for (const auto& settings : invalid) {
            mcts_rejects([&] { settings.validate(); });
            mcts_rejects([&] { MctsAgent agent(settings, 0); });
            Random random(0);
            mcts_rejects([&] { MctsSearch search(GameState::new_game(2), settings, random); });
        }
        MctsSettings{1, 0, 0}.validate();
    });
    suite.test("terminal input and invalid search steps are rejected", [] {
        const auto terminal = GameState::new_game(1).play(PASS).play(PASS);
        MctsAgent agent({2, 1, 1}, 0);
        mcts_rejects([&] { agent.choose_move(terminal); });
        mcts_rejects([&] { agent.start_search(terminal); });
        Random random(0);
        mcts_rejects([&] { MctsSearch search(terminal, {2, 1, 1}, random); });
        MctsSearch search(GameState::new_game(2), {2, 1, 1}, random);
        mcts_rejects([&] { search.best_move(); });
        mcts_rejects([&] { search.step(0); });
        mcts_rejects([&] { search.step(-1); });
        mcts_check(search.statistics().simulations == 0);
        search.step();
        mcts_check(search.statistics().simulations == 1 && !search.finished());
        mcts_check(search.best_move() == search.statistics().children.front().move);
    });
    std::cout << "MCTS: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed;
}
