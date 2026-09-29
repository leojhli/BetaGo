"""Simple Go rules using immutable boards and flood fills.

Coordinates are (row, column), starting at zero. This educational variant uses
simple ko and scores stones left on the board; it does not adjudicate dead groups.
"""

from dataclasses import dataclass, replace
from math import isfinite

EMPTY, BLACK, WHITE = 0, 1, 2
PASS = None
Point = tuple[int, int]
Move = Point | None
Board = tuple[tuple[int, ...], ...]


class IllegalMove(ValueError):
    """The requested action is not legal in this state."""


@dataclass(frozen=True)
class Score:
    black: float
    white: float

    @property
    def winner(self) -> int | None:
        """Return BLACK, WHITE, or None for a draw."""
        if self.black == self.white:
            return None
        return BLACK if self.black > self.white else WHITE


@dataclass(frozen=True)
class GameState:
    board: Board
    to_play: int = BLACK
    consecutive_passes: int = 0
    komi: float = 7.5
    previous_board: Board | None = None

    def __post_init__(self) -> None:
        # Normalize input so even callers supplying lists cannot mutate a state.
        board = tuple(tuple(row) for row in self.board)
        self._validate_board(board)
        object.__setattr__(self, "board", board)
        if self.previous_board is not None:
            previous = tuple(tuple(row) for row in self.previous_board)
            self._validate_board(previous)
            if len(previous) != len(board):
                raise ValueError("Previous board must have the same size")
            object.__setattr__(self, "previous_board", previous)
        if self.to_play not in (BLACK, WHITE):
            raise ValueError("Player must be BLACK or WHITE")
        if type(self.consecutive_passes) is not int or not 0 <= self.consecutive_passes <= 2:
            raise ValueError("Consecutive passes must be 0, 1, or 2")
        if not isfinite(self.komi):
            raise ValueError("Komi must be finite")

    @staticmethod
    def _validate_board(board: Board) -> None:
        if not board or any(len(row) != len(board) for row in board):
            raise ValueError("Board must be a nonempty square")
        if any(cell not in (EMPTY, BLACK, WHITE) for row in board for cell in row):
            raise ValueError("Unknown stone color")

    @classmethod
    def new(cls, size: int = 9, komi: float = 7.5) -> "GameState":
        if type(size) is not int or size < 1:
            raise ValueError("Board size must be a positive integer")
        return cls(tuple((EMPTY,) * size for _ in range(size)), komi=komi)

    @property
    def size(self) -> int:
        return len(self.board)

    def _check_point(self, point: Point) -> None:
        if (
            not isinstance(point, tuple)
            or len(point) != 2
            or any(type(coordinate) is not int for coordinate in point)
            or not all(0 <= coordinate < self.size for coordinate in point)
        ):
            raise ValueError("Point must be an in-bounds (row, column) tuple")

    def at(self, point: Point) -> int:
        self._check_point(point)
        row, column = point
        return self.board[row][column]

    def neighbors(self, point: Point) -> tuple[Point, ...]:
        self._check_point(point)
        row, column = point
        return tuple(
            (r, c)
            for r, c in ((row - 1, column), (row + 1, column),
                         (row, column - 1), (row, column + 1))
            if 0 <= r < self.size and 0 <= c < self.size
        )

    def _region(self, point: Point) -> set[Point]:
        """Flood fill a connected region of one color (including EMPTY)."""
        color = self.at(point)
        visited = {point}
        pending = [point]
        while pending:
            for neighbor in self.neighbors(pending.pop()):
                if neighbor not in visited and self.at(neighbor) == color:
                    visited.add(neighbor)
                    pending.append(neighbor)
        return visited

    def group_and_liberties(self, point: Point) -> tuple[set[Point], set[Point]]:
        if self.at(point) == EMPTY:
            raise ValueError("A group must start at a stone")
        group = self._region(point)
        liberties = {
            neighbor
            for stone in group
            for neighbor in self.neighbors(stone)
            if self.at(neighbor) == EMPTY
        }
        return group, liberties

    def is_terminal(self) -> bool:
        return self.consecutive_passes == 2

    def play(self, move: Move) -> "GameState":
        if self.is_terminal():
            raise IllegalMove("The game has ended")
        opponent = WHITE if self.to_play == BLACK else BLACK
        if move is PASS:
            return replace(self, to_play=opponent,
                           consecutive_passes=self.consecutive_passes + 1,
                           previous_board=self.board)
        try:
            occupied = self.at(move) != EMPTY
        except ValueError as error:
            raise IllegalMove(str(error)) from error
        if occupied:
            raise IllegalMove("The intersection is occupied")

        rows = [list(row) for row in self.board]
        row, column = move
        rows[row][column] = self.to_play
        placed = replace(self, board=rows)
        captured: set[Point] = set()
        for neighbor in self.neighbors(move):
            if placed.at(neighbor) == opponent:
                group, liberties = placed.group_and_liberties(neighbor)
                if not liberties:
                    captured.update(group)
        for r, c in captured:
            rows[r][c] = EMPTY

        result = replace(self, board=rows, to_play=opponent,
                         consecutive_passes=0, previous_board=self.board)
        if not result.group_and_liberties(move)[1]:
            raise IllegalMove("Suicide is forbidden")
        if result.board == self.previous_board:
            raise IllegalMove("Simple ko forbids immediate board repetition")
        return result

    def legal_moves(self) -> list[Move]:
        if self.is_terminal():
            return []
        moves: list[Move] = []
        for row in range(self.size):
            for column in range(self.size):
                point = (row, column)
                if self.at(point) == EMPTY:
                    try:
                        self.play(point)
                    except IllegalMove:
                        continue
                    moves.append(point)
        moves.append(PASS)
        return moves

    def score(self) -> Score:
        """Count current area; meaningful as an outcome only after termination.

        Uncaptured dead stones still count. Empty regions bordering both players
        (or neither player) are neutral. Prisoners add no separate points.
        """
        totals = {BLACK: 0, WHITE: 0}
        visited: set[Point] = set()
        for row in range(self.size):
            for column in range(self.size):
                point = (row, column)
                color = self.at(point)
                if color != EMPTY:
                    totals[color] += 1
                elif point not in visited:
                    region = self._region(point)
                    visited.update(region)
                    borders = {
                        self.at(neighbor)
                        for empty in region
                        for neighbor in self.neighbors(empty)
                        if self.at(neighbor) != EMPTY
                    }
                    if len(borders) == 1:
                        totals[borders.pop()] += len(region)
        return Score(totals[BLACK], totals[WHITE] + self.komi)

    def winner(self) -> int | None:
        if not self.is_terminal():
            raise ValueError("Winner is only defined after the game ends")
        return self.score().winner

    def __str__(self) -> str:
        symbols = {EMPTY: ".", BLACK: "X", WHITE: "O"}
        lines = ["   " + " ".join(str(c) for c in range(self.size))]
        lines.extend(f"{r:2} " + " ".join(symbols[cell] for cell in row)
                     for r, row in enumerate(self.board))
        lines.append("To play: " + ("Black" if self.to_play == BLACK else "White"))
        return "\n".join(lines)
