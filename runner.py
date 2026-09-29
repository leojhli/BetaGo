"""Run random games, save JSON records, or replay their moves.

Examples:
    python runner.py --size 3 --games 5 --seed 10
    python runner.py --replay results/random_games.json
"""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
from time import perf_counter
from typing import Iterable, Protocol

from agents import RandomAgent
from game import BLACK, WHITE, GameState, Score
from game.state import Move


class Agent(Protocol):
    def choose_move(self, state: GameState) -> Move:
        """Return an action for the player to move."""
        ...


@dataclass(frozen=True)
class GameResult:
    final_state: GameState
    moves: tuple[Move, ...]
    max_moves: int
    elapsed_seconds: float

    @property
    def termination_reason(self) -> str:
        return "two_passes" if self.final_state.is_terminal() else "move_limit"

    @property
    def score(self) -> Score | None:
        return self.final_state.score() if self.final_state.is_terminal() else None

    @property
    def winner(self) -> int | None:
        return self.final_state.winner() if self.final_state.is_terminal() else None

    def to_dict(self) -> dict:
        """JSON uses [row, column] for placements and null for pass.

        A null winner means a draw only when termination_reason is two_passes.
        Truncated games have both score and winner set to null.
        """
        score = self.score
        return {
            "size": self.final_state.size,
            "komi": self.final_state.komi,
            "max_moves": self.max_moves,
            "moves": [list(move) if move is not None else None for move in self.moves],
            "game_length": len(self.moves),
            "elapsed_seconds": self.elapsed_seconds,
            "termination_reason": self.termination_reason,
            "score": None if score is None else {"black": score.black, "white": score.white},
            "winner": self.winner,
        }


def run_game(black: Agent, white: Agent, *, size: int = 9,
             komi: float = 7.5, max_moves: int = 500) -> GameResult:
    """Start an empty board and stop at two passes or the action limit.

    Passes count toward the limit. Illegal agent actions raise IllegalMove;
    the runner never replaces them or forces passes to manufacture a result.
    """
    if type(max_moves) is not int or max_moves < 1:
        raise ValueError("Move limit must be a positive integer")
    state = GameState.new(size=size, komi=komi)
    agents = {BLACK: black, WHITE: white}
    moves: list[Move] = []
    started = perf_counter()
    for _ in range(max_moves):
        move = agents[state.to_play].choose_move(state)
        state = state.play(move)
        moves.append(move)
        if state.is_terminal():
            break
    return GameResult(state, tuple(moves), max_moves, perf_counter() - started)


def replay_moves(moves: Iterable[Move], *, size: int = 9,
                 komi: float = 7.5) -> GameState:
    """Rebuild a position using the same rules, including pass and ko history."""
    state = GameState.new(size=size, komi=komi)
    for move in moves:
        state = state.play(move)
    return state


def load_records(path: Path) -> list[dict]:
    """Read our JSON format and check the recorded outcomes by replaying."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("Unsupported game record format")
    records = data.get("games")
    if not isinstance(records, list) or not records:
        raise ValueError("Game record must contain at least one game")
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("Each game record must be an object")
        try:
            moves = tuple(tuple(move) if move is not None else None
                          for move in record["moves"])
            state = replay_moves(moves, size=record["size"], komi=record["komi"])
            max_moves = record["max_moves"]
            if type(max_moves) is not int or max_moves < 1 or len(moves) > max_moves:
                raise ValueError("Invalid recorded move limit")
            if not state.is_terminal() and len(moves) != max_moves:
                raise ValueError("Unfinished game stopped before its move limit")
            result = GameResult(state, moves, max_moves, record["elapsed_seconds"])
            expected = result.to_dict()
            for key in ("game_length", "termination_reason", "score", "winner"):
                if record[key] != expected[key]:
                    raise ValueError(f"Recorded {key} disagrees with replay")
        except (KeyError, TypeError) as error:
            raise ValueError("Malformed game record") from error
    return records


def describe(record: dict) -> str:
    if record["termination_reason"] == "move_limit":
        outcome = "truncated at move limit; no final score"
    else:
        winner = record["winner"]
        name = "draw" if winner is None else "Black wins" if winner == BLACK else "White wins"
        score = record["score"]
        outcome = f"{name}, Black {score['black']:g} / White {score['white']:g}"
    return (f"{record['game_length']} moves | {outcome} | "
            f"{record['elapsed_seconds']:.3f}s")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=9)
    parser.add_argument("--komi", type=float, default=7.5)
    parser.add_argument("--games", type=int, default=1)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--max-moves", type=int, default=500)
    parser.add_argument("--output", type=Path, default=Path("results/random_games.json"))
    parser.add_argument("--replay", type=Path, help="Replay a saved batch instead of running games")
    args = parser.parse_args(argv)
    try:
        if args.replay is not None:
            records = load_records(args.replay)
            for index, record in enumerate(records, 1):
                print(f"Game {index}: {describe(record)}")
                moves = (tuple(move) if move is not None else None for move in record["moves"])
                print(replay_moves(moves, size=record["size"], komi=record["komi"]))
            return
        if args.games < 1:
            raise ValueError("Number of games must be positive")
        records = []
        for index in range(args.games):
            # Adjacent seeds are deterministic and distinct for every player/game.
            black_seed = args.seed + 2 * index
            white_seed = black_seed + 1
            result = run_game(RandomAgent(black_seed), RandomAgent(white_seed),
                              size=args.size, komi=args.komi, max_moves=args.max_moves)
            record = result.to_dict()
            record.update(black_seed=black_seed, white_seed=white_seed)
            records.append(record)
            print(f"Game {index + 1}: {describe(record)}")
        data = {
            "schema_version": 1,
            "agents": {"black": "random", "white": "random"},
            "rules": "simple ko, no suicide, area scoring, no dead-group adjudication",
            "seed": args.seed,
            "games": records,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        completed = sum(record["termination_reason"] == "two_passes" for record in records)
        print(f"Completed: {completed}; truncated: {len(records) - completed}")
        print(f"Saved {args.output}")
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
