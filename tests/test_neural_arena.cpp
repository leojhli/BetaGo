#include "betago/arena.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace betago;

void check(bool condition, const char* message = "Unexpected result") {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10)
        throw std::runtime_error("Unexpected numeric result");
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected rejection");
}
struct Suite {
    int passed = 0, failed = 0;
    template<class Function> void test(const char* name, Function function) {
        try { function(); ++passed; std::cout << "PASS NEURAL ARENA " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL NEURAL ARENA " << name << ": " << error.what() << '\n';
        }
    }
};

std::filesystem::path file(const char* name) { return std::filesystem::path("results/neural_arena_tests") / name; }
PolicyValueNetwork constant_model(int size, int preferred_action, std::int64_t seed = 17) {
    PolicyValueNetwork network({size, 1, 1}, seed);
    auto parameters = network.parameters();
    std::fill(parameters.begin(), parameters.end(), 0.0);
    for (const auto& block : network.parameter_blocks())
        if (block.name == "policy_bias") parameters.at(block.offset + preferred_action) = 4.0;
    network.set_parameters(std::move(parameters));
    return network;
}
std::string checkpoint(const char* name, int size = 1, int action = 1) {
    const auto path = file(name);
    constant_model(size, action).save(path);
    return path.string();
}
AgentConfiguration neural_config(const std::string& path, int simulations = 3) {
    return {"neural-mcts", {}, path, {simulations, 1.5}};
}
AgentConfiguration policy_config(const std::string& path) { return {"policy", {}, path, {}}; }
ArenaSettings small_arena(const std::string& path, int pairs = 1) {
    ArenaSettings settings;
    settings.a = neural_config(path);
    settings.b = policy_config(path);
    settings.pairs = pairs; settings.size = 1; settings.komi = 0.5; settings.max_moves = 2; settings.seed = -9;
    return settings;
}
Json without_timing(Json value) {
    if (value.is_object()) {
        for (const auto* key : {"elapsed_seconds", "decision_seconds", "mean_move_seconds",
                               "search_seconds", "simulations_per_second"}) value.erase(key);
        for (auto& entry : value.items()) entry.value() = without_timing(std::move(entry.value()));
    } else if (value.is_array()) {
        for (auto& child : value) child = without_timing(std::move(child));
    }
    return value;
}
const Json& total(const Json& data, const char* identity) { return data.at("summary").at("agents").at(identity); }
} // namespace

int run_neural_arena_tests() {
    Suite suite;
    suite.test("prepared models and identity survive overwriting their source checkpoint", [] {
        const auto path = checkpoint("snapshot.json", 3, 0);
        const auto prepared = prepare_agent(policy_config(path), 3);
        const auto identity = prepared.to_json();
        const auto parameters = prepared.network->parameters();
        const auto initial = GameState::new_game(3, 0.5);
        check(prepared.create(4)(initial).move == Move(Point{0, 0}));
        constant_model(3, 8, 99).save(path);
        check(prepared.create(4)(initial).move == Move(Point{0, 0}), "Prepared inference reloaded changed file");
        check(prepared.to_json() == identity && prepared.network->parameters() == parameters);
        check(!prepared.network->training() && prepared.network->training_steps() == 0);
        const auto replaced = prepare_agent(policy_config(path), 3);
        check(replaced.create(4)(initial).move == Move(Point{2, 2}));
        check(replaced.checkpoint_metadata.at("content_fingerprint") != prepared.checkpoint_metadata.at("content_fingerprint"));
        check(prepared.checkpoint_metadata.at("fingerprint_algorithm") == "fnv1a64");
        check(prepared.checkpoint_metadata.at("feature_schema") == FEATURE_SCHEMA);
        check(prepared.checkpoint_metadata.at("board_size") == 3 && prepared.checkpoint_metadata.at("channels") == 1);
    });
    suite.test("missing mismatched or malformed models reject before progress or agent creation", [] {
        const auto valid = checkpoint("valid_one.json");
        const auto mismatch = checkpoint("wrong_board.json", 3, 0);
        save_records(file("malformed.json"), Json{{"schema_version", 1}});
        std::error_code ignored;
        std::filesystem::remove(file("missing.json"), ignored);
        for (const auto& path : {file("missing.json").string(), mismatch, file("malformed.json").string()}) {
            auto settings = small_arena(valid);
            settings.b = policy_config(path);
            int progress = 0, agents = 0;
            rejects([&] {
                run_arena(settings, Json::object(), [&](int, const Json&) { ++progress; },
                    [&](const AgentConfiguration&, std::int64_t) -> ArenaAgent {
                        ++agents; return [](const GameState&) -> ArenaDecision { return {PASS, std::nullopt}; };
                    });
            });
            check(progress == 0 && agents == 0, "Bad checkpoint was discovered after play began");
        }
        auto missing = small_arena(valid); missing.a.checkpoint.clear();
        rejects([&] { run_arena(missing); });
        AgentConfiguration random; random.checkpoint = valid;
        rejects([&] { random.validate(); });
    });
    suite.test("paired policy and PUCT games preserve identity seeds and exact distinct work", [] {
        const auto path = checkpoint("paired.json");
        const auto data = run_arena(small_arena(path, 2));
        check(data.at("games").size() == 4 && data.at("summary").at("complete_pairs") == 2);
        const auto& a = total(data, "a"); const auto& b = total(data, "b");
        check(a.at("moves") == 4 && a.at("searches") == 4 && a.at("simulations") == 12);
        check(a.at("neural_searches") == 4 && a.at("network_evaluations") == 6 && a.at("terminal_evaluations") == 10);
        check(a.at("completed_rollouts") == 0 && a.at("truncated_rollouts") == 0 && a.at("rollout_truncation_rate").is_null());
        check(b.at("moves") == 4 && b.at("searches") == 0 && b.at("network_evaluations") == 4);
        check(b.at("terminal_evaluations") == 0 && b.at("simulations") == 0);
        for (std::size_t index = 0; index < data.at("games").size(); ++index) {
            const auto& game = data.at("games").at(index);
            check(game.at("a_seed") == -9 + 2 * static_cast<int>(index / 2));
            check(game.at("b_seed") == -8 + 2 * static_cast<int>(index / 2));
            check(game.at("black_agent") == (index % 2 ? "b" : "a"));
            for (const auto& decision : game.at("decisions")) {
                if (decision.at("agent") == "a") {
                    const auto& search = decision.at("search");
                    check(search.at("algorithm") == "puct" && search.at("simulations") == 3 && search.at("root_visits") == 3);
                    check(search.at("network_evaluations").get<int>() + search.at("terminal_evaluations").get<int>() == 4);
                    check(search.at("children").size() == 1 && search.at("children").at(0).at("prior") == 1.0);
                } else {
                    check(!decision.contains("search") && decision.at("network_evaluations") == 1);
                    check(decision.at("policy").at("probabilities") == Json::array({0.0, 1.0}));
                    close(decision.at("policy").at("value_for_player_to_move"), 0);
                }
            }
            check(game.at("winner") == WHITE);
        }
        check(data.at("agents").at("a").at("checkpoint_identity") == data.at("agents").at("b").at("checkpoint_identity"));
    });
    suite.test("each neural identity gets its own recorded PUCT budget and immutable model", [] {
        const auto first = checkpoint("budget_a.json");
        constant_model(1, 1, 29).save(file("budget_b.json"));
        const auto second = file("budget_b.json").string();
        auto settings = small_arena(first);
        settings.a.neural = {2, 0.7}; settings.b = neural_config(second, 7);
        const auto data = run_arena(settings);
        check(total(data, "a").at("simulations") == 4 && total(data, "b").at("simulations") == 14);
        check(data.at("agents").at("a").at("search").at("c_puct") == 0.7);
        check(data.at("agents").at("b").at("checkpoint_identity").at("initialization_seed") == 29);
        for (const auto& game : data.at("games")) for (const auto& decision : game.at("decisions"))
            check(decision.at("search").at("simulations") == (decision.at("agent") == "a" ? 2 : 7));
    });
    suite.test("rollout cutoff rates divide by classical work when neural search is also present", [] {
        const auto path = checkpoint("mixed_work.json");
        auto games = run_arena(small_arena(path)).at("games");
        SearchStatistics classical;
        classical.simulations = 10; classical.root_visits = 10;
        classical.completed_rollouts = 8; classical.truncated_rollouts = 2; classical.elapsed_seconds = 2;
        games.at(0).at("decisions").at(0)["search"] = search_statistics_json(classical);
        const auto summary = summarize_arena(games);
        const auto& a = summary.at("agents").at("a");
        check(a.at("simulations") == 13 && a.at("rollout_simulations") == 10);
        close(a.at("rollout_truncation_rate"), 0.2);
        check(a.at("neural_searches") == 1 && a.at("network_evaluations") == 1 && a.at("terminal_evaluations") == 3);
    });
    suite.test("neural counter validation rejects contradictory or overflow-prone records", [] {
        const auto path = checkpoint("counter_validation.json");
        const auto original = run_arena(small_arena(path)).at("games");
        for (const auto& invalid : {
                Json{{"network_evaluations", 0}, {"terminal_evaluations", 0}},
                Json{{"network_evaluations", -1}}, Json{{"terminal_evaluations", 5}},
                Json{{"network_evaluations", "one"}}, Json{{"completed_rollouts", 1}},
                Json{{"truncated_rollouts", 1}}, Json{{"algorithm", "mystery"}},
                Json{{"simulations", std::numeric_limits<std::int64_t>::max()}}}) {
            auto games = original;
            auto& search = games.at(0).at("decisions").at(0).at("search");
            for (const auto& entry : invalid.items()) search[entry.key()] = entry.value();
            rejects([&] { summarize_arena(games); });
        }
    });
    suite.test("neural seeded actions statistics outcomes and model identities repeat exactly", [] {
        const auto path = checkpoint("repeat.json", 3, 0);
        auto settings = small_arena(path, 2); settings.size = 3; settings.max_moves = 8;
        settings.a.neural.simulations = 5;
        const auto first = run_arena(settings), second = run_arena(settings);
        check(without_timing(first) == without_timing(second), "Repeated inference/search changed saved behavior");
    });
    suite.test("neural arena records retain schema-one replay compatibility", [] {
        const auto path = checkpoint("replay_model.json", 3, 0);
        auto settings = small_arena(path); settings.size = 3; settings.max_moves = 5;
        const auto data = run_arena(settings);
        save_records(file("arena_records.json"), data);
        check(load_records(file("arena_records.json")) == data);
        for (const auto& game : data.at("games")) {
            const auto state = replay_moves(moves_from_json(game.at("moves")), settings.size, settings.komi);
            check(state.is_terminal() == (game.at("termination_reason") == "two_passes"));
        }
    });
    suite.test("classical record keys keep their existing meanings and format", [] {
        SearchStatistics statistics;
        statistics.simulations = 1; statistics.root_visits = 1; statistics.completed_rollouts = 1;
        statistics.children.push_back({PASS, 1, 0.0});
        const auto search = search_statistics_json(statistics);
        check(!search.contains("algorithm") && !search.contains("network_evaluations") && !search.contains("terminal_evaluations"));
        check(!search.at("children").at(0).contains("prior"));
        ArenaSettings settings;
        settings.a = {"mcts", {2, 1, 0}}; settings.b.kind = "random";
        settings.pairs = 1; settings.size = 1; settings.max_moves = 2;
        const auto data = run_arena(settings);
        check(data.at("agents").at("a").size() == 2 && data.at("agents").at("b").size() == 1);
        check(!total(data, "a").contains("network_evaluations"));
        const auto a = total(data, "a");
        close(a.at("rollout_truncation_rate"), a.at("truncated_rollouts").get<double>() / a.at("simulations").get<double>());
        save_records(file("classical_records.json"), data);
        check(load_records(file("classical_records.json")) == data);
    });
    std::cout << suite.passed << " neural arena tests passed, " << suite.failed << " failed\n";
    return suite.failed;
}
