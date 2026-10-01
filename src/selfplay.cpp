#include "betago/selfplay.hpp"
#include "betago/profile.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace betago {
namespace {
const Json& field(const Json& object, const char* name) {
    if (!object.is_object() || !object.contains(name))
        throw std::invalid_argument(std::string("Self-play record is missing field: ") + name);
    return object.at(name);
}

std::int64_t integer64(const Json& value, std::int64_t low, std::int64_t high, const char* name) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(high) ||
            (low > 0 && number < static_cast<std::uint64_t>(low)))
            throw std::invalid_argument(std::string(name) + " is out of range");
        return static_cast<std::int64_t>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < low || number > high)
        throw std::invalid_argument(std::string(name) + " is out of range");
    return number;
}

int integer(const Json& value, int low, int high, const char* name) {
    return static_cast<int>(integer64(value, low, high, name));
}

double number(const Json& value, const char* name) {
    if (!value.is_number())
        throw std::invalid_argument(std::string(name) + " must be numeric");
    const double result = value.get<double>();
    if (!std::isfinite(result))
        throw std::invalid_argument(std::string(name) + " must be finite");
    return result;
}

void text_equals(const Json& value, const char* expected, const char* name) {
    if (!value.is_string() || value.get<std::string>() != expected)
        throw std::invalid_argument(std::string("Unsupported self-play ") + name);
}

SelfPlaySettings settings_from_json(const Json& data) {
    SelfPlaySettings settings;
    settings.board_size = integer(field(data, "board_size"), 1, 19, "Board size");
    settings.komi = number(field(data, "komi"), "Komi");
    settings.max_moves = integer(field(data, "max_moves"), 1, std::numeric_limits<int>::max(), "Move limit");
    const auto& search = field(data, "search");
    settings.search.simulations = integer(field(search, "simulations"), 1,
        std::numeric_limits<int>::max() - 1, "Simulation budget");
    settings.search.c_puct = number(field(search, "c_puct"), "PUCT exploration");
    settings.temperature = number(field(data, "temperature"), "Temperature");
    settings.temperature_moves = integer(field(data, "temperature_moves"), 0,
        std::numeric_limits<int>::max(), "Temperature move count");
    settings.root_uniform_mix = number(field(data, "root_uniform_mix"), "Root uniform mixture");
    settings.validate();
    return settings;
}

Json state_json(const GameState& state) {
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi()},
            {"consecutive_passes", state.consecutive_passes()},
            {"previous_board", state.previous_board() ? Json(*state.previous_board()) : Json(nullptr)}};
}

void validate_state_json(const Json& data, const GameState& state) {
    // Comparing the complete raw state also preserves the one-ply board history
    // needed for the exact simple-ko legality mask on replay.
    if (data != state_json(state))
        throw std::invalid_argument("Self-play target state disagrees with replay");
    integer(field(data, "to_play"), BLACK, WHITE, "Player to move");
    integer(field(data, "consecutive_passes"), 0, 1, "Consecutive passes");
    number(field(data, "komi"), "Komi");
    for (const auto* name : {"board", "previous_board"}) {
        const auto& board = field(data, name);
        if (board.is_null()) continue;
        for (const auto& row : board)
            for (const auto& cell : row) integer(cell, EMPTY, WHITE, "Stone color");
    }
}

std::vector<int> action_visits(const GameState& state, const SearchStatistics& search) {
    if (state.is_terminal()) throw std::invalid_argument("Cannot form targets for a terminal state");
    if (search.root_visits <= 0 || search.simulations != search.root_visits)
        throw std::invalid_argument("Self-play target requires a positive consistent search budget");
    const auto legal = state.legal_moves();
    if (search.children.size() != legal.size())
        throw std::invalid_argument("Self-play search must report every legal root action");
    std::vector<int> counts(static_cast<std::size_t>(state.size() * state.size() + 1), 0);
    std::vector<bool> seen(counts.size(), false), allowed(counts.size(), false);
    for (Move move : legal) allowed[action_index(move, state.size())] = true;
    std::int64_t total = 0;
    for (const auto& child : search.children) {
        const auto action = action_index(child.move, state.size());
        if (action < 0 || static_cast<std::size_t>(action) >= counts.size() ||
            !allowed[action] || seen[action] || child.visits < 0)
            throw std::invalid_argument("Self-play root actions or visit counts are invalid");
        seen[action] = true;
        counts[action] = child.visits;
        total += child.visits;
    }
    if (total != search.root_visits)
        throw std::invalid_argument("Root child visits must sum to the completed simulation budget");
    return counts;
}

Json search_json(const GameState& state, const SearchStatistics& search) {
    return {{"simulations", search.simulations}, {"root_visits", search.root_visits},
            {"action_visits", action_visits(state, search)},
            {"network_evaluations", search.network_evaluations},
            {"terminal_evaluations", search.terminal_evaluations}};
}

std::vector<double> sampling_policy(const std::vector<double>& policy, double temperature) {
    // Dividing relative log weights keeps the largest exponent exactly zero.
    // Even a tiny positive temperature cannot overflow a visit count power.
    double largest = 0;
    for (double probability : policy) largest = std::max(largest, probability);
    const double log_largest = std::log(largest);
    std::vector<double> result(policy.size(), 0);
    double sum = 0;
    for (std::size_t action = 0; action < policy.size(); ++action) {
        if (policy[action] == 0) continue;
        result[action] = std::exp((std::log(policy[action]) - log_largest) / temperature);
        sum += result[action];
    }
    for (double& probability : result) probability /= sum;
    return result;
}

std::vector<Move> checked_moves(const Json& data, int size) {
    if (!data.is_array()) throw std::invalid_argument("Self-play moves must be an array");
    std::vector<Move> moves;
    moves.reserve(data.size());
    for (const auto& action : data) {
        if (action.is_null()) moves.push_back(PASS);
        else {
            if (!action.is_array() || action.size() != 2)
                throw std::invalid_argument("Placement must contain two integer coordinates");
            moves.push_back(Point{integer(action[0], 0, size - 1, "Row"),
                                  integer(action[1], 0, size - 1, "Column")});
        }
    }
    return moves;
}
} // namespace

void SelfPlaySettings::validate() const {
    if (board_size < 1 || board_size > 19)
        throw std::invalid_argument("Self-play board size must be between 1 and 19");
    if (!std::isfinite(komi)) throw std::invalid_argument("Self-play komi must be finite");
    if (max_moves <= 0) throw std::invalid_argument("Self-play move limit must be positive");
    search.validate();
    if (!std::isfinite(temperature) || temperature < 0)
        throw std::invalid_argument("Self-play temperature must be finite and nonnegative");
    if (temperature_moves < 0)
        throw std::invalid_argument("Self-play temperature move count must be nonnegative");
    if (!std::isfinite(root_uniform_mix) || root_uniform_mix < 0 || root_uniform_mix > 1)
        throw std::invalid_argument("Self-play root uniform mixture must be between 0 and 1");
}

Json SelfPlaySettings::to_json() const {
    validate();
    return {{"board_size", board_size}, {"komi", komi}, {"max_moves", max_moves},
            {"search", {{"simulations", search.simulations}, {"c_puct", search.c_puct}}},
            {"temperature", temperature}, {"temperature_moves", temperature_moves},
            {"root_uniform_mix", root_uniform_mix}};
}

std::vector<double> visit_policy(const GameState& state, const SearchStatistics& statistics) {
    const auto visits = action_visits(state, statistics);
    std::vector<double> policy;
    policy.reserve(visits.size());
    for (int count : visits) policy.push_back(static_cast<double>(count) / statistics.root_visits);
    return policy;
}

std::size_t sample_policy(const std::vector<double>& policy, Random& random) {
    if (policy.empty()) throw std::invalid_argument("Sampling policy must not be empty");
    long double total = 0;
    std::size_t last_positive = policy.size();
    for (std::size_t action = 0; action < policy.size(); ++action) {
        if (!std::isfinite(policy[action]) || policy[action] < 0 || policy[action] > 1)
            throw std::invalid_argument("Sampling probabilities must be finite and between 0 and 1");
        total += policy[action];
        if (policy[action] > 0) last_positive = action;
    }
    if (last_positive == policy.size() || std::abs(total - 1) > 1e-8L)
        throw std::invalid_argument("Sampling probabilities must sum to one");
    const long double threshold = random.unit() * total;
    long double cumulative = 0;
    for (std::size_t action = 0; action < policy.size(); ++action) {
        cumulative += policy[action];
        if (threshold < cumulative) return action;
    }
    return last_positive;
}

SelfPlayGame run_self_play(std::shared_ptr<const PolicyValueNetwork> network,
                          const SelfPlaySettings& settings, std::int64_t seed,
                          SelfPlayProgress progress, SelfPlayMoveProgress accepted_move) {
    ProfileScope scope(ProfileWork::SelfPlay);
    settings.validate();
    if (!network) throw std::invalid_argument("Self-play requires a network");
    if (network->settings().board_size != settings.board_size)
        throw std::invalid_argument("Self-play network and board sizes must match");
    GameState state = GameState::new_game(settings.board_size, settings.komi);
    Random random(seed);
    std::vector<Move> moves;
    Json targets = Json::array();
    std::vector<TrainingExample> examples;
    const auto started = std::chrono::steady_clock::now();
    for (int ply = 0; ply < settings.max_moves && !state.is_terminal(); ++ply) {
        // The first evaluator call always belongs to the root. Mixing only
        // there adds exploration without changing the leaf value convention
        // or the deterministic evaluation agent used in the arena.
        auto evaluator = [network, mixture = settings.root_uniform_mix, first = true]
                         (const GameState& position) mutable {
            auto prediction = network->predict(encode_position(position));
            if (first) {
                first = false;
                prediction.policy = neural_mcts_detail::normalize_priors(position, prediction);
                const auto legal = position.legal_moves();
                const double uniform = mixture / static_cast<double>(legal.size());
                for (Move move : legal) {
                    const auto action = action_index(move, position.size());
                    prediction.policy[action] = (1 - mixture) * prediction.policy[action] + uniform;
                }
            }
            return prediction;
        };
        NeuralMctsSearch search(state, settings.search, std::move(evaluator));
        search.step(settings.search.simulations);
        const auto statistics = search.statistics();
        auto policy = visit_policy(state, statistics);
        Move move = search.best_move();
        if (settings.temperature > 0 && ply < settings.temperature_moves)
            move = action_from_index(sample_policy(sampling_policy(policy, settings.temperature), random), state.size());
        targets.push_back({{"state", state_json(state)}, {"policy", policy},
                           {"search", search_json(state, statistics)}});
        examples.push_back({encode_position(state), std::move(policy), 0});
        if (progress) progress(ply, state, statistics);
        state = state.play(move);
        moves.push_back(move);
        if (accepted_move) accepted_move(ply + 1, state, move);
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    Json record = GameResult{state, moves, settings.max_moves, elapsed}.to_json();
    record["schema_version"] = 1;
    record["kind"] = "self_play_game";
    record["feature_schema"] = FEATURE_SCHEMA;
    record["value_perspective"] = "player_to_move";
    record["seed"] = seed;
    record["self_play_settings"] = settings.to_json();
    if (state.is_terminal()) {
        const auto winner = state.winner();
        for (std::size_t index = 0; index < examples.size(); ++index) {
            // Games start with Black and alternate, including after passes.
            const int player = index % 2 == 0 ? BLACK : WHITE;
            const double result = winner ? (*winner == player ? 1.0 : -1.0) : 0.0;
            examples[index].value = result;
            examples[index].validate();
            targets[index]["value"] = result;
        }
    } else {
        // A move limit is not a final score. Retain the replay/search evidence
        // but create no value labels or training examples from the game.
        examples.clear();
    }
    record["targets"] = std::move(targets);
    return {std::move(record), std::move(examples)};
}

std::vector<TrainingExample> validate_self_play_game(const Json& record) {
    ProfileScope scope(ProfileWork::ReplayValidation);
    try {
        integer(field(record, "schema_version"), 1, 1, "Self-play schema version");
        text_equals(field(record, "kind"), "self_play_game", "record kind");
        text_equals(field(record, "feature_schema"), FEATURE_SCHEMA, "feature schema");
        text_equals(field(record, "value_perspective"), "player_to_move", "value perspective");
        const auto seed = integer64(field(record, "seed"), std::numeric_limits<std::int64_t>::min(),
                                    std::numeric_limits<std::int64_t>::max(), "Self-play seed");
        Random selection_random(seed);
        const auto settings = settings_from_json(field(record, "self_play_settings"));
        if (integer(field(record, "size"), 1, 19, "Board size") != settings.board_size ||
            number(field(record, "komi"), "Komi") != settings.komi ||
            integer(field(record, "max_moves"), 1, std::numeric_limits<int>::max(), "Move limit") != settings.max_moves)
            throw std::invalid_argument("Self-play game settings disagree with its metadata");
        const auto moves = checked_moves(field(record, "moves"), settings.board_size);
        if (moves.empty() || moves.size() > static_cast<std::size_t>(settings.max_moves))
            throw std::invalid_argument("Self-play game has an invalid move count");
        const auto final_state = replay_moves(moves, settings.board_size, settings.komi);
        if (!final_state.is_terminal() && moves.size() != static_cast<std::size_t>(settings.max_moves))
            throw std::invalid_argument("Truncated self-play must reach its move limit");
        const double elapsed = number(field(record, "elapsed_seconds"), "Elapsed seconds");
        if (elapsed < 0) throw std::invalid_argument("Elapsed seconds must be nonnegative");
        integer(field(record, "game_length"), 1, settings.max_moves, "Game length");
        const Json expected = GameResult{final_state, moves, settings.max_moves, elapsed}.to_json();
        for (const auto* key : {"game_length", "termination_reason", "score", "winner"})
            if (field(record, key) != expected.at(key))
                throw std::invalid_argument(std::string("Self-play ") + key + " disagrees with replay");
        if (!field(record, "winner").is_null())
            integer(field(record, "winner"), BLACK, WHITE, "Winner");
        const auto& targets = field(record, "targets");
        if (!targets.is_array() || targets.size() != moves.size())
            throw std::invalid_argument("Each self-play action must have exactly one pre-action target");
        std::vector<TrainingExample> examples;
        examples.reserve(targets.size());
        GameState state = GameState::new_game(settings.board_size, settings.komi);
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const auto& target = targets[index];
            validate_state_json(field(target, "state"), state);
            const auto& search = field(target, "search");
            const int simulations = integer(field(search, "simulations"), 1,
                std::numeric_limits<int>::max() - 1, "Simulation count");
            const int root_visits = integer(field(search, "root_visits"), 1,
                std::numeric_limits<int>::max() - 1, "Root visits");
            const int network = integer(field(search, "network_evaluations"), 1,
                std::numeric_limits<int>::max(), "Network evaluations");
            const int terminal = integer(field(search, "terminal_evaluations"), 0,
                std::numeric_limits<int>::max(), "Terminal evaluations");
            if (simulations != settings.search.simulations || root_visits != simulations ||
                static_cast<std::int64_t>(network) + terminal != static_cast<std::int64_t>(simulations) + 1)
                throw std::invalid_argument("Recorded self-play search counts disagree with its budget");
            const auto& visits = field(search, "action_visits");
            const auto& probabilities = field(target, "policy");
            const auto action_count = static_cast<std::size_t>(settings.board_size * settings.board_size + 1);
            if (!visits.is_array() || visits.size() != action_count ||
                !probabilities.is_array() || probabilities.size() != action_count)
                throw std::invalid_argument("Self-play target action shape does not match the board");
            const auto input = encode_position(state);
            std::vector<double> policy;
            policy.reserve(action_count);
            std::int64_t visit_sum = 0;
            for (std::size_t action = 0; action < action_count; ++action) {
                const int count = integer(visits[action], 0, simulations, "Action visits");
                visit_sum += count;
                const double probability = number(probabilities[action], "Policy probability");
                if ((!input.legal_actions[action] && count != 0) ||
                    std::abs(probability - static_cast<double>(count) / simulations) > 1e-12)
                    throw std::invalid_argument("Self-play policy must equal normalized legal root visits");
                policy.push_back(probability);
            }
            if (visit_sum != simulations)
                throw std::invalid_argument("Recorded root action visits do not sum to the simulation budget");
            // A selected action must have been visited, both for temperature
            // sampling and the later maximum-visit choice.
            if (policy[action_index(moves[index], settings.board_size)] <= 0)
                throw std::invalid_argument("Self-play selected action has no root visits");
            const auto selected = static_cast<std::size_t>(action_index(moves[index], settings.board_size));
            if (settings.temperature > 0 && index < static_cast<std::size_t>(settings.temperature_moves)) {
                if (sample_policy(sampling_policy(policy, settings.temperature), selection_random) != selected)
                    throw std::invalid_argument("Self-play action disagrees with seeded temperature sampling");
            } else if (policy[selected] != *std::max_element(policy.begin(), policy.end())) {
                throw std::invalid_argument("Deterministic self-play must select a maximum-visit root action");
            }
            double result = 0;
            if (final_state.is_terminal()) {
                const auto winner = final_state.winner();
                result = winner ? (*winner == state.to_play() ? 1.0 : -1.0) : 0.0;
                if (number(field(target, "value"), "Training value") != result)
                    throw std::invalid_argument("Self-play value label must describe the state's player to move");
            } else if (target.contains("value")) {
                throw std::invalid_argument("Truncated self-play games must not have final-result labels");
            }
            TrainingExample example{input, std::move(policy), result};
            example.validate();
            if (final_state.is_terminal()) examples.push_back(std::move(example));
            state = state.play(moves[index]);
        }
        return examples;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed self-play record: ") + error.what());
    }
}

Json self_play_dataset(const Json& games) {
    if (!games.is_array() || games.empty())
        throw std::invalid_argument("Self-play dataset requires a nonempty game array");
    Json examples = Json::array();
    int board_size = 0;
    std::optional<double> komi;
    for (const auto& game : games) {
        const auto validated = validate_self_play_game(game);
        const int size = game.at("size").get<int>();
        const double game_komi = game.at("komi").get<double>();
        if ((board_size != 0 && board_size != size) || (komi && *komi != game_komi))
            throw std::invalid_argument("Self-play dataset games must use one board size and komi");
        board_size = size;
        komi = game_komi;
        if (validated.empty()) continue;
        for (const auto& target : game.at("targets"))
            examples.push_back({{"state", target.at("state")}, {"policy", target.at("policy")},
                                {"value", target.at("value")}});
    }
    if (examples.empty())
        throw std::invalid_argument("Self-play dataset has no completed games to label");
    return {{"schema_version", 1}, {"kind", "self_play_examples"},
            {"feature_schema", FEATURE_SCHEMA}, {"value_perspective", "player_to_move"},
            {"description", "Pre-action positions with normalized neural MCTS root visits and completed-game results; truncated games excluded."},
            {"examples", std::move(examples)}};
}
} // namespace betago
