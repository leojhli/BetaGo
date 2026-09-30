#include "betago/replay.hpp"
#include "betago/profile.hpp"
#include "betago/selfplay.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace betago {
namespace {
const Json& field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key))
        throw std::invalid_argument(std::string("Replay is missing field: ") + key);
    return object.at(key);
}
int integer(const Json& value, int minimum, int maximum, const char* name) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum) ||
            (minimum > 0 && number < static_cast<std::uint64_t>(minimum)))
            throw std::invalid_argument(std::string(name) + " is out of range");
        return static_cast<int>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < minimum || number > maximum)
        throw std::invalid_argument(std::string(name) + " is out of range");
    return static_cast<int>(number);
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
        throw std::invalid_argument(std::string("Unsupported replay ") + name);
}
Json loss_json(const LossMetrics& loss) {
    return {{"policy_cross_entropy", loss.policy}, {"value_mse", loss.value},
            {"regularization", loss.regularization}, {"total", loss.total}};
}
Json full_metrics(const PolicyValueNetwork& network, const std::vector<TrainingExample>& examples,
                  double l2, double target_entropy) {
    const auto loss = network.evaluate_batch(examples, l2);
    // Cross-entropy includes the target entropy, even for an exact prediction.
    // KL subtracts that constant to measure agreement with soft visit targets.
    return {{"loss", loss_json(loss)}, {"target_policy_entropy", target_entropy},
            {"policy_kl", std::max(0.0, loss.policy - target_entropy)}};
}
} // namespace

ReplayBuffer::ReplayBuffer(int capacity, int board_size, double komi)
    : capacity_(capacity), board_size_(board_size), komi_(komi) {
    if (capacity < 1) throw std::invalid_argument("Replay capacity must be positive");
    GameState::new_game(board_size, komi);
}

bool ReplayBuffer::add(const Json& selfplay_game) {
    // Validate even rejected/truncated games, so malformed records never hide
    // behind the decision to omit their unlabeled positions.
    auto positions = validate_self_play_game(selfplay_game);
    if (selfplay_game.at("size").get<int>() != board_size_ ||
        selfplay_game.at("komi").get<double>() != komi_)
        throw std::invalid_argument("Self-play game disagrees with replay board size or komi");
    if (positions.empty()) return false;
    games_.push_back({selfplay_game, std::move(positions)});
    if (games_.size() > static_cast<std::size_t>(capacity_)) games_.pop_front();
    return true;
}

std::vector<TrainingExample> ReplayBuffer::examples() const {
    std::size_t count = 0;
    for (const auto& game : games_) {
        if (game.examples.size() > std::numeric_limits<std::uint32_t>::max() - count)
            throw std::invalid_argument("Replay has too many positions for the portable sampler");
        count += game.examples.size();
    }
    std::vector<TrainingExample> result;
    result.reserve(count);
    for (const auto& game : games_)
        result.insert(result.end(), game.examples.begin(), game.examples.end());
    return result;
}

Json ReplayBuffer::to_json() const {
    Json games = Json::array();
    for (const auto& game : games_) games.push_back(game.record);
    return {{"schema_version", 1}, {"kind", "recent_self_play_replay"},
            {"feature_schema", FEATURE_SCHEMA}, {"value_perspective", "player_to_move"},
            {"capacity", capacity_}, {"board_size", board_size_}, {"komi", komi_},
            {"games", std::move(games)}};
}

ReplayBuffer ReplayBuffer::from_json(const Json& data) {
    try {
        integer(field(data, "schema_version"), 1, 1, "Replay schema version");
        text_equals(field(data, "kind"), "recent_self_play_replay", "kind");
        text_equals(field(data, "feature_schema"), FEATURE_SCHEMA, "feature schema");
        text_equals(field(data, "value_perspective"), "player_to_move", "value perspective");
        ReplayBuffer result(integer(field(data, "capacity"), 1, std::numeric_limits<int>::max(), "Replay capacity"),
                            integer(field(data, "board_size"), 1, 19, "Replay board size"),
                            number(field(data, "komi"), "Replay komi"));
        const auto& games = field(data, "games");
        if (!games.is_array() || games.size() > static_cast<std::size_t>(result.capacity()))
            throw std::invalid_argument("Replay must contain an array of at most capacity games");
        for (const auto& game : games)
            if (!result.add(game))
                throw std::invalid_argument("Persisted replay games must have a completed result");
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed replay: ") + error.what());
    }
}

ReplayBuffer ReplayBuffer::load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open replay " + path.string());
    try {
        return from_json(Json::parse(stream));
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed replay JSON: ") + error.what());
    }
}

void ReplayBuffer::save(const std::filesystem::path& path) const {
    save_records(path, to_json());
}

void TrainingSettings::validate() const {
    if (updates < 1) throw std::invalid_argument("Training update count must be positive");
    if (batch_size < 1) throw std::invalid_argument("Training batch size must be positive");
    optimizer.validate();
}

Json TrainingSettings::to_json() const {
    return {{"updates", updates}, {"batch_size", batch_size},
            {"learning_rate", optimizer.learning_rate}, {"momentum", optimizer.momentum},
            {"l2", optimizer.l2}, {"sampling", "uniform_positions_with_replacement"}};
}

std::vector<TrainingExample> sample_replay_batch(const std::vector<TrainingExample>& examples,
                                               int batch_size, Random& random) {
    if (examples.empty()) throw std::invalid_argument("Cannot sample an empty replay");
    if (batch_size < 1) throw std::invalid_argument("Training batch size must be positive");
    if (examples.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Replay has too many positions for the portable sampler");
    std::vector<TrainingExample> batch;
    batch.reserve(static_cast<std::size_t>(batch_size));
    for (int index = 0; index < batch_size; ++index) batch.push_back(examples[random.below(examples.size())]);
    return batch;
}

Json train_candidate(PolicyValueNetwork& candidate, const ReplayBuffer& replay,
                     const TrainingSettings& settings, std::int64_t seed, TrainingProgress progress) {
    ProfileScope scope(ProfileWork::CandidateTraining);
    settings.validate();
    if (candidate.settings().board_size != replay.board_size())
        throw std::invalid_argument("Candidate board size disagrees with replay");
    auto examples = replay.examples();
    if (examples.empty()) throw std::invalid_argument("Training requires at least one completed self-play game");
    if (static_cast<std::uint64_t>(settings.updates) >
        std::numeric_limits<std::uint64_t>::max() - candidate.training_steps())
        throw std::invalid_argument("Training updates would overflow checkpoint step count");
    double entropy = 0;
    for (const auto& example : examples) {
        example.validate();
        if (example.input.board_size != candidate.settings().board_size)
            throw std::invalid_argument("Replay example board size disagrees with candidate");
        for (double probability : example.policy)
            if (probability > 0) entropy -= probability * std::log(probability);
    }
    entropy /= static_cast<double>(examples.size());
    const Json initial = full_metrics(candidate, examples, settings.optimizer.l2, entropy);
    const auto initial_steps = candidate.training_steps();
    const bool previous_mode = candidate.training();
    Json history = Json::array();
    Random random(seed);
    try {
        candidate.train();
        for (int index = 0; index < settings.updates; ++index) {
            auto batch = sample_replay_batch(examples, settings.batch_size, random);
            const auto loss = candidate.train_batch(batch, settings.optimizer);
            history.push_back({{"update", index + 1}, {"training_steps", candidate.training_steps()},
                               {"loss_before_update", loss_json(loss)}});
            if (progress) progress(index + 1, loss);
        }
        candidate.train(false);
        const Json final = full_metrics(candidate, examples, settings.optimizer.l2, entropy);
        return {{"schema_version", 1}, {"kind", "self_play_candidate_training"},
                {"feature_schema", FEATURE_SCHEMA}, {"value_perspective", "player_to_move"},
                {"seed", seed}, {"settings", settings.to_json()},
                {"num_games", replay.game_count()}, {"num_examples", examples.size()},
                {"parameter_count", candidate.parameter_count()},
                {"initial_training_steps", initial_steps}, {"final_training_steps", candidate.training_steps()},
                {"initial", initial}, {"final", final}, {"history", std::move(history)}};
    } catch (...) {
        candidate.train(previous_mode);
        throw;
    }
}
} // namespace betago
