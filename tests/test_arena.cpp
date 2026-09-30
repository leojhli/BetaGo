#include "betago/arena.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {
using namespace betago;

void arena_check(bool condition, const std::string& message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}

void arena_close(double actual, double expected, const std::string& message = "Unexpected number") {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10)
        throw std::runtime_error(message + ": " + std::to_string(actual));
}

template<class Function> void arena_rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument was not thrown");
}

struct ArenaSuite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS ARENA " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL ARENA " << name << ": " << error.what() << '\n';
        }
    }
};

ArenaSettings pass_settings(int pairs = 2, double komi = 1) {
    ArenaSettings settings;
    settings.a.kind = settings.b.kind = "random";
    settings.pairs = pairs;
    settings.size = 1;
    settings.komi = komi;
    settings.max_moves = 2;
    settings.seed = 17;
    return settings;
}

ArenaAgentFactory pass_factory() {
    return [](const AgentConfiguration&, std::int64_t) -> ArenaAgent {
        return [](const GameState&) -> ArenaDecision { return {PASS, std::nullopt}; };
    };
}

Json pass_arena(int pairs = 2, double komi = 1) {
    return run_arena(pass_settings(pairs, komi), Json::object(), {}, pass_factory());
}

// Timing is deliberately excluded from deterministic comparisons. Everything
// involved in actions, budgets, outcomes, and seeds must still match exactly.
Json without_timing(Json value) {
    if (value.is_object()) {
        for (const auto* key : {"elapsed_seconds", "decision_seconds", "mean_move_seconds",
                               "search_seconds", "simulations_per_second"}) value.erase(key);
        for (auto& child : value.items()) child.value() = without_timing(std::move(child.value()));
    } else if (value.is_array()) {
        for (auto& child : value) child = without_timing(std::move(child));
    }
    return value;
}

const Json& total(const Json& summary, const char* id) { return summary.at("agents").at(id); }
const Json& color_total(const Json& summary, const char* id, const char* color) {
    return total(summary, id).at("by_color").at(color);
}

void outcome_counts(const Json& value, int games, int completed, int truncated,
                    int wins, int losses, int draws) {
    arena_check(value.at("games") == games && value.at("completed_games") == completed &&
                value.at("truncated_games") == truncated && value.at("wins") == wins &&
                value.at("losses") == losses && value.at("draws") == draws,
                "Outcome counts disagree with the hand-checked result");
}

void truncate_game(Json& record, int limit = 1) {
    // A first pass is unfinished, even on the otherwise forced 1x1 board.
    const auto state = GameState::new_game(1, record.at("komi").get<double>()).play(PASS);
    auto truncated = GameResult{state, {PASS}, limit, 3}.to_json();
    for (auto& entry : truncated.items()) record[entry.key()] = entry.value();
    record["decisions"].erase(record["decisions"].begin() + 1, record["decisions"].end());
}
} // namespace

int run_arena_tests() {
    ArenaSuite suite;
    suite.test("both colors use fresh agents and the same identity seeds within a pair", [] {
        auto settings = pass_settings(2);
        std::vector<std::pair<std::int64_t, int>> calls;
        int builds = 0;
        auto factory = [&](const AgentConfiguration&, std::int64_t seed) -> ArenaAgent {
            ++builds;
            return [&, seed, decisions = 0](const GameState&) mutable -> ArenaDecision {
                calls.emplace_back(seed, ++decisions);
                return {PASS, std::nullopt};
            };
        };
        const auto data = run_arena(settings, Json::object(), {}, factory);
        const auto& games = data.at("games");
        arena_check(builds == 8 && calls.size() == 8,
                    "Each of four games must create two new agent instances");
        arena_check(calls == std::vector<std::pair<std::int64_t, int>>{
            {17, 1}, {18, 1}, {18, 1}, {17, 1}, {19, 1}, {20, 1}, {20, 1}, {19, 1}},
            "RNG identity must follow the agent across the color swap and reset for each game");
        for (int index = 0; index < 4; ++index) {
            const auto& game = games.at(index);
            arena_check(game.at("pair_index") == index / 2 && game.at("game_in_pair") == index % 2 + 1);
            arena_check(game.at("black_agent") == (index % 2 == 0 ? "a" : "b"));
            arena_check(game.at("white_agent") == (index % 2 == 0 ? "b" : "a"));
            arena_check(game.at("a_seed") == 17 + 2 * (index / 2));
            arena_check(game.at("b_seed") == 18 + 2 * (index / 2));
            arena_check(game.at("black_seed") == (index % 2 == 0 ? game.at("a_seed") : game.at("b_seed")));
            arena_check(game.at("white_seed") == (index % 2 == 0 ? game.at("b_seed") : game.at("a_seed")));
        }
    });
    suite.test("White wins under positive komi with hand-checked identity and color totals", [] {
        const auto data = pass_arena();
        const auto& summary = data.at("summary");
        arena_check(summary.at("games") == 4 && summary.at("completed_games") == 4 &&
                    summary.at("truncated_games") == 0 && summary.at("complete_pairs") == 2);
        for (const auto* id : {"a", "b"}) {
            outcome_counts(total(summary, id), 4, 4, 0, 2, 2, 0);
            outcome_counts(color_total(summary, id, "black"), 2, 2, 0, 0, 2, 0);
            outcome_counts(color_total(summary, id, "white"), 2, 2, 0, 2, 0, 0);
            arena_close(total(summary, id).at("win_rate"), 0.5);
            arena_close(total(summary, id).at("score_rate"), 0.5);
        }
        for (const auto& game : data.at("games")) {
            arena_check(game.at("winner") == WHITE && game.at("termination_reason") == "two_passes");
            arena_close(game.at("score").at("black"), 0);
            arena_close(game.at("score").at("white"), 1);
        }
    });
    suite.test("Black wins under negative komi without attributing wins to the wrong identity", [] {
        const auto data = pass_arena(2, -1);
        const auto& summary = data.at("summary");
        for (const auto* id : {"a", "b"}) {
            outcome_counts(total(summary, id), 4, 4, 0, 2, 2, 0);
            outcome_counts(color_total(summary, id, "black"), 2, 2, 0, 2, 0, 0);
            outcome_counts(color_total(summary, id, "white"), 2, 2, 0, 0, 2, 0);
        }
        for (const auto& game : data.at("games")) arena_check(game.at("winner") == BLACK);
    });
    suite.test("actual draws differ from wins and contribute half a score point", [] {
        const Json owned = pass_arena(2, 0).at("summary");
        for (const auto* id : {"a", "b"}) {
            outcome_counts(total(owned, id), 4, 4, 0, 0, 0, 4);
            arena_close(total(owned, id).at("win_rate"), 0);
            arena_close(total(owned, id).at("score_rate"), 0.5);
            arena_close(total(owned, id).at("paired_win_rate_95").at("estimate"), 0);
            arena_close(total(owned, id).at("paired_score_rate_95").at("estimate"), 0.5);
        }
    });
    suite.test("a paired win plus draw gives distinct win and score estimates", [] {
        auto games = pass_arena(1, -1).at("games");
        auto drawn = pass_arena(1, 0).at("games").at(1);
        games.at(1) = std::move(drawn);
        const auto summary = summarize_arena(games);
        outcome_counts(total(summary, "a"), 2, 2, 0, 1, 0, 1);
        outcome_counts(total(summary, "b"), 2, 2, 0, 0, 1, 1);
        arena_close(total(summary, "a").at("win_rate"), 0.5);
        arena_close(total(summary, "a").at("score_rate"), 0.75);
        arena_close(total(summary, "a").at("paired_win_rate_95").at("estimate"), 0.5);
        arena_close(total(summary, "a").at("paired_score_rate_95").at("estimate"), 0.75);
        arena_close(total(summary, "b").at("paired_win_rate_95").at("estimate"), 0);
        arena_close(total(summary, "b").at("paired_score_rate_95").at("estimate"), 0.25);
    });
    suite.test("move-limit games stay unscored and produce no win loss or draw", [] {
        auto settings = pass_settings(); settings.max_moves = 1;
        const auto data = run_arena(settings, Json::object(), {}, pass_factory());
        const auto& summary = data.at("summary");
        arena_check(summary.at("completed_games") == 0 && summary.at("truncated_games") == 4 &&
                    summary.at("complete_pairs") == 0 && summary.at("incomplete_pairs") == 2);
        for (const auto* id : {"a", "b"}) {
            outcome_counts(total(summary, id), 4, 0, 4, 0, 0, 0);
            arena_check(total(summary, id).at("win_rate").is_null() &&
                        total(summary, id).at("score_rate").is_null());
            arena_check(total(summary, id).at("paired_win_rate_95").is_null() &&
                        total(summary, id).at("paired_score_rate_95").is_null());
        }
        for (const auto& game : data.at("games"))
            arena_check(game.at("score").is_null() && game.at("winner").is_null() &&
                        game.at("termination_reason") == "move_limit");
    });
    suite.test("uncertainty counts complete color pairs and excludes a partially completed pair", [] {
        auto games = pass_arena().at("games");
        truncate_game(games.at(1));
        const auto summary = summarize_arena(games);
        arena_check(summary.at("completed_games") == 3 && summary.at("truncated_games") == 1 &&
                    summary.at("complete_pairs") == 1 && summary.at("incomplete_pairs") == 1);
        outcome_counts(total(summary, "a"), 4, 3, 1, 1, 2, 0);
        outcome_counts(total(summary, "b"), 4, 3, 1, 2, 1, 0);
        arena_close(total(summary, "a").at("win_rate"), 1.0 / 3);
        const auto& interval = total(summary, "a").at("paired_win_rate_95");
        arena_check(interval.at("sample_size") == 1);
        arena_close(interval.at("estimate"), 0.5);
        arena_close(interval.at("lower"), 0);
        arena_close(interval.at("upper"), 1);
        arena_close(summary.at("mean_game_length"), 1.75);
        arena_close(summary.at("mean_completed_game_length"), 2);
    });
    suite.test("small-sample Hoeffding bounds are wide and clipped to valid rates", [] {
        const auto data = pass_arena(1);
        for (const auto* id : {"a", "b"}) {
            const auto& interval = total(data.at("summary"), id).at("paired_win_rate_95");
            arena_check(interval.at("sample_size") == 1);
            arena_close(interval.at("confidence"), 0.95);
            arena_close(interval.at("estimate"), 0.5);
            arena_close(interval.at("lower"), 0);
            arena_close(interval.at("upper"), 1);
        }
    });
    suite.test("Hoeffding radius uses independent pairs rather than twice as many games", [] {
        const auto template_games = pass_arena(1).at("games");
        Json games = Json::array();
        for (int pair = 0; pair < 100; ++pair) {
            for (auto game : template_games) {
                game["pair_index"] = pair;
                games.push_back(std::move(game));
            }
        }
        const auto summary = summarize_arena(games);
        const double radius = std::sqrt(std::log(40.0) / 200.0);
        arena_check(summary.at("games") == 200 && summary.at("complete_pairs") == 100);
        for (const auto* id : {"a", "b"}) {
            const auto& interval = total(summary, id).at("paired_win_rate_95");
            arena_check(interval.at("sample_size") == 100);
            arena_close(interval.at("lower"), 0.5 - radius);
            arena_close(interval.at("upper"), 0.5 + radius);
        }
    });
    suite.test("move timing and search throughput aggregate counts instead of per-search rates", [] {
        auto games = pass_arena(1).at("games");
        // Agent A searches twice: 10 simulations / 2s, then 30 / 3s.
        // Aggregate throughput is 40 / 5 = 8, not the mean of 5 and 10.
        int a_index = 0;
        for (auto& game : games) {
            for (auto& decision : game.at("decisions")) {
                if (decision.at("agent") == "a") {
                    SearchStatistics statistics;
                    statistics.simulations = statistics.root_visits = a_index == 0 ? 10 : 30;
                    statistics.elapsed_seconds = a_index == 0 ? 2 : 3;
                    statistics.completed_rollouts = a_index == 0 ? 9 : 15;
                    statistics.truncated_rollouts = a_index == 0 ? 1 : 15;
                    decision["search"] = search_statistics_json(statistics);
                    decision["elapsed_seconds"] = a_index == 0 ? 4 : 8;
                    ++a_index;
                } else decision["elapsed_seconds"] = 2;
            }
        }
        const auto summary = summarize_arena(games);
        const auto& a = total(summary, "a");
        arena_check(a.at("moves") == 2 && a.at("searches") == 2 && a.at("simulations") == 40 &&
                    a.at("completed_rollouts") == 24 && a.at("truncated_rollouts") == 16);
        arena_close(a.at("decision_seconds"), 12);
        arena_close(a.at("mean_move_seconds"), 6);
        arena_close(a.at("search_seconds"), 5);
        arena_close(a.at("simulations_per_second"), 8);
        arena_close(a.at("rollout_truncation_rate"), 0.4);
        const auto& b = total(summary, "b");
        arena_check(b.at("searches") == 0 && b.at("simulations") == 0);
        arena_close(b.at("mean_move_seconds"), 2);
    });
    suite.test("seeded MCTS versus random repeats actions budgets and outcomes exactly", [] {
        ArenaSettings settings;
        settings.pairs = 2; settings.size = 3; settings.komi = 0.5; settings.max_moves = 12;
        settings.seed = -73; settings.a.search = {4, std::sqrt(2.0), 2};
        const auto first = run_arena(settings);
        const auto second = run_arena(settings);
        arena_check(without_timing(first.at("games")) == without_timing(second.at("games")),
                    "Repeated seeded games disagree outside of runtime");
        arena_check(without_timing(first.at("summary")) == without_timing(second.at("summary")));
        const auto& summary = first.at("summary");
        const auto& a = total(summary, "a");
        arena_check(a.at("searches") == a.at("moves"));
        arena_check(a.at("simulations").get<int>() == 4 * a.at("searches").get<int>());
        arena_check(a.at("simulations").get<int>() ==
                    a.at("completed_rollouts").get<int>() + a.at("truncated_rollouts").get<int>());
        for (const auto& game : first.at("games")) {
            const auto moves = moves_from_json(game.at("moves"));
            const auto initial = GameState::new_game(settings.size, settings.komi);
            const auto unchanged = initial;
            auto state = initial;
            arena_check(game.at("decisions").size() == moves.size());
            for (std::size_t index = 0; index < moves.size(); ++index) {
                const auto& decision = game.at("decisions").at(index);
                arena_check(decision.at("move_number") == index + 1 && decision.at("player") == state.to_play());
                arena_check(decision.at("agent") == game.at(state.to_play() == BLACK ? "black_agent" : "white_agent"));
                const auto legal = state.legal_moves();
                arena_check(std::find(legal.begin(), legal.end(), moves[index]) != legal.end());
                state = state.play(moves[index]);
                if (decision.at("agent") == "a") {
                    const auto& search = decision.at("search");
                    arena_check(search.at("simulations") == 4 && search.at("root_visits") == 4);
                    arena_check(search.at("completed_rollouts").get<int>() +
                                search.at("truncated_rollouts").get<int>() == 4);
                } else arena_check(!decision.contains("search"));
            }
            arena_check(initial == unchanged && state == replay_moves(moves, settings.size, settings.komi));
            const auto expected = GameResult{state, moves, settings.max_moves, 0}.to_json();
            for (const auto* key : {"game_length", "termination_reason", "winner", "score"})
                arena_check(game.at(key) == expected.at(key));
        }
    });
    suite.test("each distinct MCTS budget is recorded and honored for both colors", [] {
        ArenaSettings settings;
        settings.pairs = 1; settings.size = 1; settings.komi = 0; settings.max_moves = 2;
        settings.a = {"mcts", {3, 1, 0}};
        settings.b = {"mcts", {7, 2, 0}};
        const auto data = run_arena(settings);
        const auto& summary = data.at("summary");
        arena_check(total(summary, "a").at("searches") == 2 && total(summary, "a").at("simulations") == 6);
        arena_check(total(summary, "b").at("searches") == 2 && total(summary, "b").at("simulations") == 14);
        for (const auto& game : data.at("games")) {
            for (const auto& decision : game.at("decisions")) {
                const int budget = decision.at("agent") == "a" ? 3 : 7;
                arena_check(decision.at("search").at("simulations") == budget);
            }
        }
    });
    suite.test("progress emits each completed game in saved order and metadata survives", [] {
        std::vector<Json> seen;
        auto settings = pass_settings();
        const Json metadata{{"code_version", "test revision"}, {"machine", "test machine"}};
        const auto data = run_arena(settings, metadata,
            [&](int index, const Json& record) {
                arena_check(index == static_cast<int>(seen.size()));
                seen.push_back(record);
            }, pass_factory());
        arena_check(Json(seen) == data.at("games"));
        arena_check(data.at("metadata") == metadata);
        arena_check(data.at("schema_version") == 1);
    });
    suite.test("arena validates positive experiment dimensions and finite komi", [] {
        for (int invalid : {0, -1}) {
            auto settings = pass_settings(); settings.pairs = invalid;
            arena_rejects([&] { settings.validate(); });
            settings = pass_settings(); settings.size = invalid;
            arena_rejects([&] { settings.validate(); });
            settings = pass_settings(); settings.max_moves = invalid;
            arena_rejects([&] { settings.validate(); });
        }
        auto oversized = pass_settings();
        oversized.pairs = std::numeric_limits<int>::max() / 2;
        oversized.validate();
        oversized.pairs = std::numeric_limits<int>::max() / 2 + 1;
        arena_rejects([&] { oversized.validate(); });
        for (double invalid : {std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
            auto settings = pass_settings(); settings.komi = invalid;
            arena_rejects([&] { settings.validate(); });
        }
    });
    suite.test("unknown agents and invalid per-agent search settings reject before play", [] {
        for (bool first : {false, true}) {
            auto settings = pass_settings();
            (first ? settings.a : settings.b).kind = "unknown";
            arena_rejects([&] { settings.validate(); });
            settings = pass_settings();
            auto& agent = first ? settings.a : settings.b;
            agent.kind = "mcts";
            for (const auto invalid : {MctsSettings{0, 1, 2}, MctsSettings{4, -1, 2},
                                       MctsSettings{4, std::numeric_limits<double>::infinity(), 2},
                                       MctsSettings{4, 1, -1}}) {
                agent.search = invalid;
                arena_rejects([&] { settings.validate(); });
            }
        }
        auto settings = pass_settings(); int calls = 0;
        settings.a.kind = "unknown";
        arena_rejects([&] {
            run_arena(settings, Json::object(), {},
                [&](const AgentConfiguration&, std::int64_t) -> ArenaAgent {
                    ++calls; return pass_factory()({}, 0);
                });
        });
        arena_check(calls == 0, "Invalid settings must reject before creating any agents");
    });
    suite.test("signed seed range checks the final identity seed without off-by-one overflow", [] {
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        auto settings = pass_settings(1); settings.seed = maximum - 1;
        settings.validate();
        const auto data = run_arena(settings, Json::object(), {}, pass_factory());
        arena_check(data.at("games").at(1).at("a_seed") == maximum - 1 &&
                    data.at("games").at(1).at("b_seed") == maximum);
        settings.seed = maximum; arena_rejects([&] { settings.validate(); });
        settings = pass_settings(2); settings.seed = maximum - 3; settings.validate();
        settings.seed = maximum - 2; arena_rejects([&] { settings.validate(); });
        settings = pass_settings(1); settings.seed = std::numeric_limits<std::int64_t>::min();
        settings.validate();
    });
    suite.test("aggregation rejects duplicated pairs and contradictory decision or rollout records", [] {
        const auto original = pass_arena(1).at("games");
        auto games = original;
        games.push_back(games.at(0));
        arena_rejects([&] { summarize_arena(games); });
        games = original;
        games.at(1)["black_agent"] = "a"; games.at(1)["white_agent"] = "b";
        games.at(1)["decisions"] = games.at(0)["decisions"];
        arena_rejects([&] { summarize_arena(games); });
        games = original;
        games.at(0)["decisions"].at(0)["agent"] = "b";
        arena_rejects([&] { summarize_arena(games); });
        games = original;
        SearchStatistics inconsistent;
        inconsistent.simulations = 10; inconsistent.completed_rollouts = 8;
        inconsistent.truncated_rollouts = 3;
        games.at(0)["decisions"].at(0)["search"] = search_statistics_json(inconsistent);
        arena_rejects([&] { summarize_arena(games); });
        games = original;
        games.at(0)["decisions"].at(0)["elapsed_seconds"] = -1;
        arena_rejects([&] { summarize_arena(games); });
    });
    std::cout << suite.passed << " arena tests passed, " << suite.failed << " failed\n";
    return suite.failed;
}
