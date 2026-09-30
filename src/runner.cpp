#include "betago/runner.hpp"
#include <climits>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace betago {
std::string GameResult::termination_reason() const {
    return final_state.is_terminal() ? "two_passes" : "move_limit";
}
std::optional<Score> GameResult::score() const {
    if (!final_state.is_terminal()) return std::nullopt;
    return final_state.score();
}
std::optional<int> GameResult::winner() const {
    return final_state.is_terminal() ? final_state.winner() : std::nullopt;
}
Json GameResult::to_json() const {
    Json actions = Json::array();
    for (auto move : moves) {
        if (move) actions.push_back({move->row, move->column});
        else actions.push_back(nullptr);
    }
    Json points = nullptr;
    if (auto s = score()) points = {{"black", s->black}, {"white", s->white}};
    Json victor = nullptr;
    if (auto w = winner()) victor = *w;
    return {{"size", final_state.size()}, {"komi", final_state.komi()},
            {"max_moves", max_moves}, {"moves", actions}, {"game_length", moves.size()},
            {"elapsed_seconds", elapsed_seconds}, {"termination_reason", termination_reason()},
            {"score", points}, {"winner", victor}};
}

GameResult run_game(Agent black, Agent white, int size, double komi, int max_moves) {
    if (max_moves < 1) throw std::invalid_argument("Move limit must be a positive integer");
    GameState state = GameState::new_game(size, komi);
    std::vector<Move> moves;
    auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < max_moves; ++index) {
        Move move = (state.to_play() == BLACK ? black : white)(state);
        state = state.play(move);
        moves.push_back(move);
        if (state.is_terminal()) break;
    }
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return {std::move(state), std::move(moves), max_moves, elapsed};
}

GameState replay_moves(const std::vector<Move>& moves, int size, double komi) {
    GameState state = GameState::new_game(size, komi);
    for (auto move : moves) state = state.play(move);
    return state;
}

std::vector<Move> moves_from_json(const Json& actions) {
    if (!actions.is_array()) throw std::invalid_argument("Moves must be an array");
    std::vector<Move> moves;
    for (const auto& action : actions) {
        if (action.is_null()) moves.push_back(PASS);
        else {
            if (!action.is_array() || action.size() != 2 ||
                !action[0].is_number_integer() || !action[1].is_number_integer())
                throw std::invalid_argument("Placement must contain two integer coordinates");
            auto row = action[0].get<std::int64_t>(), column = action[1].get<std::int64_t>();
            if (row < 0 || column < 0 || row > INT_MAX || column > INT_MAX)
                throw std::invalid_argument("Coordinates are out of bounds");
            moves.push_back(Point{static_cast<int>(row), static_cast<int>(column)});
        }
    }
    return moves;
}

Json load_records(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open " + path.string());
    Json data = Json::parse(stream);
    if (!data.is_object() || data.value("schema_version", 0) != 1)
        throw std::invalid_argument("Unsupported game record format");
    const auto& records = data.at("games");
    if (!records.is_array() || records.empty())
        throw std::invalid_argument("Game record must contain at least one game");
    for (const auto& record : records) {
        auto integer = [&](const char* key) {
            const auto& value = record.at(key);
            if (!value.is_number_integer()) throw std::invalid_argument("Expected an integer setting");
            auto number = value.get<std::int64_t>();
            if (number < 1 || number > INT_MAX) throw std::invalid_argument("Invalid recorded setting");
            return static_cast<int>(number);
        };
        int size = integer("size"), limit = integer("max_moves");
        double komi = record.at("komi").get<double>();
        auto moves = moves_from_json(record.at("moves"));
        auto state = replay_moves(moves, size, komi);
        if (moves.size() > static_cast<std::size_t>(limit) ||
            (!state.is_terminal() && moves.size() != static_cast<std::size_t>(limit)))
            throw std::invalid_argument("Invalid recorded move limit");
        double elapsed = record.at("elapsed_seconds").get<double>();
        if (!std::isfinite(elapsed) || elapsed < 0) throw std::invalid_argument("Invalid runtime");
        Json expected = GameResult{state, moves, limit, elapsed}.to_json();
        for (auto key : {"game_length", "termination_reason", "score", "winner"})
            if (record.at(key) != expected.at(key))
                throw std::invalid_argument(std::string("Recorded ") + key + " disagrees with replay");
    }
    return data;
}

std::string describe(const Json& record) {
    std::ostringstream out;
    out << record.at("game_length").get<int>() << " moves | ";
    if (record.at("termination_reason") == "move_limit") out << "truncated at move limit; no final score";
    else {
        const auto& winner = record.at("winner");
        out << (winner.is_null() ? "draw" : winner == BLACK ? "Black wins" : "White wins");
        out << ", Black " << record.at("score").at("black").get<double>()
            << " / White " << record.at("score").at("white").get<double>();
    }
    out << " | " << std::fixed << std::setprecision(3) << record.at("elapsed_seconds").get<double>() << 's';
    return out.str();
}

void save_records(const std::filesystem::path& path, const Json& data) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    if (!stream || !(stream << data.dump(2) << '\n')) throw std::runtime_error("Cannot write " + path.string());
}
} // namespace betago
