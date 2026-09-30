#include "betago/state.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace betago {
std::optional<int> Score::winner() const {
    if (black == white) return std::nullopt;
    return black > white ? BLACK : WHITE;
}

void GameState::validate_board(const Board& board) {
    if (board.empty()) throw std::invalid_argument("Board must be a nonempty square");
    for (const auto& row : board) {
        if (row.size() != board.size()) throw std::invalid_argument("Board must be a nonempty square");
        for (int cell : row)
            if (cell != EMPTY && cell != BLACK && cell != WHITE)
                throw std::invalid_argument("Unknown stone color");
    }
}

GameState::GameState(Board board, int to_play, double komi, int passes,
                     std::optional<Board> previous)
    : board_(std::move(board)), to_play_(to_play), consecutive_passes_(passes),
      komi_(komi), previous_board_(std::move(previous)) {
    validate_board(board_);
    if (previous_board_) {
        validate_board(*previous_board_);
        if (previous_board_->size() != board_.size())
            throw std::invalid_argument("Previous board must have the same size");
    }
    if (to_play != BLACK && to_play != WHITE) throw std::invalid_argument("Unknown player");
    if (passes < 0 || passes > 2) throw std::invalid_argument("Consecutive passes must be 0, 1, or 2");
    if (!std::isfinite(komi)) throw std::invalid_argument("Komi must be finite");
}

GameState GameState::new_game(int size, double komi) {
    if (size < 1) throw std::invalid_argument("Board size must be a positive integer");
    return GameState(Board(size, std::vector<int>(size, EMPTY)), BLACK, komi);
}

void GameState::check_point(Point p) const {
    if (p.row < 0 || p.column < 0 || p.row >= size() || p.column >= size())
        throw std::invalid_argument("Point must be an in-bounds (row, column) tuple");
}

int GameState::at(Point p) const { check_point(p); return board_[p.row][p.column]; }

std::vector<Point> GameState::neighbors(Point p) const {
    check_point(p);
    std::vector<Point> points;
    for (Point q : {Point{p.row - 1, p.column}, Point{p.row + 1, p.column},
                    Point{p.row, p.column - 1}, Point{p.row, p.column + 1}})
        if (q.row >= 0 && q.column >= 0 && q.row < size() && q.column < size()) points.push_back(q);
    return points;
}

std::set<Point> GameState::region(Point p) const {
    int color = at(p);
    std::set<Point> visited{p};
    std::vector<Point> pending{p};
    while (!pending.empty()) {
        Point current = pending.back();
        pending.pop_back();
        for (Point q : neighbors(current))
            if (at(q) == color && visited.insert(q).second) pending.push_back(q);
    }
    return visited;
}

Group GameState::group_and_liberties(Point p) const {
    if (at(p) == EMPTY) throw std::invalid_argument("A group must start at a stone");
    Group group{region(p), {}};
    for (Point stone : group.stones)
        for (Point q : neighbors(stone))
            if (at(q) == EMPTY) group.liberties.insert(q);
    return group;
}

GameState GameState::play(Move move) const {
    if (is_terminal()) throw IllegalMove("The game has ended");
    int opponent = to_play_ == BLACK ? WHITE : BLACK;
    if (!move) return GameState(board_, opponent, komi_, consecutive_passes_ + 1, board_);
    try {
        if (at(*move) != EMPTY) throw IllegalMove("The intersection is occupied");
    } catch (const std::invalid_argument& error) { throw IllegalMove(error.what()); }

    GameState result = *this;
    result.board_[move->row][move->column] = to_play_;
    std::set<Point> captured;
    for (Point q : neighbors(*move)) {
        if (result.at(q) == opponent) {
            Group group = result.group_and_liberties(q);
            if (group.liberties.empty()) captured.insert(group.stones.begin(), group.stones.end());
        }
    }
    for (Point p : captured) result.board_[p.row][p.column] = EMPTY;
    if (result.group_and_liberties(*move).liberties.empty()) throw IllegalMove("Suicide is forbidden");
    if (previous_board_ && result.board_ == *previous_board_)
        throw KoViolation("Simple ko forbids immediate board repetition");
    result.to_play_ = opponent;
    result.consecutive_passes_ = 0;
    result.previous_board_ = board_;
    return result;
}

std::vector<Move> GameState::legal_moves() const {
    std::vector<Move> moves;
    if (is_terminal()) return moves;
    for (int r = 0; r < size(); ++r) for (int c = 0; c < size(); ++c) {
        Point point{r, c};
        if (at(point) == EMPTY) {
            try { play(point); moves.push_back(point); }
            catch (const IllegalMove&) {}
        }
    }
    moves.push_back(PASS);
    return moves;
}

Score GameState::score() const {
    double totals[3] = {};
    std::set<Point> visited;
    for (int r = 0; r < size(); ++r) for (int c = 0; c < size(); ++c) {
        Point point{r, c};
        int color = at(point);
        if (color != EMPTY) ++totals[color];
        else if (!visited.contains(point)) {
            auto empty_region = region(point);
            visited.insert(empty_region.begin(), empty_region.end());
            std::set<int> borders;
            for (Point p : empty_region) for (Point q : neighbors(p))
                if (at(q) != EMPTY) borders.insert(at(q));
            if (borders.size() == 1) totals[*borders.begin()] += empty_region.size();
        }
    }
    return {totals[BLACK], totals[WHITE] + komi_};
}

std::optional<int> GameState::winner() const {
    if (!is_terminal()) throw std::invalid_argument("Winner is only defined after the game ends");
    return score().winner();
}

std::string GameState::to_string() const {
    std::ostringstream out;
    out << "   ";
    for (int c = 0; c < size(); ++c) out << (c ? " " : "") << c;
    for (int r = 0; r < size(); ++r) {
        out << '\n' << std::setw(2) << r << ' ';
        for (int c = 0; c < size(); ++c) out << (c ? " " : "") << ".XO"[board_[r][c]];
    }
    out << "\nTo play: " << (to_play_ == BLACK ? "Black" : "White");
    return out.str();
}
} // namespace betago
