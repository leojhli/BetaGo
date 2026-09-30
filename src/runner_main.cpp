#include "betago/options.hpp"
#include "betago/runner.hpp"
#include "betago/mcts.hpp"
#include "betago/arena.hpp"
#include "build_info.hpp"
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
betago::Json environment_value(const char* name) {
    auto value = std::getenv(name);
    return value && *value ? betago::Json(value) : betago::Json(nullptr);
}
betago::Json arena_metadata() {
    using namespace betago;
#if defined(_WIN32)
    const char* operating_system = "Windows";
#elif defined(__APPLE__)
    const char* operating_system = "macOS";
#elif defined(__linux__)
    const char* operating_system = "Linux";
#else
    const char* operating_system = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char* architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* architecture = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* architecture = "x86";
#else
    const char* architecture = "unknown";
#endif
    Json hostname = environment_value("COMPUTERNAME");
    if (hostname.is_null()) hostname = environment_value("HOSTNAME");
    auto threads = std::thread::hardware_concurrency();
    std::time_t now = std::time(nullptr);
    std::ostringstream timestamp;
    if (const auto* utc = std::gmtime(&now)) timestamp << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");
#ifdef __VERSION__
    const char* compiler_version = __VERSION__;
#else
    const char* compiler_version = build_info::compiler;
#endif
    return {{"recorded_at_utc", timestamp.str()},
        {"machine", {{"hostname", hostname}, {"os", operating_system}, {"architecture", architecture},
            {"cpu_identifier", environment_value("PROCESSOR_IDENTIFIER")},
            {"hardware_threads", threads ? Json(threads) : Json(nullptr)}}},
        {"build", {{"git_revision", build_info::git_available ? Json(build_info::revision) : Json(nullptr)},
            {"git_dirty", build_info::git_available ? Json(build_info::dirty) : Json(nullptr)},
            {"source_sha256", build_info::source_sha256}, {"built_at_utc", build_info::built_at_utc},
            {"compiler", build_info::compiler}, {"compiler_version", compiler_version},
            {"profile", BETAGO_BUILD_PROFILE}, {"flags", build_info::flags}, {"cpp_standard", 20}}}};
}

std::string number_or_na(const betago::Json& value, int precision = 3) {
    if (value.is_null()) return "n/a";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value.get<double>();
    return out.str();
}

void print_arena_summary(const betago::Json& summary) {
    std::cout << "Completed: " << summary.at("completed_games") << "; truncated: " << summary.at("truncated_games")
              << "; complete color pairs: " << summary.at("complete_pairs") << '\n';
    std::cout << "Mean game length: " << number_or_na(summary.at("mean_game_length"), 1) << " moves\n";
    for (const auto* identity : {"a", "b"}) {
        const auto& agent = summary.at("agents").at(identity);
        std::cout << (identity[0] == 'a' ? "A" : "B") << ": " << agent.at("wins") << " wins / "
                  << agent.at("losses") << " losses / " << agent.at("draws") << " draws / "
                  << agent.at("truncated_games") << " truncated\n";
        std::cout << "  Completed-game win rate: " << number_or_na(agent.at("win_rate"))
                  << "; score rate: " << number_or_na(agent.at("score_rate")) << '\n';
        for (const auto* color : {"black", "white"}) {
            const auto& record = agent.at("by_color").at(color);
            std::cout << "  As " << color << ": " << record.at("wins") << '/' << record.at("losses")
                      << '/' << record.at("draws") << " W/L/D, " << record.at("truncated_games") << " truncated\n";
        }
        std::cout << "  Mean move: " << number_or_na(agent.at("mean_move_seconds")) << "s; "
                  << agent.at("simulations") << " simulations; "
                  << number_or_na(agent.at("simulations_per_second"), 1) << " simulations/s; "
                  << agent.at("truncated_rollouts") << " rollout cutoffs\n";
        for (const auto* measure : {"win", "score"}) {
            const auto& interval = agent.at(std::string("paired_") + measure + "_rate_95");
            std::cout << "  Paired " << measure << " rate: ";
            if (interval.is_null()) std::cout << "n/a (no complete pairs)\n";
            else std::cout << number_or_na(interval.at("estimate")) << "; 95% bound ["
                           << number_or_na(interval.at("lower")) << ", " << number_or_na(interval.at("upper"))
                           << "] from " << interval.at("sample_size") << " pairs\n";
        }
    }
    std::cout << "Bounds use complete pairs only; move-limit exclusions can bias comparisons.\n";
}
}

int main(int argc, char** argv) {
    using namespace betago;
    try {
        Options args(argc, argv, {"--size", "--komi", "--games", "--seed", "--max-moves", "--output", "--replay",
            "--black", "--white", "--simulations", "--rollout-limit", "--exploration",
            "--agent-a", "--agent-b", "--pairs", "--a-simulations", "--b-simulations",
            "--a-rollout-limit", "--b-rollout-limit", "--a-exploration", "--b-exploration"},
            {"--help", "--benchmark", "--arena"});
        if (args.has("--help")) {
            std::cout << "BetaGo game runner (random or classical MCTS)\n"
                      << "runner.exe [--size 9] [--komi 7.5] [--games 1] [--seed 0]\n"
                      << "           [--max-moves 500] [--output results/random_games.json]\n"
                      << "           [--black random|mcts] [--white random|mcts]\n"
                      << "           [--simulations 128] [--rollout-limit 200] [--exploration 1.4142135623730951]\n"
                      << "runner.exe --replay results/random_games.json\n"
                      << "runner.exe --benchmark [--size 9] [--simulations 128] [--seed 0]\n"
                      << "runner.exe --arena [--agent-a mcts] [--agent-b random] [--pairs 5]\n"
                      << "           [--size 3] [--komi 7.5] [--max-moves 100] [--seed 0]\n"
                      << "           [--a-simulations 128] [--b-simulations 128] [--output results/arena.json]\n"
                      << "           [--a-rollout-limit 200] [--b-rollout-limit 200]\n"
                      << "           [--a-exploration 1.4142135623730951] [--b-exploration 1.4142135623730951]\n"
                      << "Each pair plays both color assignments with fixed identity seeds.\n"
                      << "Common search settings are fallbacks for each agent's settings.\n";
            return 0;
        }
        if (int(args.has("--arena")) + int(args.has("--benchmark")) + int(args.has("--replay")) > 1)
            throw std::invalid_argument("Choose one of --arena, --benchmark, or --replay");
        if (!args.has("--arena")) {
            for (const auto* key : {"--agent-a", "--agent-b", "--pairs", "--a-simulations", "--b-simulations",
                                   "--a-rollout-limit", "--b-rollout-limit", "--a-exploration", "--b-exploration"})
                if (args.has(key)) throw std::invalid_argument(std::string(key) + " requires --arena");
        } else {
            for (const auto* key : {"--games", "--black", "--white"})
                if (args.has(key)) throw std::invalid_argument(std::string(key) + " cannot be used with --arena; use --pairs and --agent-a/--agent-b");
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
        if (args.has("--arena")) {
            ArenaSettings arena;
            arena.a = {args.text("--agent-a", "mcts"),
                {args.integer("--a-simulations", settings.simulations), args.real("--a-exploration", settings.exploration),
                 args.integer("--a-rollout-limit", settings.rollout_limit)}};
            arena.b = {args.text("--agent-b", "random"),
                {args.integer("--b-simulations", settings.simulations), args.real("--b-exploration", settings.exploration),
                 args.integer("--b-rollout-limit", settings.rollout_limit)}};
            arena.pairs = args.integer("--pairs", 5); arena.size = args.integer("--size", 3);
            arena.max_moves = args.integer("--max-moves", 100); arena.komi = args.real("--komi", 7.5); arena.seed = seed;
            arena.validate();
            std::cout << "A: " << arena.a.kind << "; B: " << arena.b.kind << "; " << arena.pairs
                      << " color pairs on " << arena.size << 'x' << arena.size << '\n' << std::flush;
            auto data = run_arena(arena, arena_metadata(), [](int index, const Json& record) {
                std::cout << "Game " << index + 1 << " (pair " << record.at("pair_index").get<int>() + 1
                          << ", Black " << (record.at("black_agent") == "a" ? "A" : "B") << "): "
                          << describe(record) << '\n' << std::flush;
            });
            auto output = args.text("--output", "results/arena.json");
            save_records(output, data);
            print_arena_summary(data.at("summary"));
            std::cout << "Saved " << output << '\n';
            return 0;
        }
        if (args.has("--benchmark")) {
            MctsAgent agent(settings, seed);
            auto state = GameState::new_game(args.integer("--size", 9), args.real("--komi", 7.5));
            auto move = agent.choose_move(state);
            const auto& stats = agent.last_search();
            Json record = search_statistics_json(stats);
            record["size"] = state.size(); record["komi"] = state.komi(); record["seed"] = seed;
            record["settings"] = mcts_settings_json(settings);
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
                Json search = search_statistics_json(mcts.last_search());
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
        if (black_name == "mcts" || white_name == "mcts") data["mcts_settings"] = mcts_settings_json(settings);
        auto output = args.text("--output", "results/random_games.json");
        save_records(output, data);
        std::cout << "Completed: " << completed << "; truncated: " << games - completed << "\nSaved " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
