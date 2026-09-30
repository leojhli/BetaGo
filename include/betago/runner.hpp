#pragma once
#include "random.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace betago {
using Json = nlohmann::json;
using Agent = std::function<Move(const GameState&)>;
struct GameResult {
    GameState final_state;
    std::vector<Move> moves;
    int max_moves;
    double elapsed_seconds;
    std::string termination_reason() const;
    std::optional<Score> score() const;
    std::optional<int> winner() const;
    Json to_json() const;
};
GameResult run_game(Agent black, Agent white, int size = 9, double komi = 7.5, int max_moves = 500);
GameState replay_moves(const std::vector<Move>& moves, int size = 9, double komi = 7.5);
std::vector<Move> moves_from_json(const Json& moves);
Json load_records(const std::filesystem::path& path);
std::string describe(const Json& record);
void save_records(const std::filesystem::path& path, const Json& data);
} // namespace betago
