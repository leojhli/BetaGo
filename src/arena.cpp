#include "betago/arena.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace betago {
namespace {
using Count = std::int64_t;

Json ratio(double numerator, double denominator) {
    return denominator > 0 ? Json(numerator / denominator) : Json(nullptr);
}

Count count_field(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string("Arena ") + key + " must be an integer");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<Count>::max()))
        throw std::invalid_argument(std::string("Arena ") + key + " is too large");
    auto count = value.get<Count>();
    if (count < 0) throw std::invalid_argument(std::string("Arena ") + key + " must be nonnegative");
    return count;
}

double seconds_field(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number())
        throw std::invalid_argument(std::string("Arena ") + key + " must be numeric");
    double seconds = value.get<double>();
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::invalid_argument(std::string("Arena ") + key + " must be finite and nonnegative");
    return seconds;
}

int identity(const Json& value) {
    if (value == "a") return 0;
    if (value == "b") return 1;
    throw std::invalid_argument("Arena identity must be a or b");
}

struct Outcomes {
    Count games = 0;
    Count completed = 0;
    Count truncated = 0;
    Count wins = 0;
    Count losses = 0;
    Count draws = 0;
    Count failed = 0;
    Count resigned = 0;

    void add(bool terminal, std::optional<int> winner, int color) {
        ++games;
        if (!terminal) { ++truncated; return; }
        ++completed;
        if (!winner) ++draws;
        else if (*winner == color) ++wins;
        else ++losses;
    }

    void add_attempt(const std::string& status, std::optional<int> winner, int color) {
        if (status == "failed" || status == "resigned") {
            ++games;
            if (status == "failed") ++failed;
            else ++resigned;
        } else add(status == "completed", winner, color);
    }

    Json to_json(bool external = false) const {
        Json result = {{"games", games}, {"completed_games", completed}, {"truncated_games", truncated},
                {"wins", wins}, {"losses", losses}, {"draws", draws},
                {"win_rate", ratio(static_cast<double>(wins), static_cast<double>(completed))},
                {"score_rate", ratio(static_cast<double>(wins) + 0.5 * static_cast<double>(draws),
                                     static_cast<double>(completed))}};
        if (external) {
            result["attempted_games"] = games;
            result["failed_games"] = failed;
            result["resigned_games"] = resigned;
        }
        return result;
    }
};

struct Performance {
    Count moves = 0;
    Count decision_attempts = 0;
    bool external_unknown = false;
    Count searches = 0;
    Count simulations = 0;
    Count completed_rollouts = 0;
    Count truncated_rollouts = 0;
    Count rollout_simulations = 0;
    Count neural_searches = 0;
    Count network_evaluations = 0;
    Count terminal_evaluations = 0;
    double decision_seconds = 0;
    double search_seconds = 0;

    void add(const Json& decision) {
        ++decision_attempts;
        if (decision.value("accepted", true)) ++moves;
        external_unknown = external_unknown || decision.value("external", false);
        decision_seconds += seconds_field(decision, "elapsed_seconds");
        if (decision.contains("network_evaluations"))
            network_evaluations += count_field(decision, "network_evaluations");
        if (!decision.contains("search")) return;
        const auto& search = decision.at("search");
        Count count = count_field(search, "simulations");
        Count completed = count_field(search, "completed_rollouts");
        Count truncated = count_field(search, "truncated_rollouts");
        const auto algorithm = search.value("algorithm", std::string("uct"));
        if (algorithm == "uct") {
            if (completed > count || truncated != count - completed)
                throw std::invalid_argument("Arena rollout counts must sum to simulation count");
            rollout_simulations += count;
        } else if (algorithm == "puct") {
            const auto network = count_field(search, "network_evaluations");
            const auto terminal = count_field(search, "terminal_evaluations");
            if (count == std::numeric_limits<Count>::max() || completed || truncated ||
                network > count + 1 || terminal != count + 1 - network)
                throw std::invalid_argument("Arena PUCT evaluations must sum to simulations plus root initialization");
            ++neural_searches;
            network_evaluations += network;
            terminal_evaluations += terminal;
        } else throw std::invalid_argument("Arena search algorithm must be uct or puct");
        ++searches;
        simulations += count;
        completed_rollouts += completed;
        truncated_rollouts += truncated;
        search_seconds += seconds_field(search, "elapsed_seconds");
    }

    void append_to(Json& object) const {
        object["moves"] = moves;
        object["decision_seconds"] = decision_seconds;
        object["mean_move_seconds"] = ratio(decision_seconds, static_cast<double>(decision_attempts));
        object["searches"] = searches;
        object["simulations"] = simulations;
        object["search_seconds"] = search_seconds;
        object["simulations_per_second"] = ratio(static_cast<double>(simulations), search_seconds);
        object["completed_rollouts"] = completed_rollouts;
        object["truncated_rollouts"] = truncated_rollouts;
        object["rollout_truncation_rate"] = ratio(static_cast<double>(truncated_rollouts),
                                                 static_cast<double>(rollout_simulations));
        if (network_evaluations || terminal_evaluations || neural_searches) {
            object["neural_searches"] = neural_searches;
            object["network_evaluations"] = network_evaluations;
            object["terminal_evaluations"] = terminal_evaluations;
            object["rollout_simulations"] = rollout_simulations;
        }
        if (external_unknown) {
            for (const auto* field : {"searches", "simulations", "search_seconds", "simulations_per_second",
                                     "completed_rollouts", "truncated_rollouts", "rollout_truncation_rate",
                                     "network_evaluations", "terminal_evaluations", "neural_searches", "rollout_simulations"})
                object[field] = nullptr;
            object["external_search_counters"] = "unknown";
        }
    }
};

struct AgentTotals {
    Outcomes outcomes;
    std::array<Outcomes, 2> color;
    Performance performance;
    double paired_wins = 0;
    double paired_scores = 0;
    Count failures_caused = 0, resignations_by_agent = 0;
};

struct Pair {
    std::array<const Json*, 2> games{nullptr, nullptr};
};

bool completed_game(const Json& game) {
    if (game.contains("status")) {
        const auto& status = game.at("status");
        if (status == "completed" && game.at("termination_reason") == "two_passes") return true;
        if ((status == "truncated" && game.at("termination_reason") == "move_limit") ||
            (status == "failed" && game.at("termination_reason") == "failure") ||
            (status == "resigned" && game.at("termination_reason") == "resignation")) return false;
        throw std::invalid_argument("External arena status disagrees with termination reason");
    }
    const auto& reason = game.at("termination_reason");
    if (reason == "two_passes") return true;
    if (reason == "move_limit") return false;
    throw std::invalid_argument("Unsupported arena termination reason");
}

std::optional<int> recorded_winner(const Json& game, bool completed) {
    const auto& winner = game.at("winner");
    if (winner.is_null()) return std::nullopt;
    if (!completed || (!winner.is_number_integer()) || (winner != BLACK && winner != WHITE))
        throw std::invalid_argument("Invalid arena winner");
    return winner.get<int>();
}

Json paired_interval(double value_sum, Count sample_size) {
    if (sample_size == 0) return nullptr;
    double estimate = value_sum / static_cast<double>(sample_size);
    // Hoeffding's two-sided bound: 2 exp(-2 n epsilon^2) = 0.05.
    // Each bounded observation is the mean of a COMPLETE color pair; the two
    // correlated games never count as two independent observations.
    double half_width = std::sqrt(std::log(40.0) / (2.0 * static_cast<double>(sample_size)));
    return {{"method", "Hoeffding over complete color pairs"}, {"sample_size", sample_size},
            {"estimate", estimate}, {"lower", std::max(0.0, estimate - half_width)},
            {"upper", std::min(1.0, estimate + half_width)}, {"confidence", 0.95},
            {"population", "pairs where both color games completed"},
            {"assumption", "independent seed pairs"}};
}

std::string checkpoint_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot open checkpoint " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::invalid_argument("Cannot read checkpoint " + path.string());
    return bytes;
}

std::string fnv1a64(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}
} // namespace

void AgentConfiguration::validate() const {
    if (kind != "random" && kind != "mcts" && kind != "policy" && kind != "neural-mcts" && kind != "external-gtp")
        throw std::invalid_argument("Agent must be random, mcts, policy, neural-mcts, or external-gtp");
    search.validate();
    if (kind == "neural-mcts") neural.validate();
    const bool uses_network = kind == "policy" || kind == "neural-mcts";
    if (uses_network && checkpoint.empty()) throw std::invalid_argument(kind + " requires a checkpoint");
    if (!uses_network && !checkpoint.empty()) throw std::invalid_argument(kind + " does not use a checkpoint");
    if ((kind == "external-gtp") != external_gtp.has_value())
        throw std::invalid_argument("An external GTP profile belongs only to an external-gtp agent");
    if (external_gtp) external_gtp->validate();
}

Json AgentConfiguration::to_json() const {
    Json result = {{"kind", kind}};
    if (kind == "mcts") result["search"] = mcts_settings_json(search);
    if (kind == "neural-mcts") result["search"] = neural_mcts_settings_json(neural);
    if (!checkpoint.empty()) result["checkpoint"] = checkpoint;
    if (external_gtp) result["external_gtp"] = external_gtp->to_json();
    return result;
}

PreparedAgent prepare_agent(const AgentConfiguration& configuration, int board_size) {
    configuration.validate();
    if (board_size < 1) throw std::invalid_argument("Board size must be positive");
    PreparedAgent prepared{configuration, {}, nullptr};
    if (configuration.external_gtp) {
        auto& external = *prepared.configuration.external_gtp;
        prepared.external_identity = external_gtp_identity(external);
        if (!external.declared_file_fingerprints.is_null() &&
            external.declared_file_fingerprints != prepared.external_identity)
            throw std::invalid_argument("External profile files changed after loading");
        external.declared_file_fingerprints = prepared.external_identity;
        return prepared;
    }
    if (configuration.checkpoint.empty()) return prepared;
    const auto before = checkpoint_bytes(configuration.checkpoint);
    auto network = PolicyValueNetwork::load(configuration.checkpoint);
    if (before != checkpoint_bytes(configuration.checkpoint))
        throw std::invalid_argument("Checkpoint changed while loading; retry with a stable file");
    if (network.settings().board_size != board_size)
        throw std::invalid_argument("Checkpoint board size does not match requested board size");
    network.train(false);
    const auto& settings = network.settings();
    prepared.checkpoint_metadata = {
        {"path", std::filesystem::absolute(configuration.checkpoint).lexically_normal().string()},
        {"fingerprint_algorithm", "fnv1a64"}, {"content_fingerprint", fnv1a64(before)},
        {"feature_schema", FEATURE_SCHEMA}, {"board_size", settings.board_size},
        {"channels", settings.channels}, {"value_hidden", settings.value_hidden},
        {"initialization_seed", network.initialization_seed()},
        {"training_steps", network.training_steps()}, {"parameter_count", network.parameter_count()}};
    prepared.network = std::make_shared<const PolicyValueNetwork>(std::move(network));
    return prepared;
}

ArenaAgent PreparedAgent::create(std::int64_t seed) const {
    if (configuration.kind == "external-gtp")
        throw std::invalid_argument("External GTP agents require a per-game session");
    if (configuration.kind == "random") {
        auto agent = std::make_shared<RandomAgent>(seed);
        return [agent](const GameState& state) { return ArenaDecision{agent->choose_move(state), std::nullopt, std::nullopt}; };
    }
    if (configuration.kind == "mcts") {
        auto agent = std::make_shared<MctsAgent>(configuration.search, seed);
        return [agent](const GameState& state) {
            Move move = agent->choose_move(state);
            return ArenaDecision{move, agent->last_search(), std::nullopt};
        };
    }
    if (!network) throw std::invalid_argument("Neural agent has no prepared checkpoint");
    if (configuration.kind == "policy") {
        auto agent = std::make_shared<PolicyAgent>(network);
        return [agent](const GameState& state) {
            Move move = agent->choose_move(state);
            return ArenaDecision{move, std::nullopt, agent->last_prediction()};
        };
    }
    auto agent = std::make_shared<NeuralMctsAgent>(network, configuration.neural);
    return [agent](const GameState& state) {
        Move move = agent->choose_move(state);
        return ArenaDecision{move, agent->last_search(), std::nullopt};
    };
}

ArenaAgentSession PreparedAgent::create_session(std::int64_t seed, int size, double komi) const {
    if (!configuration.external_gtp) return ArenaAgentSession{create(seed)};
    auto engine = std::make_shared<ExternalGtpSession>(*configuration.external_gtp, seed, size, komi);
    return ArenaAgentSession{
        [engine](const GameState& state) { return ArenaDecision{engine->genmove(state), std::nullopt, std::nullopt}; },
        [engine] { engine->start(); },
        [engine](int color, Move move, bool own) { engine->accepted_move(color, move, own); },
        [engine] { return engine->shutdown(); },
        [engine] { return engine->metadata(); }};
}

Json PreparedAgent::to_json() const {
    auto result = configuration.to_json();
    if (!checkpoint_metadata.is_null()) result["checkpoint_identity"] = checkpoint_metadata;
    if (!external_identity.is_null()) result["external_identity"] = external_identity;
    return result;
}

void ArenaSettings::validate() const {
    a.validate(); b.validate();
    if (pairs < 1) throw std::invalid_argument("Arena pairs must be positive");
    if (pairs > std::numeric_limits<int>::max() / 2)
        throw std::invalid_argument("Arena pair count exceeds the supported game index range");
    if (size < 1) throw std::invalid_argument("Arena board size must be positive");
    if (max_moves < 1) throw std::invalid_argument("Arena move limit must be positive");
    if (!std::isfinite(komi)) throw std::invalid_argument("Arena komi must be finite");
    if (a.external_gtp && b.external_gtp)
        throw std::invalid_argument("Use one external opponent profile per arena experiment");
    if (seed > std::numeric_limits<std::int64_t>::max() - (2LL * pairs - 1))
        throw std::invalid_argument("Arena seeds exceed signed 64-bit range");
}

Json search_statistics_json(const SearchStatistics& statistics) {
    Json children = Json::array();
    for (const auto& child : statistics.children) {
        Json move = child.move ? Json::array({child.move->row, child.move->column}) : Json(nullptr);
        children.push_back({{"move", move}, {"visits", child.visits},
                            {"value_sum_for_child_player", child.value_sum}});
        if (statistics.algorithm == "puct") children.back()["prior"] = child.prior;
    }
    Json result = {{"simulations", statistics.simulations}, {"root_visits", statistics.root_visits},
            {"root_value_sum", statistics.root_value_sum}, {"completed_rollouts", statistics.completed_rollouts},
            {"truncated_rollouts", statistics.truncated_rollouts}, {"elapsed_seconds", statistics.elapsed_seconds},
            {"simulations_per_second", statistics.simulations_per_second()}, {"children", children}};
    if (statistics.algorithm == "puct") {
        result["algorithm"] = statistics.algorithm;
        result["network_evaluations"] = statistics.network_evaluations;
        result["terminal_evaluations"] = statistics.terminal_evaluations;
    }
    return result;
}

Json mcts_settings_json(const MctsSettings& settings) {
    return {{"simulations", settings.simulations}, {"exploration", settings.exploration},
            {"rollout_limit", settings.rollout_limit}, {"truncated_rollout_value", 0}};
}

Json neural_mcts_settings_json(const NeuralMctsSettings& settings) {
    return {{"simulations", settings.simulations}, {"c_puct", settings.c_puct},
            {"algorithm", "puct"}, {"leaf_value_perspective", "player_to_move"}};
}

Json prediction_json(const Prediction& prediction) {
    return {{"probabilities", prediction.policy}, {"value_for_player_to_move", prediction.value}};
}

Json summarize_arena(const Json& games, std::optional<bool> eligibility) {
    if (!games.is_array()) throw std::invalid_argument("Arena games must be an array");
    std::array<AgentTotals, 2> agents;
    std::map<Count, Pair> pairs;
    Count completed = 0, truncated = 0;
    Count failed = 0, resigned = 0, cleanup_warning_count = 0;
    const bool external = !games.empty() && games.front().contains("status");
    bool uncertainty_eligible = eligibility.value_or(!external);
    Json failures_by_actor = {{"a", 0}, {"b", 0}}, failures_by_code = Json::object();
    double length_sum = 0, completed_length_sum = 0;
    for (const auto& game : games) {
        if (game.contains("status") != external) throw std::invalid_argument("Cannot mix arena record schemas");
        Count pair_index = count_field(game, "pair_index");
        Count in_pair = count_field(game, "game_in_pair");
        if (in_pair != 1 && in_pair != 2)
            throw std::invalid_argument("Arena game_in_pair must be 1 or 2");
        auto& slot = pairs[pair_index].games[static_cast<std::size_t>(in_pair - 1)];
        if (slot) throw std::invalid_argument("Duplicate arena game in a color pair");
        slot = &game;
        int black = identity(game.at("black_agent")), white = identity(game.at("white_agent"));
        if (black == white) throw std::invalid_argument("Arena game needs one agent of each identity");
        bool terminal = completed_game(game);
        auto winner = recorded_winner(game, terminal);
        double length = static_cast<double>(count_field(game, "game_length"));
        length_sum += length;
        const auto status = external ? game.at("status").get<std::string>() : terminal ? "completed" : "truncated";
        if (terminal) { ++completed; completed_length_sum += length; }
        else if (status == "failed") {
            ++failed;
            const auto& failure = game.at("failure");
            const int cause = identity(failure.at("actor"));
            ++agents[cause].failures_caused;
            const auto cause_name = cause == 0 ? "a" : "b";
            failures_by_actor[cause_name] = failures_by_actor[cause_name].get<Count>() + 1;
            const auto code = failure.at("code").get<std::string>();
            failures_by_code[code] = failures_by_code.value(code, Count{0}) + 1;
        } else if (status == "resigned") {
            ++resigned;
            ++agents[identity(game.at("resigned_by"))].resignations_by_agent;
        } else ++truncated;
        if (external) {
            uncertainty_eligible = uncertainty_eligible && game.value("uncertainty_eligible", false);
            if (!game.at("cleanup_warnings").is_array()) throw std::invalid_argument("Cleanup warnings must be an array");
            cleanup_warning_count += static_cast<Count>(game.at("cleanup_warnings").size());
        }
        for (auto [agent, color] : {std::pair{black, BLACK}, std::pair{white, WHITE}}) {
            agents[agent].outcomes.add_attempt(status, winner, color);
            agents[agent].color[color == BLACK ? 0 : 1].add_attempt(status, winner, color);
            if (external && game.at("agent_kinds").at(agent == 0 ? "a" : "b") == "external-gtp")
                agents[agent].performance.external_unknown = true;
        }
        const auto& decisions = game.at("decisions");
        if (!decisions.is_array()) throw std::invalid_argument("Arena decisions must be an array");
        for (const auto& decision : decisions) {
            int agent = identity(decision.at("agent"));
            const auto& player = decision.at("player");
            if (!player.is_number_integer() || (player != BLACK && player != WHITE) ||
                agent != (player == BLACK ? black : white))
                throw std::invalid_argument("Arena decision identity disagrees with its player color");
            agents[agent].performance.add(decision);
        }
    }

    Count complete_pairs = 0;
    for (const auto& [index, pair] : pairs) {
        (void)index;
        if (!pair.games[0] || !pair.games[1]) continue;
        if (pair.games[0]->at("black_agent") != pair.games[1]->at("white_agent") ||
            pair.games[0]->at("white_agent") != pair.games[1]->at("black_agent"))
            throw std::invalid_argument("Arena pair must exchange the agents' colors");
        if (!completed_game(*pair.games[0]) || !completed_game(*pair.games[1])) continue;
        ++complete_pairs;
        for (const auto* game : pair.games) {
            auto winner = recorded_winner(*game, true);
            int winning_agent = !winner ? -1 : identity(game->at(*winner == BLACK ? "black_agent" : "white_agent"));
            for (int agent = 0; agent < 2; ++agent) {
                agents[agent].paired_wins += winning_agent == agent ? 0.5 : 0.0;
                agents[agent].paired_scores += !winner ? 0.25 : winning_agent == agent ? 0.5 : 0.0;
            }
        }
    }

    Json totals = Json::object();
    for (int index = 0; index < 2; ++index) {
        const auto& agent = agents[index];
        Json record = agent.outcomes.to_json(external);
        record["by_color"] = {{"black", agent.color[0].to_json(external)}, {"white", agent.color[1].to_json(external)}};
        agent.performance.append_to(record);
        record["paired_win_rate_95"] = uncertainty_eligible ? paired_interval(agent.paired_wins, complete_pairs) : Json(nullptr);
        record["paired_score_rate_95"] = uncertainty_eligible ? paired_interval(agent.paired_scores, complete_pairs) : Json(nullptr);
        if (external) {
            record["decision_attempts"] = agent.performance.decision_attempts;
            record["failures_caused"] = agent.failures_caused;
            record["resignations_by_agent"] = agent.resignations_by_agent;
        }
        totals[index == 0 ? "a" : "b"] = record;
    }
    Json summary = {{"games", games.size()}, {"completed_games", completed}, {"truncated_games", truncated},
            {"complete_pairs", complete_pairs},
            {"incomplete_pairs", static_cast<Count>(pairs.size()) - complete_pairs},
            {"mean_game_length", ratio(length_sum, static_cast<double>(games.size()))},
            {"mean_completed_game_length", ratio(completed_length_sum, static_cast<double>(completed))},
            {"agents", totals}};
    if (external) {
        summary["attempted_games"] = games.size();
        summary["failed_games"] = failed;
        summary["resigned_games"] = resigned;
        summary["failed_by_actor"] = std::move(failures_by_actor);
        summary["failed_by_code"] = std::move(failures_by_code);
        summary["uncertainty_eligible"] = uncertainty_eligible;
        summary["uncertainty_exclusion_reason"] = !uncertainty_eligible
            ? "documented independent stochastic seed integration is unavailable"
            : complete_pairs == 0 ? "no complete scored color pairs" : "";
        summary["cleanup_warning_count"] = cleanup_warning_count;
    }
    return summary;
}

namespace {
Json arena_report(const ArenaSettings& settings, const Json& metadata,
                  const PreparedAgent& a, const PreparedAgent& b, const Json& games,
                  bool external, bool uncertainty_eligible, bool finished) {
    Json report = {{"schema_version", external ? 2 : 1}, {"mode", "arena"}, {"seed", settings.seed},
        {"agents", {{"a", a.to_json()}, {"b", b.to_json()}}},
        {"settings", {{"pairs", settings.pairs}, {"size", settings.size},
                      {"komi", settings.komi}, {"max_moves", settings.max_moves}}},
        {"rules", "simple ko, no suicide, area scoring, no dead-group adjudication"},
        {"metadata", metadata}, {"games", games}, {"summary", summarize_arena(games, uncertainty_eligible)}};
    if (external) {
        report["run_status"] = finished ? "finished" : "running";
        report["comparison_context"] = {{"board_size", settings.size},
            {"description", "Configuration-specific " + std::to_string(settings.size) + "x" + std::to_string(settings.size) + " external-engine comparison"},
            {"reference_labels", "Unvalidated metadata; HumanSL imitation labels do not establish a calibrated 9x9 rank ladder"},
            {"human_rank_estimate", nullptr}, {"score_authority", "BetaGo raw final-board area score after two passes"}};
        report["diagnostics_encoding"] = "UTF-8 JSON; invalid diagnostic bytes are replaced with U+FFFD when saved";
        report["uncertainty_eligible"] = uncertainty_eligible;
        report["uncertainty_assumption"] = uncertainty_eligible
            ? "external profile declares documented stochastic seed integration; independent seed pairs assumed"
            : "external independence is undocumented; uncertainty intervals suppressed";
    }
    return report;
}

Json run_external_arena(const ArenaSettings& settings, const Json& metadata,
                        const ArenaProgress& progress, const ArenaAgentFactory& factory,
                        const ArenaSnapshot& snapshot, const ArenaMoveProgress& move_progress, const PreparedAgent& prepared_a,
                        const PreparedAgent& prepared_b) {
    Json games = Json::array();
    const bool eligible = (!settings.a.external_gtp || settings.a.external_gtp->seeded_stochastic()) &&
                          (!settings.b.external_gtp || settings.b.external_gtp->seeded_stochastic());
    for (int pair = 0; pair < settings.pairs; ++pair) {
        const std::int64_t a_seed = settings.seed + 2LL * pair, b_seed = a_seed + 1;
        for (int in_pair = 1; in_pair <= 2; ++in_pair) {
            const bool a_black = in_pair == 1;
            const auto started = std::chrono::steady_clock::now();
            auto prefix_state = GameState::new_game(settings.size, settings.komi);
            const int game_index = static_cast<int>(games.size());
            if (move_progress) move_progress(game_index, prefix_state, PASS, 0);
            std::vector<Move> prefix;
            Json decisions = Json::array(), failure = nullptr, resigned_by = nullptr;
            Json cleanup_warnings = Json::array(), external_sessions = Json::object();
            ArenaAgentSession a, b;
            std::exception_ptr deferred_error;
            std::string active_actor = "a", phase = "session_start", status;
            try {
                a = factory && !settings.a.external_gtp ? ArenaAgentSession{factory(settings.a, a_seed)}
                            : prepared_a.create_session(a_seed, settings.size, settings.komi);
                active_actor = "b";
                b = factory && !settings.b.external_gtp ? ArenaAgentSession{factory(settings.b, b_seed)}
                            : prepared_b.create_session(b_seed, settings.size, settings.komi);
                if (!a.choose) { active_actor = "a"; throw std::invalid_argument("Arena factory returned an empty agent"); }
                if (!b.choose) { active_actor = "b"; throw std::invalid_argument("Arena factory returned an empty agent"); }
                active_actor = "a"; a.start();
                active_actor = "b"; b.start();
                auto choose = [&](const GameState& state) {
                    const bool use_a = (state.to_play() == BLACK) == a_black;
                    active_actor = use_a ? "a" : "b";
                    phase = "genmove";
                    const auto decision_started = std::chrono::steady_clock::now();
                    Json record = {{"move_number", prefix.size() + 1}, {"player", state.to_play()},
                                   {"agent", active_actor}, {"accepted", false},
                                   {"external", (use_a ? settings.a : settings.b).kind == "external-gtp"}};
                    auto finish = [&] {
                        record["elapsed_seconds"] = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - decision_started).count();
                        decisions.push_back(std::move(record));
                    };
                    try {
                        const auto decision = (use_a ? a : b)(state);
                        record["event"] = "move";
                        record["move"] = decision.move ? Json::array({decision.move->row, decision.move->column}) : Json(nullptr);
                        if (decision.search) record["search"] = search_statistics_json(*decision.search);
                        if (decision.prediction) {
                            record["policy"] = prediction_json(*decision.prediction);
                            record["network_evaluations"] = 1;
                        }
                        finish();
                        phase = "local_play";
                        return decision.move;
                    } catch (const GtpResignation&) {
                        record["event"] = "resignation"; finish(); throw;
                    } catch (...) {
                        record["event"] = "failure"; finish(); throw;
                    }
                };
                auto accepted = [&](const GameState& before, Move move, const GameState& after) {
                    // Retain the local acceptance before any protocol notification.
                    // Even a failed final-pass sync must remain a legal prefix.
                    prefix.push_back(move);
                    prefix_state = after;
                    decisions.back()["accepted"] = true;
                    const bool own_a = (before.to_play() == BLACK) == a_black;
                    phase = "accepted_move";
                    active_actor = "a"; a.accepted(before.to_play(), move, own_a);
                    active_actor = "b"; b.accepted(before.to_play(), move, !own_a);
                    if (move_progress) move_progress(game_index, after, move, static_cast<int>(prefix.size()));
                };
                auto result = run_game(choose, choose, settings.size, settings.komi, settings.max_moves, accepted);
                prefix_state = std::move(result.final_state);
                status = prefix_state.is_terminal() ? "completed" : "truncated";
            } catch (const GtpResignation&) {
                if ((active_actor == "a" ? settings.a : settings.b).kind != "external-gtp" || phase != "genmove")
                    deferred_error = std::current_exception();
                else { status = "resigned"; resigned_by = active_actor; }
            } catch (const GtpFailure& error) {
                if ((active_actor == "a" ? settings.a : settings.b).kind != "external-gtp")
                    deferred_error = std::current_exception();
                else {
                    status = "failed"; failure = error.to_json();
                    failure["actor"] = active_actor; failure["message"] = error.what();
                }
            } catch (const std::exception& error) {
                if ((active_actor == "a" ? settings.a : settings.b).kind != "external-gtp")
                    deferred_error = std::current_exception();
                else {
                    status = "failed";
                    failure = {{"actor", active_actor}, {"code", dynamic_cast<const IllegalMove*>(&error) ? "illegal_move" :
                                             phase == "session_start" ? "launch_failure" : "unexpected_exit"},
                           {"command", phase}, {"diagnostics", error.what()}, {"message", error.what()}};
                }
            }
            auto close = [&](const char* actor, ArenaAgentSession& session, bool external) {
                try {
                    const auto warnings = session.shutdown();
                    if (!warnings.is_array()) throw std::runtime_error("Session shutdown warnings must be an array");
                    for (const auto& warning : warnings) {
                        Json entry = warning.is_object() ? warning : Json{{"diagnostics", warning.dump()}};
                        entry["actor"] = actor;
                        cleanup_warnings.push_back(std::move(entry));
                    }
                } catch (const std::exception& error) {
                    cleanup_warnings.push_back({{"actor", actor}, {"code", "shutdown_failure"}, {"diagnostics", error.what()}});
                }
                if (external) {
                    try { external_sessions[actor] = session.metadata(); }
                    catch (const std::exception& error) { external_sessions[actor] = {{"metadata_error", error.what()}}; }
                }
            };
            close("a", a, settings.a.kind == "external-gtp");
            close("b", b, settings.b.kind == "external-gtp");
            if (deferred_error) std::rethrow_exception(deferred_error);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            Json game = GameResult{prefix_state, prefix, settings.max_moves, elapsed}.to_json();
            game["status"] = status;
            if (status == "failed" || status == "resigned") {
                game["score"] = game["winner"] = nullptr;
                game["termination_reason"] = status == "failed" ? "failure" : "resignation";
            }
            game["pair_index"] = pair; game["game_in_pair"] = in_pair;
            game["black_agent"] = a_black ? "a" : "b";
            game["white_agent"] = a_black ? "b" : "a";
            game["a_seed"] = a_seed; game["b_seed"] = b_seed;
            game["black_seed"] = a_black ? a_seed : b_seed;
            game["white_seed"] = a_black ? b_seed : a_seed;
            game["agent_kinds"] = {{"a", settings.a.kind}, {"b", settings.b.kind}};
            game["uncertainty_eligible"] = eligible;
            game["decisions"] = std::move(decisions);
            game["failure"] = std::move(failure); game["resigned_by"] = std::move(resigned_by);
            game["cleanup_warnings"] = std::move(cleanup_warnings);
            game["external_sessions"] = std::move(external_sessions);
            games.push_back(std::move(game));
            const bool finished = pair + 1 == settings.pairs && in_pair == 2;
            if (snapshot) snapshot(arena_report(settings, metadata, prepared_a, prepared_b, games, true, eligible, finished));
            if (progress) progress(static_cast<int>(games.size() - 1), games.back());
        }
    }
    return arena_report(settings, metadata, prepared_a, prepared_b, games, true, eligible, true);
}
} // namespace

Json run_arena(const ArenaSettings& settings, const Json& metadata,
               const ArenaProgress& progress, const ArenaAgentFactory& factory,
               const ArenaSnapshot& snapshot, const ArenaMoveProgress& move_progress) {
    settings.validate();
    if (!metadata.is_object()) throw std::invalid_argument("Arena metadata must be an object");
    Json games = Json::array();
    const auto prepared_a = prepare_agent(settings.a, settings.size);
    const auto prepared_b = prepare_agent(settings.b, settings.size);
    if (settings.a.external_gtp || settings.b.external_gtp)
        return run_external_arena(settings, metadata, progress, factory, snapshot, move_progress, prepared_a, prepared_b);
    for (int pair = 0; pair < settings.pairs; ++pair) {
        std::int64_t a_seed = settings.seed + 2LL * pair, b_seed = a_seed + 1;
        for (int game_in_pair = 1; game_in_pair <= 2; ++game_in_pair) {
            // A new closure owns a new RNG for each game, preserving identical
            // identity seeds after the colors change without sharing RNG state.
            ArenaAgent a = factory ? factory(settings.a, a_seed) : prepared_a.create(a_seed);
            ArenaAgent b = factory ? factory(settings.b, b_seed) : prepared_b.create(b_seed);
            if (!a || !b) throw std::invalid_argument("Arena factory returned an empty agent");
            bool a_black = game_in_pair == 1;
            Json decisions = Json::array();
            Count move_number = 0;
            const int game_index = static_cast<int>(games.size());
            if (move_progress) move_progress(game_index, GameState::new_game(settings.size, settings.komi), PASS, 0);
            auto choose = [&](const GameState& state) {
                bool use_a = (state.to_play() == BLACK) == a_black;
                auto started = std::chrono::steady_clock::now();
                ArenaDecision decision = (use_a ? a : b)(state);
                double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                Json record = {{"move_number", ++move_number}, {"player", state.to_play()},
                               {"agent", use_a ? "a" : "b"}, {"elapsed_seconds", elapsed}};
                if (decision.search) record["search"] = search_statistics_json(*decision.search);
                if (decision.prediction) {
                    record["policy"] = prediction_json(*decision.prediction);
                    record["network_evaluations"] = 1;
                }
                decisions.push_back(std::move(record));
                return decision.move;
            };
            AcceptedMoveObserver accepted;
            if (move_progress) accepted = [&](const GameState&, Move move, const GameState& after) {
                move_progress(game_index, after, move, static_cast<int>(move_number));
            };
            auto result = run_game(choose, choose, settings.size, settings.komi, settings.max_moves, accepted);
            Json game = result.to_json();
            game["pair_index"] = pair; game["game_in_pair"] = game_in_pair;
            game["black_agent"] = a_black ? "a" : "b";
            game["white_agent"] = a_black ? "b" : "a";
            game["a_seed"] = a_seed; game["b_seed"] = b_seed;
            game["black_seed"] = a_black ? a_seed : b_seed;
            game["white_seed"] = a_black ? b_seed : a_seed;
            game["decisions"] = std::move(decisions);
            games.push_back(std::move(game));
            if (snapshot) snapshot(arena_report(settings, metadata, prepared_a, prepared_b, games, false, true,
                                                pair + 1 == settings.pairs && game_in_pair == 2));
            if (progress) progress(static_cast<int>(games.size() - 1), games.back());
        }
    }
    return arena_report(settings, metadata, prepared_a, prepared_b, games, false, true, true);
}
} // namespace betago
