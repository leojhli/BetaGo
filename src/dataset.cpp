#include "betago/dataset.hpp"
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace betago {
namespace {
const Json& field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key))
        throw std::invalid_argument(std::string("Dataset is missing field: ") + key);
    return object.at(key);
}
int integer(const Json& value, int minimum, int maximum, const char* name) {
    if (!value.is_number_integer())
        throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum))
            throw std::invalid_argument(std::string(name) + " is out of range");
        if (minimum > 0 && number < static_cast<std::uint64_t>(minimum))
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
        throw std::invalid_argument(std::string("Unsupported dataset ") + name);
}
Board board_from_json(const Json& data) {
    if (!data.is_array() || data.empty() || data.size() > 19)
        throw std::invalid_argument("Dataset boards must be square with size between 1 and 19");
    Board result;
    result.reserve(data.size());
    for (const auto& row : data) {
        if (!row.is_array() || row.size() != data.size())
            throw std::invalid_argument("Dataset boards must be square");
        std::vector<int> values;
        values.reserve(row.size());
        for (const auto& cell : row) values.push_back(integer(cell, EMPTY, WHITE, "Stone color"));
        result.push_back(std::move(values));
    }
    return result;
}
GameState state_from_json(const Json& data) {
    Board board = board_from_json(field(data, "board"));
    const int to_play = integer(field(data, "to_play"), BLACK, WHITE, "Player to move");
    const double komi = number(field(data, "komi"), "Komi");
    const int passes = integer(field(data, "consecutive_passes"), 0, 2, "Consecutive passes");
    const auto& previous = field(data, "previous_board");
    std::optional<Board> previous_board;
    if (!previous.is_null()) previous_board = board_from_json(previous);
    return GameState(std::move(board), to_play, komi, passes, std::move(previous_board));
}
Json state_to_json(const GameState& state) {
    Json previous = state.previous_board() ? Json(*state.previous_board()) : Json(nullptr);
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi()},
            {"consecutive_passes", state.consecutive_passes()}, {"previous_board", std::move(previous)}};
}
} // namespace

Json make_demo_dataset() {
    const std::vector<Move> moves{Point{0, 1}, Point{1, 1}, Point{1, 0}, Point{8, 8},
                                Point{2, 1}, Point{8, 7}, Point{1, 2}, PASS, PASS};
    GameState state = GameState::new_game(9, 0.5);
    std::vector<GameState> positions;
    positions.reserve(moves.size());
    for (Move move : moves) {
        positions.push_back(state);
        state = state.play(move);
    }
    const auto winner = state.winner();
    Json examples = Json::array();
    for (std::size_t index = 0; index < positions.size(); ++index) {
        std::vector<double> policy(82, 0.0);
        policy[action_index(moves[index], 9)] = 1.0;
        const double value = winner ? (*winner == positions[index].to_play() ? 1.0 : -1.0) : 0.0;
        TrainingExample example{encode_position(positions[index]), policy, value};
        example.validate();
        examples.push_back({{"state", state_to_json(positions[index])},
                            {"policy", std::move(policy)}, {"value", value}});
    }
    return {{"schema_version", 1}, {"kind", "scripted_go_examples"},
            {"feature_schema", FEATURE_SCHEMA}, {"value_perspective", "player_to_move"},
            {"description", "Fixed capture-and-pass examples for a memorization check; not self-play or evidence of playing strength."},
            {"examples", std::move(examples)}};
}

std::vector<TrainingExample> dataset_from_json(const Json& data) {
    try {
        if (integer(field(data, "schema_version"), 1, 1, "Dataset schema version") != 1)
            throw std::invalid_argument("Unsupported dataset schema version");
        text_equals(field(data, "kind"), "scripted_go_examples", "kind");
        text_equals(field(data, "feature_schema"), FEATURE_SCHEMA, "feature schema");
        text_equals(field(data, "value_perspective"), "player_to_move", "value perspective");
        if (!field(data, "description").is_string())
            throw std::invalid_argument("Dataset description must be a string");
        const auto& examples = field(data, "examples");
        if (!examples.is_array() || examples.empty() ||
            examples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Dataset must contain a valid nonempty example array");
        std::vector<TrainingExample> result;
        result.reserve(examples.size());
        int board_size = 0;
        for (const auto& record : examples) {
            GameState state = state_from_json(field(record, "state"));
            if (board_size && state.size() != board_size)
                throw std::invalid_argument("Dataset examples must use one board size");
            board_size = state.size();
            const auto& probabilities = field(record, "policy");
            if (!probabilities.is_array() || probabilities.size() != static_cast<std::size_t>(board_size * board_size + 1))
                throw std::invalid_argument("Dataset policy has the wrong action shape");
            std::vector<double> policy;
            policy.reserve(probabilities.size());
            for (const auto& probability : probabilities) policy.push_back(number(probability, "Policy probability"));
            TrainingExample example{encode_position(state), std::move(policy), number(field(record, "value"), "Training value")};
            example.validate();
            result.push_back(std::move(example));
        }
        return result;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed dataset: ") + error.what());
    }
}

std::vector<TrainingExample> load_dataset(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open dataset " + path.string());
    try {
        return dataset_from_json(Json::parse(stream));
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Malformed dataset JSON: ") + error.what());
    }
}
} // namespace betago
