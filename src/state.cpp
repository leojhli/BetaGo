#include "betago/state.hpp"
#include "betago/profile.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace betago {
namespace {
// Internal move checks need only to find a liberty, or collect a group that
// has none. They do not need the sorted sets returned by the public Group API.
struct LibertyScratch {
    std::vector<unsigned char> marks;
    std::vector<Point> stones;
    unsigned char generation = 0;
};

template<class Function>
bool any_neighbor(Point point, int size, Function function) {
    return (point.row > 0 && function(Point{point.row - 1, point.column}))
        || (point.row + 1 < size && function(Point{point.row + 1, point.column}))
        || (point.column > 0 && function(Point{point.row, point.column - 1}))
        || (point.column + 1 < size && function(Point{point.row, point.column + 1}));
}

bool has_liberty(const Board& board, Point start, LibertyScratch& scratch) {
    ProfileScope scope(ProfileWork::Group);
    const int size = static_cast<int>(board.size());
    scratch.stones.clear();
    // Most candidate stones/groups already have an adjacent liberty. Avoid
    // allocating traversal buffers when that direct check settles the result.
    if (any_neighbor(start, size, [&](Point point) {
        return board[point.row][point.column] == EMPTY;
    })) return true;

    if (scratch.marks.empty()) {
        scratch.marks.resize(static_cast<std::size_t>(size) * size, 0);
        scratch.stones.reserve(static_cast<std::size_t>(size));
    }
    // One placement checks at most four adjacent opponent groups and its own
    // group, so the byte generation cannot overflow within this scratch object.
    const unsigned char generation = ++scratch.generation;
    const int color = board[start.row][start.column];
    auto index = [size](Point point) {
        return static_cast<std::size_t>(point.row) * size + point.column;
    };
    scratch.marks[index(start)] = generation;
    scratch.stones.push_back(start);
    for (std::size_t cursor = 0; cursor < scratch.stones.size(); ++cursor) {
        const Point current = scratch.stones[cursor];
        if (any_neighbor(current, size, [&](Point point) {
            const int neighbor_color = board[point.row][point.column];
            if (neighbor_color == EMPTY) return true;
            const auto at = index(point);
            if (neighbor_color == color && scratch.marks[at] != generation) {
                scratch.marks[at] = generation;
                scratch.stones.push_back(point);
            }
            return false;
        })) return true;
    }
    return false;
}
} // namespace

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

GameState::GameState(Board board, int to_play, double komi, int passes,
                     std::optional<Board> previous, SuccessorTag)
    : board_(std::move(board)), to_play_(to_play), consecutive_passes_(passes),
      komi_(komi), previous_board_(std::move(previous)) {}

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
    ProfileScope scope(ProfileWork::Group);
    if (at(p) == EMPTY) throw std::invalid_argument("A group must start at a stone");
    Group group{region(p), {}};
    for (Point stone : group.stones)
        for (Point q : neighbors(stone))
            if (at(q) == EMPTY) group.liberties.insert(q);
    return group;
}

GameState GameState::play(Move move) const {
    ProfileScope scope(ProfileWork::StatePlay);
    if (is_terminal()) throw IllegalMove("The game has ended");
    int opponent = to_play_ == BLACK ? WHITE : BLACK;
    if (!move) return GameState(board_, opponent, komi_, consecutive_passes_ + 1, board_);
    try {
        if (at(*move) != EMPTY) throw IllegalMove("The intersection is occupied");
    } catch (const std::invalid_argument& error) { throw IllegalMove(error.what()); }

    // Copy only the current board and its required one-ply history. Cloning the
    // source's older history would allocate a board that is immediately replaced.
    // This narrow profile scope excludes pass and ordinary caller construction.
    GameState result = [&] {
        ProfileScope copy_scope(ProfileWork::StateCopy);
        return GameState(board_, opponent, komi_, 0, board_, SuccessorTag{});
    }();
    result.board_[move->row][move->column] = to_play_;
    LibertyScratch scratch;
    std::vector<Point> captured;
    any_neighbor(*move, size(), [&](Point point) {
        if (result.board_[point.row][point.column] == opponent
            && std::find(captured.begin(), captured.end(), point) == captured.end()
            && !has_liberty(result.board_, point, scratch)) {
            captured.insert(captured.end(), scratch.stones.begin(), scratch.stones.end());
        }
        // Inspect every adjacent group before removing any captured stones.
        return false;
    });
    for (Point p : captured) result.board_[p.row][p.column] = EMPTY;
    if (!has_liberty(result.board_, *move, scratch)) throw IllegalMove("Suicide is forbidden");
    if (previous_board_ && result.board_ == *previous_board_)
        throw KoViolation("Simple ko forbids immediate board repetition");
    return result;
}

std::vector<Move> GameState::legal_moves() const {
    ProfileScope scope(ProfileWork::LegalMoves);
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
