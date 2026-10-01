#include "betago/pretraining.hpp"
#include "betago/random.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace betago {
namespace {
Point transformed_point(Point point, int size, int symmetry) {
    if (symmetry < 0 || symmetry > 7) throw std::invalid_argument("Expert symmetry must be 0..7");
    if (symmetry >= 4) point.column = size - 1 - point.column;
    for (int rotation = 0; rotation < symmetry % 4; ++rotation)
        point = {point.column, size - 1 - point.row};
    return point;
}
Move transformed_move(Move move, int size, int symmetry) {
    return move ? Move(transformed_point(*move, size, symmetry)) : PASS;
}
Board transformed_board(const Board& board, int symmetry) {
    const int size = static_cast<int>(board.size());
    Board result(size, std::vector<int>(size));
    for (int row = 0; row < size; ++row) for (int column = 0; column < size; ++column) {
        const auto point = transformed_point({row, column}, size, symmetry);
        result[point.row][point.column] = board[row][column];
    }
    return result;
}
GameState transformed_state(const GameState& state, int symmetry) {
    return GameState(transformed_board(state.board(), symmetry), state.to_play(), state.komi(),
        state.consecutive_passes(), state.previous_board() ?
            std::optional<Board>(transformed_board(*state.previous_board(), symmetry)) : std::nullopt);
}
Json state_json(const GameState& state) {
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi() == 0 ? 0.0 : state.komi()},
        {"consecutive_passes", state.consecutive_passes()},
        {"previous_board", state.previous_board() ? Json(*state.previous_board()) : Json(nullptr)}};
}
std::string text_fingerprint(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : text) { hash ^= character; hash *= 1099511628211ULL; }
    std::ostringstream output; output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}
std::string file_fingerprint(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot read source checkpoint: " + path.string());
    std::uint64_t hash = 14695981039346656037ULL;
    char block[65536];
    while (input) {
        input.read(block, sizeof(block));
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(block[i]); hash *= 1099511628211ULL;
        }
    }
    if (!input.eof()) throw std::runtime_error("Cannot fingerprint source checkpoint");
    std::ostringstream output; output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}
// Group rotated/reflected copies together as well as identical copies. Result
// labels stay outside the key so conflicting labels cannot enter different splits.
std::string split_key(const ExpertGame& game) {
    std::string minimum;
    for (int symmetry = 0; symmetry < 8; ++symmetry) {
        Json moves = Json::array();
        for (const auto move : game.moves) {
            const auto transformed = transformed_move(move, game.initial_state.size(), symmetry);
            moves.push_back(transformed ? Json::array({transformed->row, transformed->column}) : Json(nullptr));
        }
        const auto candidate = Json{{"initial_state", state_json(transformed_state(game.initial_state, symmetry))},
            {"moves", std::move(moves)}}.dump();
        if (symmetry == 0 || candidate < minimum) minimum = candidate;
    }
    return minimum;
}
struct Dataset {
    std::vector<TrainingExample> examples;
    Json records = Json::array();
    Json game_counts = Json::array();
};
Dataset make_dataset(const std::vector<ExpertGame>& games, bool augment) {
    Dataset dataset;
    for (const auto& game : games) {
        const auto id = expert_game_id(game);
        const auto examples = expert_examples(game);
        const std::size_t multiplier = augment ? 8 : 1;
        if (examples.size() > (static_cast<std::size_t>(std::numeric_limits<int>::max()) - dataset.examples.size()) / multiplier)
            throw std::invalid_argument("Too many expert training examples");
        GameState state = game.initial_state;
        for (std::size_t ply = 0; ply < examples.size(); ++ply) {
            for (int symmetry = 0; symmetry < static_cast<int>(multiplier); ++symmetry) {
                auto example = augment_expert_example(examples[ply], symmetry);
                dataset.records.push_back({{"state", state_json(transformed_state(state, symmetry))},
                    {"policy", example.policy}, {"value", example.value}, {"game_id", id},
                    {"ply", ply}, {"symmetry", symmetry}});
                dataset.examples.push_back(std::move(example));
            }
            state = state.play(game.moves[ply]);
        }
        dataset.game_counts.push_back({{"id", id}, {"source", game.source}, {"moves", game.moves.size()},
            {"examples", examples.size() * multiplier}, {"split_identity", text_fingerprint(split_key(game))}});
    }
    return dataset;
}
Json dataset_json(const Dataset& dataset, const char* split, bool augmented) {
    return {{"schema_version", 1}, {"kind", "scripted_go_examples"}, {"feature_schema", FEATURE_SCHEMA},
        {"value_perspective", "player_to_move"},
        {"description", "Supervised expert-game moves and reported outcome labels; playing strength is not established by dataset fit."},
        {"provenance", {{"kind", "expert_games"}, {"split", split}, {"augmentation", augmented ? "D4 eight symmetries" : "original only"},
            {"games", dataset.game_counts}}}, {"examples", dataset.records}};
}
Json metrics(const PolicyValueNetwork& network, const std::vector<TrainingExample>& examples, int batch_size) {
    double policy = 0, value = 0;
    for (std::size_t start = 0; start < examples.size(); start += static_cast<std::size_t>(batch_size)) {
        const auto end = std::min(examples.size(), start + static_cast<std::size_t>(batch_size));
        const std::vector<TrainingExample> batch(examples.begin() + start, examples.begin() + end);
        const auto loss = network.evaluate_batch(batch, 0);
        policy += loss.policy * static_cast<double>(batch.size());
        value += loss.value * static_cast<double>(batch.size());
    }
    policy /= static_cast<double>(examples.size()); value /= static_cast<double>(examples.size());
    if (!std::isfinite(policy) || !std::isfinite(value)) throw std::runtime_error("Expert evaluation produced nonfinite losses");
    return {{"policy_cross_entropy", policy}, {"value_mse", value}, {"combined_loss", policy + value}, {"examples", examples.size()}};
}
void atomic_checkpoint(const PolicyValueNetwork& network, const std::filesystem::path& target) {
    auto temporary = target; temporary += ".pretraining-tmp";
    if (std::filesystem::exists(temporary)) throw std::runtime_error("Unexpected pretraining temporary checkpoint exists");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temporary};
    network.save(temporary);
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) ||
            std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Cannot atomically replace expert checkpoint: " + target.string() +
                " (Windows error " + std::to_string(error) + ")");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    std::filesystem::rename(temporary, target);
#endif
}
} // namespace

void PretrainingSettings::validate() const {
    network.validate(); optimizer.validate();
    if (epochs < 1 || batch_size < 1) throw std::invalid_argument("Expert epochs and batch size must be positive");
    if (!std::isfinite(validation_fraction) || validation_fraction <= 0 || validation_fraction >= 1)
        throw std::invalid_argument("Expert validation_fraction must be between zero and one exclusively");
}
Json PretrainingSettings::to_json() const {
    return {{"epochs", epochs}, {"batch_size", batch_size}, {"validation_fraction", validation_fraction},
        {"seed", seed}, {"augment", augment},
        {"network", {{"board_size", network.board_size}, {"channels", network.channels}, {"value_hidden", network.value_hidden}}},
        {"optimizer", {{"learning_rate", optimizer.learning_rate}, {"momentum", optimizer.momentum}, {"l2", optimizer.l2}}}};
}
std::vector<TrainingExample> expert_examples(const ExpertGame& input) {
    const auto game = expert_game_from_json(expert_game_json(input));
    if (game.moves.empty()) throw std::invalid_argument("Expert games require at least one move");
    std::vector<TrainingExample> result;
    result.reserve(game.moves.size());
    auto state = game.initial_state;
    for (const auto move : game.moves) {
        auto encoded = encode_position(state);
        std::vector<double> policy(encoded.legal_actions.size());
        policy.at(static_cast<std::size_t>(action_index(move, state.size()))) = 1.0;
        const double value = game.winner == EMPTY ? 0.0 : (game.winner == state.to_play() ? 1.0 : -1.0);
        TrainingExample example{std::move(encoded), std::move(policy), value};
        example.validate(); result.push_back(std::move(example));
        state = state.play(move);
    }
    return result;
}
TrainingExample augment_expert_example(const TrainingExample& example, int symmetry) {
    example.validate();
    if (symmetry < 0 || symmetry > 7) throw std::invalid_argument("Expert symmetry must be 0..7");
    auto result = example;
    const int size = example.input.board_size, area = size * size;
    for (int row = 0; row < size; ++row) for (int column = 0; column < size; ++column) {
        const auto target = transformed_point({row, column}, size, symmetry);
        const auto source_action = row * size + column, target_action = target.row * size + target.column;
        for (int channel = 0; channel < FEATURE_CHANNELS; ++channel)
            result.input.features[channel * area + target_action] = example.input.features[channel * area + source_action];
        result.input.legal_actions[target_action] = example.input.legal_actions[source_action];
        result.policy[target_action] = example.policy[source_action];
    }
    result.validate();
    return result;
}
Json pretrain_expert_games(std::vector<ExpertGame> games, const PretrainingSettings& requested,
    const std::optional<std::filesystem::path>& checkpoint, const std::filesystem::path& output_directory,
    PretrainingProgress progress) {
    requested.validate();
    if (!std::filesystem::is_directory(output_directory))
        throw std::invalid_argument("Expert pretraining output must be an existing directory");
    for (const auto* file : {"model.json", "final.json", "report.json", "train.json", "validation.json",
                            "model.json.pretraining-tmp", "final.json.pretraining-tmp"})
        if (std::filesystem::exists(output_directory / file))
            throw std::invalid_argument("Expert pretraining must not overwrite an existing output: " + std::string(file));
    const auto started = std::chrono::steady_clock::now();
    std::map<std::string, std::size_t> seen;
    std::vector<ExpertGame> unique;
    Json duplicates = Json::array(), source_counts = Json::object();
    const auto received_games = games.size();
    for (const auto& input : games) {
        auto game = expert_game_from_json(expert_game_json(input));
        if (game.moves.empty()) throw std::invalid_argument("Expert games require at least one move");
        if (game.initial_state.size() != requested.network.board_size)
            throw std::invalid_argument("Expert board size disagrees with pretraining settings");
        if (!unique.empty() && game.initial_state.komi() != unique.front().initial_state.komi())
            throw std::invalid_argument("Expert games must use consistent komi");
        const auto key = split_key(game);
        if (const auto found = seen.find(key); found != seen.end()) {
            const auto& original = unique.at(found->second);
            if (original.winner != game.winner) throw std::invalid_argument("Duplicate expert move history has conflicting outcome labels");
            duplicates.push_back({{"id", expert_game_id(game)}, {"source", game.source}, {"kept_id", expert_game_id(original)},
                {"reason", "duplicate move history under D4 symmetry"}});
            continue;
        }
        seen.emplace(key, unique.size());
        if (!source_counts.contains(game.source)) source_counts[game.source] = 0;
        source_counts[game.source] = source_counts[game.source].get<std::size_t>() + 1;
        unique.push_back(std::move(game));
    }
    if (unique.size() < 2) throw std::invalid_argument("Expert pretraining requires at least two unique whole games");
    Random random(requested.seed);
    for (std::size_t count = unique.size(); count > 1; --count)
        std::swap(unique[count - 1], unique[random.below(count)]);
    const auto validation_count = std::clamp(static_cast<std::size_t>(std::ceil(requested.validation_fraction * unique.size())),
        std::size_t{1}, unique.size() - 1);
    const std::vector<ExpertGame> validation_games(unique.begin(), unique.begin() + validation_count);
    const std::vector<ExpertGame> training_games(unique.begin() + validation_count, unique.end());
    auto training = make_dataset(training_games, requested.augment);
    auto validation = make_dataset(validation_games, false);
    const auto checkpoint_identity = checkpoint ? Json(file_fingerprint(*checkpoint)) : Json(nullptr);
    auto network = checkpoint ? PolicyValueNetwork::load(*checkpoint) : PolicyValueNetwork(requested.network, requested.seed);
    if (checkpoint && file_fingerprint(*checkpoint) != checkpoint_identity.get<std::string>())
        throw std::runtime_error("Source checkpoint changed while loading");
    if (network.settings().board_size != requested.network.board_size)
        throw std::invalid_argument("Expert games and initial checkpoint use different board sizes");
    auto settings = requested; settings.network = network.settings();
    save_records_atomic(output_directory / "train.json", dataset_json(training, "training", settings.augment));
    save_records_atomic(output_directory / "validation.json", dataset_json(validation, "validation", false));
    const auto initial_steps = network.training_steps();
    network.train(false);
    Json initial = {{"training", metrics(network, training.examples, settings.batch_size)},
        {"validation", metrics(network, validation.examples, settings.batch_size)}};
    auto best = network;
    auto best_metrics = initial;
    auto final_metrics = initial;
    double best_loss = initial.at("validation").at("combined_loss").get<double>();
    int selected_epoch = 0;
    atomic_checkpoint(best, output_directory / "model.json");
    Json history = Json::array();
    int completed_epochs = 0;
    auto snapshot = [&](bool completed) {
        return Json{{"schema_version", 1}, {"kind", "expert_game_pretraining"}, {"feature_schema", FEATURE_SCHEMA},
            {"status", completed ? "completed" : "running"},
            {"settings", settings.to_json()}, {"requested_settings", requested.to_json()},
            {"received_games", received_games}, {"unique_games", unique.size()}, {"duplicates", duplicates},
            {"duplicate_count", duplicates.size()}, {"source_counts", source_counts},
            {"split", {{"unit", "whole game"}, {"duplicate_identity", "D4 canonical initial state and move history; outcome conflicts rejected"},
                {"training_games", training.game_counts}, {"validation_games", validation.game_counts}}},
            {"initial_checkpoint", checkpoint ? Json(std::filesystem::absolute(*checkpoint).string()) : Json(nullptr)},
            {"initial_checkpoint_fingerprint_fnv1a64", checkpoint_identity},
            {"fingerprint_note", "FNV-1a 64-bit identifies inputs; it is not a cryptographic signature."},
            {"initial_training_steps", initial_steps}, {"final_training_steps", network.training_steps()},
            {"selected_training_steps", best.training_steps()}, {"epochs_completed", completed_epochs}, {"selected_epoch", selected_epoch},
            {"selection_metric", "Held-out inference policy cross entropy + value MSE, without L2; earliest strict minimum including epoch zero"},
            {"initial", initial}, {"final", final_metrics},
            {"best", best_metrics}, {"history", history}, {"model", "model.json"},
            {"final_model", completed ? Json("final.json") : Json(nullptr)},
            {"training_dataset", "train.json"}, {"validation_dataset", "validation.json"},
            {"parameter_count", network.parameter_count()},
            {"elapsed_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
            {"strength_claim", false}};
    };
    save_records_atomic(output_directory / "report.json", snapshot(false));
    std::vector<std::size_t> order(training.examples.size()); std::iota(order.begin(), order.end(), 0);
    for (int epoch_index = 0; epoch_index < settings.epochs; ++epoch_index) {
        const int epoch = epoch_index + 1;
        for (std::size_t count = order.size(); count > 1; --count) std::swap(order[count - 1], order[random.below(count)]);
        network.train();
        for (std::size_t start = 0; start < order.size(); start += static_cast<std::size_t>(settings.batch_size)) {
            const auto end = std::min(order.size(), start + static_cast<std::size_t>(settings.batch_size));
            std::vector<TrainingExample> batch; batch.reserve(end - start);
            for (std::size_t index = start; index < end; ++index) batch.push_back(training.examples[order[index]]);
            network.train_batch(batch, settings.optimizer);
        }
        network.train(false);
        Json current = {{"training", metrics(network, training.examples, settings.batch_size)},
            {"validation", metrics(network, validation.examples, settings.batch_size)}};
        final_metrics = current;
        const auto loss = current.at("validation").at("combined_loss").get<double>();
        const bool selected = loss < best_loss;
        if (selected) {
            best = network; best_metrics = current; best_loss = loss; selected_epoch = epoch;
            atomic_checkpoint(best, output_directory / "model.json");
        }
        current["epoch"] = epoch; current["training_steps"] = network.training_steps(); current["selected"] = selected;
        history.push_back(current);
        completed_epochs = epoch;
        save_records_atomic(output_directory / "report.json", snapshot(false));
        if (progress) progress(epoch, current);
    }
    atomic_checkpoint(network, output_directory / "final.json");
    if (checkpoint && file_fingerprint(*checkpoint) != checkpoint_identity.get<std::string>())
        throw std::runtime_error("Source checkpoint changed while pretraining");
    auto report = snapshot(true);
    save_records_atomic(output_directory / "report.json", report);
    return report;
}
} // namespace betago
