#include "betago/sgf.hpp"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
using namespace betago;
void sgf_check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void sgf_rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument was not thrown");
}
struct SgfSuite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS SGF " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL SGF " << name << ": " << error.what() << '\n';
        }
    }
};
ExpertGame sgf_game(std::string_view body = ";B[aa];W[bb]", const char* result = "B+R") {
    std::string text = "(;FF[4]GM[1]SZ[3]KM[0.5]RE[";
    text += result; text += ']'; text += body; text += ')';
    return parse_sgf(text, 3, "fixture.sgf").front();
}
GameState sgf_final(const ExpertGame& game) {
    auto state = game.initial_state;
    for (Move move : game.moves) state = state.play(move);
    return state;
}
} // namespace

int run_sgf_tests() {
    SgfSuite suite;
    suite.test("coordinates are column then row and recorded results retain both perspectives", [] {
        auto game = sgf_game(";B[ba];W[ac]", "W+Resign");
        sgf_check(game.moves == std::vector<Move>{Point{0, 1}, Point{2, 0}});
        sgf_check(game.initial_state.size() == 3 && game.initial_state.komi() == 0.5 && game.winner == WHITE);
        sgf_check(game.source == "fixture.sgf" && game.metadata.at("mainline_only") == true &&
                  game.metadata.at("rules_mismatch_possible") == true);
        auto state = game.initial_state;
        sgf_check((game.winner == state.to_play() ? 1 : -1) == -1);
        state = state.play(game.moves[0]);
        sgf_check((game.winner == state.to_play() ? 1 : -1) == 1);
        sgf_check(state.previous_board() == game.initial_state.board(), "Replay must preserve simple-ko history");
        sgf_check(sgf_final(game).at({2, 0}) == WHITE);
        const auto number_forms = parse_sgf("(;FF[+04]GM[01]SZ[+03]KM[+0.5]RE[W+R];B[ba];W[ac])", 3).front();
        sgf_check(expert_game_id(number_forms) == expert_game_id(game));
    });
    suite.test("capture replay removes surrounded groups before testing suicide", [] {
        const auto game = sgf_game(";B[ba];W[bb];B[ab];W[cc];B[bc];W[ca];B[cb]", "B+9");
        const auto final = sgf_final(game);
        sgf_check(final.at({1, 1}) == EMPTY && final.at({0, 2}) == EMPTY && final.at({2, 2}) == EMPTY);
        sgf_check(final.at({1, 2}) == BLACK && final.to_play() == WHITE);
        sgf_check(final.previous_board() && final.previous_board()->at(1).at(1) == WHITE);
    });
    suite.test("immediate ko recapture rejects but legal threats preserve later recapture", [] {
        const std::string before = "(;FF[4]GM[1]SZ[5]KM[0]RE[B+R]"
            ";B[ba];W[ca];B[ab];W[bb];B[bc];W[db];B[ee];W[cc];B[cb]";
        sgf_rejects([&] { parse_sgf(before + ";W[bb])", 5); });
        const auto game = parse_sgf(before + ";W[ae];B[be];W[bb])", 5).front();
        const auto final = sgf_final(game);
        sgf_check(final.at({1, 2}) == EMPTY && final.at({1, 1}) == WHITE);
    });
    suite.test("blank and legacy tt passes terminate after two passes", [] {
        const auto game = parse_sgf("(;FF[4]GM[1]SZ[9]KM[7.5]RE[W+7.5];B[tt];W[])").front();
        sgf_check(game.moves == std::vector<Move>{PASS, PASS});
        sgf_check(sgf_final(game).is_terminal() && sgf_final(game).winner() == WHITE);
        const auto big = parse_sgf("(;SZ[19]KM[0]RE[0];B[tt];W[])", 19).front();
        sgf_check(big.winner == EMPTY && !sgf_final(big).winner());
        sgf_rejects([] { parse_sgf("(;SZ[3]KM[0]RE[0];B[];W[];B[aa])", 3); });
    });
    suite.test("collections follow the first variation without importing side branches", [] {
        const auto games = parse_sgf("(;FF[4]GM[1]SZ[3]KM[0.5]RE[B+R];B[aa]"
            "(;W[bb](;B[cc])(;B[ac]))(;W[aa]))"
            "\n(;SZ[3]KM[0]RE[Draw];B[];W[])", 3, "collection.sgf");
        sgf_check(games.size() == 2);
        sgf_check(games[0].moves == std::vector<Move>{Point{0, 0}, Point{1, 1}, Point{2, 2}});
        sgf_check(games[1].winner == EMPTY && games[1].metadata.at("collection_index") == 1);
        sgf_check(games[0].metadata.at("ignored_variations") == 2);
        sgf_check(games[0].metadata.at("source_fingerprint_fnv1a64") == games[1].metadata.at("source_fingerprint_fnv1a64"));
    });
    suite.test("escaped brackets backslashes and soft line breaks do not change the tree", [] {
        const auto text = "(; FF[4] GM[1] SZ[3] KM[+0.5] RE[B+R] "
            "PB[A\\]lice\\\\Name] PW[Bob\\\r\n Smith] RU[Japanese] "
            "C[comment with \\] and (; parentheses; \\\\ escaped slash] "
            ";B[aa]C[continued\\\ncomment];W[bb]TR[aa][bb])";
        const auto game = parse_sgf(text, 3).front();
        sgf_check(game.moves == std::vector<Move>{Point{0, 0}, Point{1, 1}});
        sgf_check(game.metadata.at("PB") == "A]lice\\Name");
        sgf_check(game.metadata.at("PW") == "Bob Smith" && game.metadata.at("RU") == "Japanese");
        sgf_check(game.metadata.at("RE") == "B+R");
    });
    suite.test("content identity ignores comments paths markup and winner margin but keeps winner", [] {
        const auto first = sgf_game(";B[aa];W[bb]", "B+R");
        const auto second = parse_sgf("(;SZ[3]KM[0.5]RE[B+12.5]C[other comments]PB[Other]"
                                     ";B[aa]TR[aa];W[bb])", 3, "renamed.sgf").front();
        sgf_check(expert_game_id(first) == expert_game_id(second));
        sgf_check(first.metadata.at("source_fingerprint_fnv1a64") != second.metadata.at("source_fingerprint_fnv1a64"));
        auto changed = first; changed.winner = WHITE;
        sgf_check(expert_game_id(first) != expert_game_id(changed));
        changed = first; changed.moves[1] = Point{2, 2};
        sgf_check(expert_game_id(first) != expert_game_id(changed));
    });
    suite.test("UTF-8 and legacy Latin-1 names produce valid provenance JSON", [] {
        std::string text = "(;SZ[3]KM[0.5]RE[B+R]PB[Andr";
        text.push_back(static_cast<char>(0xe9)); text += "];B[aa])";
        const auto latin = parse_sgf(text, 3).front();
        sgf_check(latin.metadata.at("PB") == "Andr\xc3\xa9");
        sgf_check(expert_game_from_json(expert_game_json(latin)).metadata == latin.metadata);
        const auto utf8 = parse_sgf("(;CA[UTF-8]SZ[3]KM[0.5]RE[B+R]PB[Andr\xc3\xa9];B[aa])", 3).front();
        sgf_check(utf8.metadata.at("PB") == latin.metadata.at("PB"));
        sgf_check(expert_game_id(utf8) == expert_game_id(latin));
        text.insert(2, "CA[UTF-8]");
        sgf_rejects([&] { parse_sgf(text, 3); });
        sgf_rejects([] { parse_sgf("(;CA[Shift_JIS]SZ[3]KM[0]RE[B+R];B[aa])", 3); });
    });
    suite.test("explicit finite komi and known result are required instead of guessed", [] {
        for (const auto* text : {"(;SZ[3]RE[B+R];B[aa])", "(;SZ[3]KM[0.5];B[aa])",
             "(;SZ[3]KM[nan]RE[B+R];B[aa])", "(;SZ[3]KM[inf]RE[B+R];B[aa])",
             "(;SZ[3]KM[1e2]RE[B+R];B[aa])", "(;SZ[3]KM[1.]RE[B+R];B[aa])",
             "(;SZ[3]KM[0.5]RE[?];B[aa])", "(;SZ[3]KM[0.5]RE[Void];B[aa])",
             "(;SZ[3]KM[0.5]RE[B+T];B[aa])", "(;SZ[3]KM[0.5]RE[W+Forfeit];B[aa])",
             "(;SZ[3]KM[0.5]RE[B+-1];B[aa])", "(;SZ[3]KM[0.5]RE[W+0];B[aa])"})
            sgf_rejects([&] { parse_sgf(text, 3); });
        sgf_check(sgf_game(";B[aa]", "B+").winner == BLACK);
        sgf_check(sgf_game(";B[aa]", "W+2.5").winner == WHITE);
    });
    suite.test("board size is exact and unsupported sizes or games cannot be cropped", [] {
        for (const auto* text : {"(;SZ[19]KM[0.5]RE[B+R];B[aa])", "(;SZ[0]KM[0]RE[B+R];B[aa])",
             "(;SZ[20]KM[0]RE[B+R];B[aa])", "(;SZ[3:3]KM[0]RE[B+R];B[aa])",
             "(;KM[0]RE[B+R];B[aa])", "(;FF[3]SZ[3]KM[0]RE[B+R];B[aa])",
             "(;GM[2]SZ[3]KM[0]RE[B+R];B[aa])"})
            sgf_rejects([&] { parse_sgf(text, 3); });
        sgf_rejects([] { parse_sgf("(;SZ[3]KM[0]RE[B+R];B[aa])", 0); });
        sgf_rejects([] { parse_sgf("(;SZ[3]KM[0]RE[B+R];B[aa])", 20); });
        sgf_check(parse_sgf("(;SZ[1]KM[0]RE[0];B[];W[])", 1).front().initial_state.size() == 1);
    });
    suite.test("setup handicap turn overrides and later critical properties reject", [] {
        for (const auto* property : {"AB[aa]", "AW[bb]", "AE[aa]", "HA[2]", "PL[B]", "KO[]"}) {
            const auto root = std::string("(;SZ[3]KM[0.5]RE[B+R]") + property + ";B[aa])";
            sgf_rejects([&] { parse_sgf(root, 3); });
            const auto later = std::string("(;SZ[3]KM[0.5]RE[B+R];B[aa]") + property + ')';
            sgf_rejects([&] { parse_sgf(later, 3); });
        }
        for (const auto* property : {"KM[0]", "RE[W+R]", "SZ[9]", "GM[1]", "FF[4]", "RU[Chinese]"}) {
            const auto text = std::string("(;SZ[3]KM[0.5]RE[B+R];B[aa]") + property + ')';
            sgf_rejects([&] { parse_sgf(text, 3); });
        }
    });
    suite.test("illegal occupancy suicide coordinates and nonalternating actions reject", [] {
        for (const auto* body : {";W[aa]", ";B[aa];B[bb]", ";B[aa];W[aa]", ";B[dd]",
                                ";B[a]", ";B[AA]", ";B[aa]W[bb]", ";B[aa][bb]"})
            sgf_rejects([&] { sgf_game(body); });
        sgf_rejects([] { parse_sgf("(;SZ[1]KM[0]RE[B+R];B[aa])", 1); });
    });
    suite.test("malformed syntax and malformed ignored variations reject", [] {
        for (const auto* text : {"", "()", ";SZ[3]", "(;SZ[3]KM[0]RE[B+R];B[aa]",
             "(;SZ[3]KM[0]RE[B+R];B[aa]))", "(;SZ[3]KM[0]RE[B+R]C[unclosed)",
             "(;SZ[3]SZ[3]KM[0]RE[B+R];B[aa])", "(;SZ[3]KM[0]RE[B+R];B[aa]bad)",
             "(;SZ[3]KM[0]RE[B+R];B[aa](;W[bb])(;W[bb))"})
            sgf_rejects([&] { parse_sgf(text, 3); });
        std::string nul = "(;SZ[3]KM[0]RE[B+R]C["; nul.push_back('\0'); nul += "];B[aa])";
        sgf_rejects([&] { parse_sgf(nul, 3); });
    });
    suite.test("input depth and node caps also apply to skipped variations", [] {
        std::string oversized(32U * 1024U * 1024U + 1, ' ');
        sgf_rejects([&] { parse_sgf(oversized, 3); });
        std::string deep = "(;SZ[3]KM[0]RE[B+R];B[aa](;W[bb])";
        for (int i = 0; i < 256; ++i) deep += "(;";
        deep.append(257, ')');
        sgf_rejects([&] { parse_sgf(deep, 3); });
        std::string nodes = "(;SZ[3]KM[0]RE[B+R];B[aa](;W[bb])(";
        nodes.append(200000, ';'); nodes += "))";
        sgf_rejects([&] { parse_sgf(nodes, 3); });
    });
    suite.test("raw JSON roundtrip preserves initial position previous board and legal history", [] {
        auto state = GameState::new_game(3, -0.5).play(Point{0, 0});
        ExpertGame game{state, {Point{1, 1}, Point{2, 2}}, WHITE, "continuation", {{"teacher", true}}};
        const auto record = expert_game_json(game);
        const auto loaded = expert_game_from_json(record);
        sgf_check(loaded.initial_state == state && loaded.initial_state.previous_board() == state.previous_board());
        sgf_check(loaded.moves == game.moves && loaded.winner == WHITE && loaded.source == game.source &&
                  loaded.metadata == game.metadata && expert_game_json(loaded) == record);
        sgf_check(sgf_final(loaded) == sgf_final(game));
        sgf_rejects([&] { expert_game_sgf(game); });
    });
    suite.test("JSON content edits and malformed raw states fail strict replay or identity", [] {
        const auto original = expert_game_json(sgf_game());
        for (const auto* field : {"initial_state", "moves", "winner", "source", "metadata", "id", "schema_version", "kind"}) {
            auto record = original; record.erase(field);
            sgf_rejects([&] { expert_game_from_json(record); });
        }
        auto record = original; record["moves"][1] = Json::array({0, 0});
        sgf_rejects([&] { expert_game_from_json(record); });
        record = original; record["winner"] = WHITE;
        sgf_rejects([&] { expert_game_from_json(record); });
        record = original; record["initial_state"]["previous_board"] = Json::array({Json::array({0})});
        sgf_rejects([&] { expert_game_from_json(record); });
        record = original; record["initial_state"]["board"][0][0] = 1.0;
        sgf_rejects([&] { expert_game_from_json(record); });
        record = original; record["winner"] = std::numeric_limits<std::uint64_t>::max();
        sgf_rejects([&] { expert_game_from_json(record); });
        record = original; record["metadata"] = Json::array();
        sgf_rejects([&] { expert_game_from_json(record); });
    });
    suite.test("SGF export roundtrips teacher games labels names and local score margins", [] {
        auto game = parse_sgf("(;FF[4]GM[1]SZ[3]KM[0.5]RE[W+0.5]PB[A\\]\\\\B]"
                              "PW[White]RU[BetaGo];B[];W[])", 3).front();
        const auto text = expert_game_sgf(game);
        sgf_check(text.find("RE[W+0.5]") != std::string::npos);
        const auto loaded = parse_sgf(text, 3).front();
        sgf_check(loaded.initial_state == game.initial_state && loaded.moves == game.moves && loaded.winner == game.winner);
        sgf_check(loaded.metadata.at("PB") == game.metadata.at("PB") && loaded.metadata.at("PW") == "White");
        sgf_check(expert_game_id(loaded) == expert_game_id(game));
        game = sgf_game(";B[aa];W[bb]", "B+R");
        sgf_check(expert_game_id(parse_sgf(expert_game_sgf(game), 3).front()) == expert_game_id(game));
        for (const double komi : {1e-25, 1e25, -0.0, std::numeric_limits<double>::denorm_min()}) {
            game.initial_state = GameState::new_game(3, komi);
            const auto roundtrip = parse_sgf(expert_game_sgf(game), 3).front();
            sgf_check(roundtrip.initial_state.komi() == komi, "Finite decimal komi must roundtrip exactly");
        }
    });
    std::cout << suite.passed << " SGF tests passed, " << suite.failed << " failed\n";
    return suite.failed;
}
