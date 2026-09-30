#include "betago/options.hpp"
#include "betago/runner.hpp"
#include "betago/mcts.hpp"
#include <iomanip>
#include <iostream>
#include <limits>

namespace {
betago::Json search_record(const betago::SearchStatistics& stats) {
    using namespace betago;
    Json children = Json::array();
    for (const auto& child : stats.children) {
        Json move = nullptr;
        if (child.move) move = {child.move->row, child.move->column};
        children.push_back({{"move", move}, {"visits", child.visits},
            {"value_sum_for_child_player", child.value_sum}});
    }
    return {{"simulations", stats.simulations}, {"root_visits", stats.root_visits},
        {"root_value_sum", stats.root_value_sum}, {"completed_rollouts", stats.completed_rollouts},
        {"truncated_rollouts", stats.truncated_rollouts}, {"elapsed_seconds", stats.elapsed_seconds},
        {"simulations_per_second", stats.simulations_per_second()}, {"children", children}};
}
betago::Json settings_record(const betago::MctsSettings& settings) {
    return {{"simulations", settings.simulations}, {"exploration", settings.exploration},
        {"rollout_limit", settings.rollout_limit}, {"truncated_rollout_value", 0}};
}
}

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--size", "--komi", "--games", "--seed", "--max-moves", "--output", "--replay",
            "--black", "--white", "--simulations", "--rollout-limit", "--exploration"}, {"--help", "--benchmark"});
        if (args.has("--help")) {
            std::cout << "BetaGo game runner (random or classical MCTS)\n"
                      << "runner.exe [--size 9] [--komi 7.5] [--games 1] [--seed 0]\n"
                      << "           [--max-moves 500] [--output results/random_games.json]\n"
                      << "           [--black random|mcts] [--white random|mcts]\n"
                      << "           [--simulations 128] [--rollout-limit 200] [--exploration 1.4142135623730951]\n"
                      << "runner.exe --replay results/random_games.json\n"
                      << "runner.exe --benchmark [--size 9] [--simulations 128] [--seed 0]\n";
            return 0;
        }
        if (args.has("--replay")) {
            auto data = load_records(args.text("--replay"));
            int index = 0;
            for (const auto& record : data.at("games")) {
                std::cout << "Game " << ++index << ": " << describe(record) << '\n';
                std::cout << replay_moves(moves_from_json(record.at("moves")), record.at("size"), record.at("komi")).to_string() << '\n';
            }
            return 0;
        }
        MctsSettings settings{args.integer("--simulations", 128),
                              args.real("--exploration", 1.4142135623730951),
                              args.integer("--rollout-limit", 200)};
        settings.validate();
        auto seed = args.integer<std::int64_t>("--seed", 0);
        if (args.has("--benchmark")) {
            MctsAgent agent(settings, seed);
            auto state = GameState::new_game(args.integer("--size", 9), args.real("--komi", 7.5));
            auto move = agent.choose_move(state);
            const auto& stats = agent.last_search();
            Json record = search_record(stats);
            record["size"] = state.size(); record["komi"] = state.komi(); record["seed"] = seed;
            record["settings"] = settings_record(settings);
            record["selected_move"] = move ? Json::array({move->row, move->column}) : Json(nullptr);
            std::cout << stats.simulations << " simulations in " << std::fixed << std::setprecision(3)
                      << stats.elapsed_seconds << "s | " << std::setprecision(1) << stats.simulations_per_second()
                      << " simulations/s | " << stats.truncated_rollouts << " truncated rollouts\n";
            std::cout << "Selected " << (move ? "(" + std::to_string(move->row) + ", " + std::to_string(move->column) + ")" : "pass") << '\n';
            auto path = args.text("--output", "results/search_benchmark.json");
            save_records(path, record);
            std::cout << "Saved " << path << '\n';
            return 0;
        }
        auto black_name = args.text("--black", "random"), white_name = args.text("--white", "random");
        for (const auto& name : {black_name, white_name})
            if (name != "random" && name != "mcts") throw std::invalid_argument("Agent must be random or mcts");
        int games = args.integer("--games", 1);
        if (games < 1) throw std::invalid_argument("Number of games must be positive");
        if (seed > std::numeric_limits<std::int64_t>::max() - (2LL * games - 1))
            throw std::invalid_argument("Batch seeds exceed signed 64-bit range");
        Json records = Json::array();
        int completed = 0;
        for (int index = 0; index < games; ++index) {
            auto black_seed = seed + 2LL * index, white_seed = black_seed + 1;
            RandomAgent black(black_seed), white(white_seed);
            MctsAgent black_mcts(settings, black_seed), white_mcts(settings, white_seed);
            Json searches = Json::array();
            int action_index = 0;
            auto choose = [&](const GameState& state, const std::string& name, RandomAgent& random, MctsAgent& mcts) {
                ++action_index;
                if (name == "random") return random.choose_move(state);
                Move move = mcts.choose_move(state);
                Json search = search_record(mcts.last_search());
                search["move_number"] = action_index; search["player"] = state.to_play();
                searches.push_back(search);
                return move;
            };
            auto result = run_game([&](const GameState& s) { return choose(s, black_name, black, black_mcts); },
                                   [&](const GameState& s) { return choose(s, white_name, white, white_mcts); },
                                   args.integer("--size", 9), args.real("--komi", 7.5), args.integer("--max-moves", 500));
            Json record = result.to_json();
            record["black_seed"] = black_seed;
            record["white_seed"] = white_seed;
            if (!searches.empty()) record["searches"] = searches;
            records.push_back(record);
            completed += result.final_state.is_terminal();
            std::cout << "Game " << index + 1 << ": " << describe(record) << '\n';
            if (!searches.empty()) {
                std::int64_t rollouts = 0, truncated = 0;
                for (const auto& search : searches) {
                    rollouts += search["simulations"].get<int>(); truncated += search["truncated_rollouts"].get<int>();
                }
                std::cout << "  Search: " << rollouts << " simulations; " << truncated << " truncated rollouts\n";
            }
        }
        Json data = {{"schema_version", 1}, {"seed", seed},
            {"agents", {{"black", black_name}, {"white", white_name}}},
            {"rules", "simple ko, no suicide, area scoring, no dead-group adjudication"}, {"games", records}};
        if (black_name == "mcts" || white_name == "mcts") data["mcts_settings"] = settings_record(settings);
        auto output = args.text("--output", "results/random_games.json");
        save_records(output, data);
        std::cout << "Completed: " << completed << "; truncated: " << games - completed << "\nSaved " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
