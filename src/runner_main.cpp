#include "betago/options.hpp"
#include "betago/runner.hpp"
#include "betago/mcts.hpp"
#include "betago/arena.hpp"
#include "build_info.hpp"
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
bool uses_checkpoint(const std::string& kind) { return kind == "policy" || kind == "neural-mcts"; }

bool same_file_path(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code error;
    if (std::filesystem::equivalent(first, second, error) && !error) return true;
    auto normalized = [](const std::filesystem::path& path) {
        std::error_code failure;
        auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(path), failure);
        if (failure) absolute = std::filesystem::absolute(path).lexically_normal();
        auto text = absolute.generic_wstring();
#if defined(_WIN32)
        for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
#endif
        return text;
    };
    return normalized(first) == normalized(second);
}

void protect_checkpoints(const std::string& output,
                         std::initializer_list<betago::AgentConfiguration> configurations) {
    for (const auto& configuration : configurations) {
        if (!configuration.checkpoint.empty() && same_file_path(output, configuration.checkpoint))
            throw std::invalid_argument("--output must not overwrite an input checkpoint");
        if (configuration.external_gtp) {
            const auto& external = *configuration.external_gtp;
            for (const auto& input : {external.profile_path, external.executable})
                if (!input.empty() && same_file_path(output, input))
                    throw std::invalid_argument("--output must not overwrite an external profile or executable");
            for (const auto& file : external.files)
                if (same_file_path(output, file.path))
                    throw std::invalid_argument("--output must not overwrite a declared external configuration/model");
        }
    }
}

betago::AgentConfiguration agent_configuration(const betago::Options& args, const std::string& kind,
                                               const betago::MctsSettings& settings,
                                               const std::string& checkpoint_option,
                                               const std::string& puct_option = "",
                                               const std::string& simulations_option = "",
                                               const std::string& exploration_option = "",
                                               const std::string& rollout_option = "",
                                               const std::string& gtp_profile_option = "") {
    using namespace betago;
    AgentConfiguration result;
    result.kind = kind;
    if (kind == "external-gtp") {
        if (gtp_profile_option.empty() || !args.has(gtp_profile_option))
            throw std::invalid_argument("external-gtp requires an arena --a-gtp-profile or --b-gtp-profile");
        for (const auto& option : {simulations_option, exploration_option, rollout_option})
            if (!option.empty() && args.has(option))
                throw std::invalid_argument(option + " requires a BetaGo search agent");
        result.external_gtp = load_external_gtp_profile(args.text(gtp_profile_option));
    } else if (!gtp_profile_option.empty() && args.has(gtp_profile_option))
        throw std::invalid_argument(gtp_profile_option + " requires an external-gtp agent");
    if (uses_checkpoint(kind)) {
        for (const auto& option : {exploration_option, rollout_option})
            if (!option.empty() && args.has(option))
                throw std::invalid_argument(option + " requires a classical mcts agent");
        if (kind == "policy" && !simulations_option.empty() && args.has(simulations_option))
            throw std::invalid_argument(simulations_option + " requires an mcts or neural-mcts agent");
    }
    result.search = {simulations_option.empty() ? settings.simulations : args.integer(simulations_option, settings.simulations),
        exploration_option.empty() ? settings.exploration : args.real(exploration_option, settings.exploration),
        rollout_option.empty() ? settings.rollout_limit : args.integer(rollout_option, settings.rollout_limit)};
    if (args.has(checkpoint_option) && !uses_checkpoint(kind))
        throw std::invalid_argument(checkpoint_option + " requires a policy or neural-mcts agent");
    if (args.has(checkpoint_option)) result.checkpoint = args.text(checkpoint_option);
    else if (uses_checkpoint(kind)) result.checkpoint = args.text("--checkpoint");
    if (!puct_option.empty() && args.has(puct_option) && kind != "neural-mcts")
        throw std::invalid_argument(puct_option + " requires a neural-mcts agent");
    result.neural = {result.search.simulations,
        puct_option.empty() ? args.real("--c-puct", 1.5) : args.real(puct_option, args.real("--c-puct", 1.5))};
    result.validate();
    return result;
}

void validate_common_search_options(const betago::Options& args,
                                   std::initializer_list<betago::AgentConfiguration> agents) {
    bool has_classical = false, has_neural = false, has_search = false, has_external = false;
    for (const auto& agent : agents) {
        has_classical = has_classical || agent.kind == "mcts";
        has_neural = has_neural || uses_checkpoint(agent.kind);
        has_search = has_search || agent.kind == "mcts" || agent.kind == "neural-mcts";
        has_external = has_external || agent.kind == "external-gtp";
    }
    if ((has_neural || has_external) && !has_classical)
        for (const auto* option : {"--exploration", "--rollout-limit"})
            if (args.has(option)) throw std::invalid_argument(std::string(option) + " requires a classical mcts agent");
    if ((has_neural || has_external) && !has_search && args.has("--simulations"))
        throw std::invalid_argument("--simulations requires an mcts or neural-mcts agent");
}

void validate_common_neural_options(const betago::Options& args,
                                   std::initializer_list<std::pair<betago::AgentConfiguration, std::string>> agents) {
    bool checkpoint_used = false, puct_used = false;
    for (const auto& [configuration, checkpoint_option] : agents) {
        checkpoint_used = checkpoint_used || (uses_checkpoint(configuration.kind) && !args.has(checkpoint_option));
        const std::string override = checkpoint_option == "--a-checkpoint" ? "--a-c-puct" :
                                     checkpoint_option == "--b-checkpoint" ? "--b-c-puct" : "";
        puct_used = puct_used || (configuration.kind == "neural-mcts" && (override.empty() || !args.has(override)));
    }
    if (args.has("--checkpoint") && !checkpoint_used)
        throw std::invalid_argument("--checkpoint is unused; select a neural agent without a checkpoint override");
    if (args.has("--c-puct") && !puct_used)
        throw std::invalid_argument("--c-puct is unused; select a neural-mcts agent without a PUCT override");
}

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
    if (summary.contains("attempted_games"))
        std::cout << "Attempted: " << summary.at("attempted_games") << "; engine failures: " << summary.at("failed_games")
                  << "; unscored resignations: " << summary.at("resigned_games")
                  << "; incomplete color pairs: " << summary.at("incomplete_pairs")
                  << "; cleanup warnings: " << summary.value("cleanup_warning_count", 0) << '\n';
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
                  << number_or_na(agent.at("simulations"), 0) << " simulations; "
                  << number_or_na(agent.at("simulations_per_second"), 1) << " simulations/s; "
                  << number_or_na(agent.at("truncated_rollouts"), 0) << " rollout cutoffs\n";
        if (agent.contains("network_evaluations") && !agent.at("network_evaluations").is_null())
            std::cout << "  Neural work: " << number_or_na(agent.at("network_evaluations"), 0) << " network evaluations; "
                      << number_or_na(agent.at("terminal_evaluations"), 0) << " exact terminal evaluations\n";
        for (const auto* measure : {"win", "score"}) {
            const auto& interval = agent.at(std::string("paired_") + measure + "_rate_95");
            std::cout << "  Paired " << measure << " rate: ";
            if (interval.is_null()) std::cout << "n/a (" << summary.value("uncertainty_exclusion_reason", "no complete pairs") << ")\n";
            else std::cout << number_or_na(interval.at("estimate")) << "; 95% bound ["
                           << number_or_na(interval.at("lower")) << ", " << number_or_na(interval.at("upper"))
                           << "] from " << interval.at("sample_size") << " pairs\n";
        }
    }
    std::cout << "Bounds use complete pairs only; move-limit exclusions can bias comparisons.\n";
    if (summary.contains("attempted_games"))
        std::cout << "Configuration-specific comparison; reference labels are unvalidated metadata. No kyu/dan estimate.\n";
}
}

int main(int argc, char** argv) {
    using namespace betago;
    try {
        const std::set<std::string> valued{"--size", "--komi", "--games", "--seed", "--max-moves", "--output", "--replay",
            "--black", "--white", "--simulations", "--rollout-limit", "--exploration",
            "--agent-a", "--agent-b", "--pairs", "--a-simulations", "--b-simulations",
            "--a-rollout-limit", "--b-rollout-limit", "--a-exploration", "--b-exploration",
            "--agent", "--checkpoint", "--black-checkpoint", "--white-checkpoint",
            "--a-checkpoint", "--b-checkpoint", "--c-puct", "--a-c-puct", "--b-c-puct",
            "--a-gtp-profile", "--b-gtp-profile"};
        Options args(argc, argv, valued, {"--help", "--benchmark", "--arena"});
        if (args.has("--help")) {
            std::cout << "BetaGo game runner (random, classical MCTS, policy, neural MCTS, or external GTP arena)\n"
                      << "runner.exe [--size 9] [--komi 7.5] [--games 1] [--seed 0]\n"
                      << "           [--max-moves 500] [--output results/random_games.json]\n"
                      << "           [--black random|mcts|policy|neural-mcts] [--white random|mcts|policy|neural-mcts]\n"
                      << "           [--checkpoint results/policy_value.json] [--black-checkpoint path] [--white-checkpoint path]\n"
                      << "           [--simulations 128] [--rollout-limit 200] [--exploration 1.4142135623730951]\n"
                      << "runner.exe --replay results/random_games.json\n"
                      << "runner.exe --benchmark [--agent mcts|policy|neural-mcts] [--size 9] [--simulations 128] [--seed 0]\n"
                      << "runner.exe --arena [--agent-a mcts] [--agent-b random] [--pairs 5]\n"
                      << "           [--size 3] [--komi 7.5] [--max-moves 100] [--seed 0]\n"
                      << "           [--a-simulations 128] [--b-simulations 128] [--output results/arena.json]\n"
                      << "           [--a-rollout-limit 200] [--b-rollout-limit 200]\n"
                      << "           [--a-exploration 1.4142135623730951] [--b-exploration 1.4142135623730951]\n"
                      << "           [--a-checkpoint path] [--b-checkpoint path] [--c-puct 1.5] [--a-c-puct 1.5] [--b-c-puct 1.5]\n"
                      << "           [--agent-a external-gtp --a-gtp-profile profiles/engine.json]\n"
                      << "           [--agent-b external-gtp --b-gtp-profile profiles/engine.json]\n"
                      << "Each pair plays both color assignments with fixed identity seeds.\n"
                      << "Common search/checkpoint settings are fallbacks for each agent. Neural agents require a matching checkpoint.\n"
                      << "Policy selects the highest legal prior; neural-mcts uses PUCT and network leaf values without rollouts.\n";
            return 0;
        }
        if (int(args.has("--arena")) + int(args.has("--benchmark")) + int(args.has("--replay")) > 1)
            throw std::invalid_argument("Choose one of --arena, --benchmark, or --replay");
        if (!args.has("--arena")) {
            for (const auto* key : {"--agent-a", "--agent-b", "--pairs", "--a-simulations", "--b-simulations",
                                   "--a-rollout-limit", "--b-rollout-limit", "--a-exploration", "--b-exploration",
                                   "--a-checkpoint", "--b-checkpoint", "--a-c-puct", "--b-c-puct",
                                   "--a-gtp-profile", "--b-gtp-profile"})
                if (args.has(key)) throw std::invalid_argument(std::string(key) + " requires --arena");
        } else {
            for (const auto* key : {"--games", "--black", "--white", "--black-checkpoint", "--white-checkpoint"})
                if (args.has(key)) throw std::invalid_argument(std::string(key) + " cannot be used with --arena; use --pairs and --agent-a/--agent-b");
        }
        if (!args.has("--benchmark") && args.has("--agent"))
            throw std::invalid_argument("--agent requires --benchmark; use --black/--white or --agent-a/--agent-b");
        if (args.has("--benchmark")) {
            for (const auto* key : {"--games", "--black", "--white", "--max-moves", "--black-checkpoint", "--white-checkpoint"})
                if (args.has(key)) throw std::invalid_argument(std::string(key) + " cannot be used with --benchmark");
        }
        if (args.has("--replay")) {
            for (const auto& key : valued)
                if (key != "--replay" && args.has(key))
                    throw std::invalid_argument(key + " cannot be used with --replay");
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
            arena.a = agent_configuration(args, args.text("--agent-a", "mcts"), settings, "--a-checkpoint",
                "--a-c-puct", "--a-simulations", "--a-exploration", "--a-rollout-limit", "--a-gtp-profile");
            arena.b = agent_configuration(args, args.text("--agent-b", "random"), settings, "--b-checkpoint",
                "--b-c-puct", "--b-simulations", "--b-exploration", "--b-rollout-limit", "--b-gtp-profile");
            validate_common_neural_options(args, {{arena.a, "--a-checkpoint"}, {arena.b, "--b-checkpoint"}});
            validate_common_search_options(args, {arena.a, arena.b});
            arena.pairs = args.integer("--pairs", 5); arena.size = args.integer("--size", 3);
            arena.max_moves = args.integer("--max-moves", 100); arena.komi = args.real("--komi", 7.5); arena.seed = seed;
            arena.validate();
            auto output = args.text("--output", "results/arena.json");
            protect_checkpoints(output, {arena.a, arena.b});
            const bool external = arena.a.kind == "external-gtp" || arena.b.kind == "external-gtp";
            std::cout << "A: " << arena.a.kind << "; B: " << arena.b.kind << "; " << arena.pairs
                      << " color pairs on " << arena.size << 'x' << arena.size << '\n' << std::flush;
            auto data = run_arena(arena, arena_metadata(), [](int index, const Json& record) {
                std::cout << "Game " << index + 1 << " (pair " << record.at("pair_index").get<int>() + 1
                          << ", Black " << (record.at("black_agent") == "a" ? "A" : "B") << "): "
                          << describe(record) << '\n' << std::flush;
            }, {}, external ? ArenaSnapshot([&](const Json& snapshot) { save_records_atomic(output, snapshot); }) : ArenaSnapshot{});
            if (external) save_records_atomic(output, data);
            else save_records(output, data);
            print_arena_summary(data.at("summary"));
            std::cout << "Saved " << output << '\n';
            return external && (data.at("summary").at("failed_games").get<int>() ||
                                data.at("summary").at("resigned_games").get<int>()) ? 2 : 0;
        }
        if (args.has("--benchmark")) {
            const auto kind = args.text("--agent", "mcts");
            if (kind == "random") throw std::invalid_argument("Benchmark agent must be mcts, policy, or neural-mcts");
            const auto configuration = agent_configuration(args, kind, settings, "--checkpoint");
            validate_common_search_options(args, {configuration});
            if (args.has("--c-puct") && kind != "neural-mcts")
                throw std::invalid_argument("--c-puct requires a neural-mcts agent");
            auto path = args.text("--output", "results/search_benchmark.json");
            protect_checkpoints(path, {configuration});
            auto state = GameState::new_game(args.integer("--size", 9), args.real("--komi", 7.5));
            auto prepared = prepare_agent(configuration, state.size());
            auto agent = prepared.create(seed);
            auto started = std::chrono::steady_clock::now();
            const auto decision = agent(state);
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const auto move = decision.move;
            Json record = decision.search ? search_statistics_json(*decision.search) : Json::object();
            record["size"] = state.size(); record["komi"] = state.komi(); record["seed"] = seed;
            record["settings"] = kind == "neural-mcts" ? neural_mcts_settings_json(configuration.neural) :
                                  kind == "mcts" ? mcts_settings_json(settings) : Json::object();
            if (uses_checkpoint(kind)) {
                record["agent"] = prepared.to_json();
                record["decision_seconds"] = elapsed;
            }
            if (decision.prediction) {
                record["policy"] = prediction_json(*decision.prediction);
                record["network_evaluations"] = 1;
            }
            record["selected_move"] = move ? Json::array({move->row, move->column}) : Json(nullptr);
            if (decision.search) {
                const auto& stats = *decision.search;
                std::cout << stats.simulations << " simulations in " << std::fixed << std::setprecision(3)
                          << stats.elapsed_seconds << "s | " << std::setprecision(1) << stats.simulations_per_second()
                          << " simulations/s";
                if (kind == "neural-mcts")
                    std::cout << " | " << stats.network_evaluations << " network / " << stats.terminal_evaluations << " terminal evaluations\n";
                else std::cout << " | " << stats.truncated_rollouts << " truncated rollouts\n";
            } else std::cout << "Policy inference in " << elapsed << "s\n";
            std::cout << "Selected " << (move ? "(" + std::to_string(move->row) + ", " + std::to_string(move->column) + ")" : "pass") << '\n';
            save_records(path, record);
            std::cout << "Saved " << path << '\n';
            return 0;
        }
        auto black_name = args.text("--black", "random"), white_name = args.text("--white", "random");
        const auto black_configuration = agent_configuration(args, black_name, settings, "--black-checkpoint");
        const auto white_configuration = agent_configuration(args, white_name, settings, "--white-checkpoint");
        validate_common_neural_options(args, {{black_configuration, "--black-checkpoint"}, {white_configuration, "--white-checkpoint"}});
        validate_common_search_options(args, {black_configuration, white_configuration});
        int games = args.integer("--games", 1);
        if (games < 1) throw std::invalid_argument("Number of games must be positive");
        if (seed > std::numeric_limits<std::int64_t>::max() - (2LL * games - 1))
            throw std::invalid_argument("Batch seeds exceed signed 64-bit range");
        const auto output = args.text("--output", "results/random_games.json");
        protect_checkpoints(output, {black_configuration, white_configuration});
        const int size = args.integer("--size", 9), max_moves = args.integer("--max-moves", 500);
        const double komi = args.real("--komi", 7.5);
        if (max_moves < 1) throw std::invalid_argument("Move limit must be positive");
        const auto prepared_black = prepare_agent(black_configuration, size);
        const auto prepared_white = prepare_agent(white_configuration, size);
        Json records = Json::array();
        int completed = 0;
        for (int index = 0; index < games; ++index) {
            auto black_seed = seed + 2LL * index, white_seed = black_seed + 1;
            auto black = prepared_black.create(black_seed), white = prepared_white.create(white_seed);
            Json searches = Json::array();
            Json policies = Json::array();
            int action_index = 0;
            auto choose = [&](const GameState& state, ArenaAgent& agent) {
                ++action_index;
                const auto decision = agent(state);
                if (decision.search) {
                    Json search = search_statistics_json(*decision.search);
                    search["move_number"] = action_index; search["player"] = state.to_play();
                    searches.push_back(std::move(search));
                }
                if (decision.prediction) {
                    Json policy = prediction_json(*decision.prediction);
                    policy["move_number"] = action_index; policy["player"] = state.to_play();
                    policy["network_evaluations"] = 1;
                    policies.push_back(std::move(policy));
                }
                return decision.move;
            };
            auto result = run_game([&](const GameState& s) { return choose(s, black); },
                                   [&](const GameState& s) { return choose(s, white); }, size, komi, max_moves);
            Json record = result.to_json();
            record["black_seed"] = black_seed;
            record["white_seed"] = white_seed;
            if (!searches.empty()) record["searches"] = searches;
            if (!policies.empty()) record["policies"] = policies;
            records.push_back(record);
            completed += result.final_state.is_terminal();
            std::cout << "Game " << index + 1 << ": " << describe(record) << '\n';
            if (!searches.empty()) {
                std::int64_t simulations = 0, truncated = 0, network = 0, terminal = 0;
                for (const auto& search : searches) {
                    simulations += search["simulations"].get<int>(); truncated += search["truncated_rollouts"].get<int>();
                    network += search.value("network_evaluations", 0); terminal += search.value("terminal_evaluations", 0);
                }
                std::cout << "  Search: " << simulations << " simulations; " << truncated << " truncated rollouts";
                if (network || terminal) std::cout << "; " << network << " network / " << terminal << " terminal evaluations";
                std::cout << '\n';
            }
        }
        Json data = {{"schema_version", 1}, {"seed", seed},
            {"agents", {{"black", black_name}, {"white", white_name}}},
            {"rules", "simple ko, no suicide, area scoring, no dead-group adjudication"}, {"games", records}};
        if (black_name == "mcts" || white_name == "mcts") data["mcts_settings"] = mcts_settings_json(settings);
        if (uses_checkpoint(black_name) || uses_checkpoint(white_name))
            data["agent_configurations"] = {{"black", prepared_black.to_json()}, {"white", prepared_white.to_json()}};
        save_records(output, data);
        std::cout << "Completed: " << completed << "; truncated: " << games - completed << "\nSaved " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
