#pragma once

#include <compare>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace betago {
inline constexpr int EMPTY = 0, BLACK = 1, WHITE = 2;
struct Point {
    int row, column;
    auto operator<=>(const Point&) const = default;
};
using Move = std::optional<Point>;
inline constexpr std::nullopt_t PASS = std::nullopt;
using Board = std::vector<std::vector<int>>;

class IllegalMove : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class KoViolation : public IllegalMove {
public:
    using IllegalMove::IllegalMove;
};

struct Score {
    double black, white;
    std::optional<int> winner() const;
    bool operator==(const Score&) const = default;
};

struct Group {
    std::set<Point> stones, liberties;
};

// Callers can read a position, but every action returns a separate successor.
class GameState {
public:
    explicit GameState(Board board, int to_play = BLACK, double komi = 7.5,
                       int consecutive_passes = 0,
                       std::optional<Board> previous_board = std::nullopt);
    static GameState new_game(int size = 9, double komi = 7.5);
    int size() const { return static_cast<int>(board_.size()); }
    const Board& board() const { return board_; }
    int to_play() const { return to_play_; }
    double komi() const { return komi_; }
    int consecutive_passes() const { return consecutive_passes_; }
    const std::optional<Board>& previous_board() const { return previous_board_; }
    int at(Point point) const;
    std::vector<Point> neighbors(Point point) const;
    Group group_and_liberties(Point point) const;
    bool is_terminal() const { return consecutive_passes_ == 2; }
    GameState play(Move move) const;
    std::vector<Move> legal_moves() const;
    Score score() const;
    std::optional<int> winner() const;
    std::string to_string() const;
    bool operator==(const GameState&) const = default;

private:
    struct SuccessorTag {};
    // Only play can use this path: the source position is already validated,
    // and captures/placements preserve its shape and allowed cell values.
    GameState(Board board, int to_play, double komi, int consecutive_passes,
              std::optional<Board> previous_board, SuccessorTag);
    Board board_;
    int to_play_, consecutive_passes_;
    double komi_;
    std::optional<Board> previous_board_;
    static void validate_board(const Board& board);
    void check_point(Point point) const;
    std::set<Point> region(Point point) const;
};
} // namespace betago
