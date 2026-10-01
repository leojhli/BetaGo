#pragma once

#include "runner.hpp"
#include <string_view>

namespace betago {
// The winner is the reported game result, with EMPTY representing a genuine
// draw. It may differ from BetaGo's simplified area score for imported games.
struct ExpertGame {
    GameState initial_state = GameState::new_game();
    std::vector<Move> moves;
    int winner = EMPTY;
    std::string source;
    Json metadata = Json::object();
};

// Read the first variation of every game in an SGF collection. Board size must
// match exactly; imported moves are replayed under BetaGo's legal-move rules.
// Setup/handicap/turn changes and unknown outcomes are deliberately unsupported.
std::vector<ExpertGame> parse_sgf(std::string_view text, int required_size = 9,
                                  std::string source = {});
Json expert_game_json(const ExpertGame& game);
ExpertGame expert_game_from_json(const Json& record);
// Content identity excludes comments, source paths, and provenance metadata.
std::string expert_game_id(const ExpertGame& game);
// Export ordinary games that begin on an empty board with Black to move.
std::string expert_game_sgf(const ExpertGame& game);
} // namespace betago
