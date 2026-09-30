#include "betago/options.hpp"
#include "betago/runner.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

using namespace betago;
int run_mcts_tests();

void check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
template<class Error = std::invalid_argument, class Function> void rejects(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected exception was not thrown");
}
GameState position(std::initializer_list<std::string> rows, int to_play = BLACK, double komi = 0) {
    Board board;
    for (const auto& row : rows) {
        std::vector<int> cells;
        for (char cell : row) cells.push_back(cell == 'X' ? BLACK : cell == 'O' ? WHITE : EMPTY);
        board.push_back(cells);
    }
    return GameState(board, to_play, komi);
}
bool contains(const std::vector<Move>& moves, Move action) {
    return std::find(moves.begin(), moves.end(), action) != moves.end();
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
};

int main(int argc, char** argv) {
    Suite suite;
    suite.test("new board and text", [] {
        auto state = GameState::new_game();
        check(state.size() == 9 && state.to_play() == BLACK && state.legal_moves().size() == 82);
        check(state.to_string().find("To play: Black") != std::string::npos);
    });
    suite.test("orthogonal neighbors", [] {
        auto state = GameState::new_game(3);
        auto corners = state.neighbors({0, 0});
        check(std::set<Point>(corners.begin(), corners.end()) == std::set<Point>{{0, 1}, {1, 0}});
        check(state.neighbors({0, 1}).size() == 3 && state.neighbors({1, 1}).size() == 4);
        rejects([&] { state.at({-1, 0}); });
    });
    suite.test("groups and distinct liberties", [] {
        auto state = position({"XX.", "X..", "..X"});
        auto group = state.group_and_liberties({0, 0});
        check(group.stones == std::set<Point>{{0, 0}, {0, 1}, {1, 0}});
        check(group.liberties == std::set<Point>{{0, 2}, {1, 1}, {2, 0}});
        check(state.group_and_liberties({2, 2}).stones == std::set<Point>{{2, 2}});
        rejects([&] { state.group_and_liberties({1, 1}); });
    });
    suite.test("diagonal separation", [] {
        check(position({"X..", ".X.", "..."}).group_and_liberties({0, 0}).stones.size() == 1);
    });
    suite.test("construction validation and input independence", [] {
        Board board(3, std::vector<int>(3));
        GameState state(board); board[0][0] = BLACK;
        check(state.at({0, 0}) == EMPTY);
        rejects([] { GameState::new_game(0); });
        rejects([] { GameState::new_game(-1); });
        rejects([] { GameState(Board{}); });
        rejects([] { GameState(Board{{0, 0}}); });
        rejects([] { GameState(Board{{3}}); });
        rejects([] { GameState(Board{{0}}, 3); });
        rejects([] { GameState(Board{{0}}, BLACK, 0, 3); });
        rejects([] { GameState::new_game(3, std::numeric_limits<double>::infinity()); });
        rejects([] { GameState(Board{{0}}, BLACK, 0, 0, Board{{0, 0}, {0, 0}}); });
    });
    suite.test("single capture", [] {
        auto state = position({".X.", "XO.", ".X."});
        auto result = state.play(Point{1, 2});
        check(result.at({1, 1}) == EMPTY && result.to_play() == WHITE && state.at({1, 1}) == WHITE);
    });
    suite.test("edge group capture", [] {
        auto result = position({"XOO", ".X.", "..."}).play(Point{1, 2});
        check(result.board()[0] == std::vector<int>{BLACK, EMPTY, EMPTY});
    });
    suite.test("multiple group capture", [] {
        auto result = position({"XOX", "O.X", "XX."}).play(Point{1, 1});
        check(result.at({0, 1}) == EMPTY && result.at({1, 0}) == EMPTY);
    });
    suite.test("suicide and capture before suicide", [] {
        auto state = position({".O.", "O.O", ".O."});
        rejects<IllegalMove>([&] { state.play(Point{1, 1}); });
        check(!contains(state.legal_moves(), Point{1, 1}));
        auto result = position({"XOX", "O.O", "XOX"}).play(Point{1, 1});
        check(result.group_and_liberties({1, 1}).liberties == std::set<Point>{{0, 1}, {1, 0}, {1, 2}, {2, 1}});
    });
    suite.test("ko and delayed recapture", [] {
        auto captured = position({".XO..", "XO.O.", ".XO..", ".....", "....."}).play(Point{1, 2});
        rejects<IllegalMove>([&] { captured.play(Point{1, 1}); });
        check(!contains(captured.legal_moves(), Point{1, 1}));
        auto result = captured.play(Point{4, 4}).play(Point{4, 0}).play(Point{1, 1});
        check(result.at({1, 2}) == EMPTY);
    });
    suite.test("pass updates ko history", [] {
        auto state = position({".XO..", "XO.O.", ".XO..", ".....", "....."});
        check(state.play(Point{1, 2}).play(PASS).play(Point{4, 0}).play(Point{1, 1}).at({1, 2}) == EMPTY);
    });
    suite.test("illegal moves and independent successors", [] {
        auto parent = GameState::new_game(3);
        auto first = parent.play(Point{0, 0}), sibling = parent.play(Point{1, 1});
        for (Point action : {Point{0, 0}, Point{-1, 0}, Point{3, 0}})
            rejects<IllegalMove>([&] { first.play(action); });
        check(parent.at({0, 0}) == EMPTY && sibling.at({0, 0}) == EMPTY && first.at({1, 1}) == EMPTY);
    });
    suite.test("passes and terminal state", [] {
        auto parent = GameState::new_game(3), passed = parent.play(PASS);
        check(passed.board() == parent.board() && passed.to_play() == WHITE && !passed.is_terminal());
        check(passed.play(Point{0, 0}).consecutive_passes() == 0);
        auto ended = passed.play(PASS);
        check(ended.is_terminal() && ended.legal_moves().empty());
        rejects<IllegalMove>([&] { ended.play(PASS); });
        rejects<IllegalMove>([&] { ended.play(Point{0, 0}); });
    });
    suite.test("empty score and komi", [] { check(GameState::new_game(3).score() == Score{0, 7.5}); });
    suite.test("exclusive area and neutral region", [] {
        check(position({"XXX..", "X.X..", "XXX..", ".....", "....O"}).score() == Score{9, 1});
        check(position({"OOO", "O.O", "OOO"}, BLACK, 0.5).score() == Score{0, 9.5});
    });
    suite.test("winner requires end and supports draw", [] {
        auto state = GameState::new_game(3, 0);
        rejects([&] { state.winner(); });
        check(!state.play(PASS).play(PASS).winner());
        check(GameState::new_game(3).play(PASS).play(PASS).winner() == WHITE);
        check(position({"XXX", "X.X", "XXX"}).play(PASS).play(PASS).winner() == BLACK);
    });
    suite.test("seed compatibility and independent generators", [] {
        Random first(19), second(19);
        const double expected[] = {0.6771258268002703, 0.7849113560871108, 0.5204661572030815, 0.5114917024932601, 0.39353466292596484};
        for (double value : expected) { check(first.unit() == value); check(second.unit() == value); }
        Random positive(42), negative(-42);
        for (int i = 0; i < 1000; ++i) check(positive.next_u32() == negative.next_u32());
        rejects([&] { first.below(0); });
    });
    suite.test("pass is sampled and terminal selection rejected", [] {
        RandomAgent agent(10);
        check(!agent.choose_move(GameState(Board{{BLACK}}, BLACK, 0)));
        rejects([&] { agent.choose_move(GameState::new_game().play(PASS).play(PASS)); });
        bool pass_seen = false;
        auto state = GameState::new_game(2);
        for (int i = 0; i < 100; ++i) {
            auto action = agent.choose_move(state);
            check(contains(state.legal_moves(), action));
            pass_seen |= !action;
        }
        check(pass_seen);
    });
    suite.test("runner completion and color dispatch", [] {
        int b = 0, w = 0;
        auto result = run_game([&](const GameState& s) -> Move { check(s.to_play() == BLACK); return ++b == 1 ? Move(Point{0, 0}) : Move(PASS); },
                              [&](const GameState& s) -> Move { check(s.to_play() == WHITE); return ++w == 1 ? Move(Point{2, 2}) : Move(PASS); }, 3, 0, 10);
        check(result.moves == std::vector<Move>{Point{0, 0}, Point{2, 2}, PASS, PASS});
        check(b == 2 && w == 2 && result.termination_reason() == "two_passes");
        check(result.score() == Score{1, 1} && !result.winner() && result.elapsed_seconds >= 0);
    });
    suite.test("truncation has no score or manufactured passes", [] {
        auto result = run_game([](const GameState&) -> Move { return Point{0, 0}; },
                              [](const GameState&) -> Move { throw std::runtime_error("White should not play"); }, 3, 7.5, 1);
        check(result.moves == std::vector<Move>{Point{0, 0}} && !result.final_state.is_terminal());
        check(result.termination_reason() == "move_limit" && !result.score() && !result.winner());
        check(result.to_json()["score"].is_null() && result.to_json()["winner"].is_null());
    });
    suite.test("second pass at move limit completes game", [] {
        auto pass = [](const GameState&) -> Move { return PASS; };
        auto result = run_game(pass, pass, 9, 7.5, 2);
        check(result.termination_reason() == "two_passes" && result.score() == Score{0, 7.5} && result.winner() == WHITE);
    });
    suite.test("illegal agent moves propagate", [] {
        auto occupied = [](const GameState&) -> Move { return Point{0, 0}; };
        rejects<IllegalMove>([&] { run_game(occupied, occupied, 3, 0, 2); });
    });
    suite.test("invalid runner settings", [] {
        auto pass = [](const GameState&) -> Move { return PASS; };
        rejects([&] { run_game(pass, pass, 3, 0, 0); });
        rejects([&] { run_game(pass, pass, 0); });
    });
    suite.test("seeded game replay and state invariants", [] {
        for (auto [size, seed] : {std::pair{2, 0}, {2, 2}, {3, 4}, {3, 6}, {9, 8}}) {
            auto play = [&] {
                RandomAgent black(seed), white(seed + 1);
                return run_game([&](const GameState& s) { return black.choose_move(s); },
                                [&](const GameState& s) { return white.choose_move(s); }, size, 7.5, 60);
            };
            auto result = play(), repeated = play();
            check(result.moves == repeated.moves && result.final_state == repeated.final_state);
            check(replay_moves(result.moves, size) == result.final_state);
            auto state = GameState::new_game(size);
            for (auto action : result.moves) {
                auto before = state;
                check(contains(state.legal_moves(), action));
                auto next = state.play(action);
                check(state == before && next.previous_board() == state.board());
                check(next.to_play() == (state.to_play() == BLACK ? WHITE : BLACK));
                check(next.consecutive_passes() == (action ? 0 : state.consecutive_passes() + 1));
                for (int r = 0; r < size; ++r) for (int c = 0; c < size; ++c)
                    if (next.at({r, c}) != EMPTY) check(!next.group_and_liberties({r, c}).liberties.empty());
                state = std::move(next);
            }
        }
    });
    suite.test("illegal replay and actions after end", [] {
        rejects<IllegalMove>([] { replay_moves({Point{0, 0}, Point{0, 0}}); });
        rejects<IllegalMove>([] { replay_moves({PASS, PASS, Point{0, 0}}); });
    });
    suite.test("JSON round trip and outcome validation", [] {
        auto pass = [](const GameState&) -> Move { return PASS; };
        auto result = run_game(pass, pass);
        auto path = std::filesystem::temp_directory_path() / ("betago-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        Json data = {{"schema_version", 1}, {"games", Json::array({result.to_json()})}};
        save_records(path, data);
        check(load_records(path) == data);
        for (const char* key : {"score", "winner", "termination_reason", "game_length"}) {
            auto bad = data; bad["games"][0][key] = "wrong"; save_records(path, bad);
            rejects([&] { load_records(path); });
        }
        auto truncated = run_game([](const GameState&) -> Move { return Point{0, 0}; }, pass, 3, 0, 1);
        data["games"][0] = truncated.to_json(); save_records(path, data); check(load_records(path) == data);
        data["games"][0]["max_moves"] = 2; save_records(path, data);
        rejects([&] { load_records(path); });
    });
    suite.test("malformed JSON moves rejected", [] {
        for (Json moves : {Json("wrong"), Json::array({1}), Json::array({Json::array({1})}),
                           Json::array({Json::array({1.5, 0})}), Json::array({Json::array({-1, 0})})})
            rejects([&] { moves_from_json(moves); });
    });
    suite.test("CLI option parsing rejects malformed integers", [] {
        char executable[] = "tests", key[] = "--count", value[] = "1.5";
        char* values[] = {executable, key, value};
        Options options(3, values, {"--count"}, {});
        rejects([&] { options.integer("--count", 1); });
    });
    if (argc == 3 && std::string(argv[1]) == "--oracle") suite.test("reference position fixtures", [&] {
        std::ifstream stream(argv[2]);
        check(bool(stream), "Cannot open oracle fixture");
        auto fixtures = Json::parse(stream);
        for (const auto& fixture : fixtures) {
            std::optional<Board> previous;
            if (!fixture["previous_board"].is_null()) previous = fixture["previous_board"].get<Board>();
            GameState state(fixture["board"].get<Board>(), fixture["to_play"], fixture["komi"], fixture["passes"], previous);
            check(state.legal_moves() == moves_from_json(fixture["legal_moves"]), "Legal move parity");
            check(state.score() == Score{fixture["score"][0], fixture["score"][1]}, "Score parity");
            check(state.to_string() == fixture["text"].get<std::string>(), "Text parity");
            for (const auto& action : fixture["successors"]) {
                auto move = moves_from_json(Json::array({action["move"]}))[0];
                auto next = state.play(move);
                check(next.board() == action["board"].get<Board>(), "Successor parity");
                check(next.to_play() == action["to_play"] && next.consecutive_passes() == action["passes"], "Turn/pass parity");
            }
        }
        std::cout << "Compared " << fixtures.size() << " reference positions.\n";
    });
    if (argc == 3 && std::string(argv[1]) == "--oracle") suite.test("original seeded game compatibility", [&] {
        auto path = std::filesystem::path(argv[2]).parent_path() / "seeded_games.json";
        auto data = load_records(path);
        for (const auto& record : data["games"]) {
            RandomAgent black(record["black_seed"].get<std::int64_t>()), white(record["white_seed"].get<std::int64_t>());
            auto result = run_game([&](const GameState& s) { return black.choose_move(s); },
                                   [&](const GameState& s) { return white.choose_move(s); },
                                   record["size"], record["komi"], record["max_moves"]);
            auto actual = result.to_json();
            for (auto key : {"moves", "score", "winner", "game_length", "termination_reason"})
                check(actual[key] == record[key], std::string("Seed compatibility: ") + key);
        }
    });
    std::cout << suite.passed << " passed, " << suite.failed << " failed\n";
    int search_failures = run_mcts_tests();
    return suite.failed || search_failures ? 1 : 0;
}
