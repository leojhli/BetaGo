#pragma once

#include "gtp.hpp"
#include "sgf.hpp"
#include <functional>

namespace betago {
struct TeacherSettings {
    int board_size = 9;
    double komi = 7.5;
    int games = 10;
    int max_moves = 400;
    std::int64_t seed = 0;
    void validate() const;
    Json to_json() const;
};

// The initial position has move_number 0 and an empty Move. Later callbacks
// describe locally accepted positions, including accepted passes.
using TeacherProgress = std::function<void(int game_index, const GameState&, Move, int move_number)>;
// Called after every attempt, including failures. Observers can persist an
// atomic corpus snapshot; observer exceptions stop generation and propagate.
using TeacherSnapshot = std::function<void(const Json&)>;

// One fresh GTP session plays BOTH colors. Only legal games completed by two
// passes enter games[]; other attempts retain their legal prefixes and evidence.
// Generic GTP adapters are allowed, with unverified rules explicitly disclosed.
Json generate_teacher_games(const ExternalGtpConfiguration& configuration,
                            const TeacherSettings& settings,
                            const TeacherProgress& progress = {},
                            const TeacherSnapshot& snapshot = {});
} // namespace betago
