"""Seeded games connect agents, rules, records, and replay."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import random
import tempfile
import unittest
from unittest.mock import patch

from agents import RandomAgent
from game import BLACK, EMPTY, PASS, WHITE, GameState, IllegalMove, Score
from runner import load_records, main, replay_moves, run_game


class ScriptedAgent:
    """A fixed sequence makes runner boundary cases independent of randomness."""

    def __init__(self, *moves):
        self.moves = iter(moves)
        self.players = []

    def choose_move(self, state):
        self.players.append(state.to_play)
        return next(self.moves)


class RandomAgentTests(unittest.TestCase):
    def test_choice_includes_every_legal_action_and_pass(self):
        state = GameState.new(3).play((0, 0))
        agent = RandomAgent(123)
        with patch.object(agent.rng, "choice", return_value=PASS) as choose:
            self.assertIs(agent.choose_move(state), PASS)
        choose.assert_called_once_with(state.legal_moves())
        actions = choose.call_args.args[0]
        self.assertIn(PASS, actions)
        self.assertNotIn((0, 0), actions)
        self.assertEqual(len(actions), len(set(actions)))

    def test_seeded_choices_are_independent_of_global_randomness(self):
        global_state = random.getstate()
        state = GameState.new(3)
        first, second = RandomAgent(42), RandomAgent(42)
        self.assertEqual([first.choose_move(state) for _ in range(20)],
                         [second.choose_move(state) for _ in range(20)])
        self.assertEqual(random.getstate(), global_state)

    def test_only_pass_is_legal(self):
        state = GameState(((BLACK,),), komi=0)
        self.assertIs(RandomAgent(5).choose_move(state), PASS)

    def test_terminal_state_and_invalid_seed(self):
        state = GameState.new(3).play(PASS).play(PASS)
        with self.assertRaisesRegex(ValueError, "game ends"):
            RandomAgent().choose_move(state)
        with self.assertRaises(ValueError):
            RandomAgent("seed")


class RunnerTests(unittest.TestCase):
    def test_completion_and_color_dispatch(self):
        black = ScriptedAgent((0, 0), PASS)
        white = ScriptedAgent((2, 2), PASS)
        result = run_game(black, white, size=3, komi=0, max_moves=10)
        self.assertEqual(result.moves, ((0, 0), (2, 2), PASS, PASS))
        self.assertEqual(black.players, [BLACK, BLACK])
        self.assertEqual(white.players, [WHITE, WHITE])
        self.assertEqual(result.termination_reason, "two_passes")
        self.assertEqual(result.score, Score(1, 1))
        self.assertIsNone(result.winner)
        self.assertGreaterEqual(result.elapsed_seconds, 0)

    def test_truncation_has_no_outcome_or_forced_passes(self):
        result = run_game(ScriptedAgent((0, 0)), ScriptedAgent(), size=3, max_moves=1)
        self.assertEqual(result.moves, ((0, 0),))
        self.assertFalse(result.final_state.is_terminal())
        self.assertEqual(result.termination_reason, "move_limit")
        self.assertIsNone(result.score)
        self.assertIsNone(result.winner)
        self.assertIsNone(result.to_dict()["score"])
        self.assertIsNone(result.to_dict()["winner"])

    def test_two_passes_on_limit_is_completed(self):
        result = run_game(ScriptedAgent(PASS), ScriptedAgent(PASS), max_moves=2)
        self.assertEqual(result.termination_reason, "two_passes")
        self.assertEqual(result.score, Score(0, 7.5))
        self.assertEqual(result.winner, WHITE)

    def test_illegal_agent_move_is_not_replaced(self):
        with self.assertRaises(IllegalMove):
            run_game(ScriptedAgent((0, 0)), ScriptedAgent((0, 0)), max_moves=2)

    def test_invalid_settings(self):
        for limit in (0, -1, 1.5, True):
            with self.subTest(limit=limit), self.assertRaises(ValueError):
                run_game(RandomAgent(), RandomAgent(), max_moves=limit)
        with self.assertRaises(ValueError):
            run_game(RandomAgent(), RandomAgent(), size=0)

    def test_seeded_games_replay_legally_with_state_invariants(self):
        # Tiny batches exercise captures and passes; 9x9 checks the default size.
        for size, seed in ((2, 0), (2, 2), (3, 4), (3, 6), (9, 8)):
            with self.subTest(size=size, seed=seed):
                result = run_game(RandomAgent(seed), RandomAgent(seed + 1),
                                  size=size, max_moves=60)
                repeated = run_game(RandomAgent(seed), RandomAgent(seed + 1),
                                    size=size, max_moves=60)
                self.assertEqual(result.moves, repeated.moves)
                self.assertEqual(result.final_state, repeated.final_state)
                self.assertEqual(result.termination_reason, repeated.termination_reason)
                self.assertEqual(result.score, repeated.score)
                self.assertEqual(result.winner, repeated.winner)
                self.assertEqual(replay_moves(result.moves, size=size), result.final_state)
                state = GameState.new(size)
                for index, move in enumerate(result.moves):
                    board_before = state.board
                    self.assertEqual(state.to_play, BLACK if index % 2 == 0 else WHITE)
                    self.assertIn(move, state.legal_moves())
                    successor = state.play(move)
                    self.assertEqual(state.board, board_before)
                    self.assertEqual(successor.previous_board, board_before)
                    self.assertEqual(successor.to_play, WHITE if state.to_play == BLACK else BLACK)
                    passes = state.consecutive_passes + 1 if move is PASS else 0
                    self.assertEqual(successor.consecutive_passes, passes)
                    self.assertEqual(len(successor.board), size)
                    for row in range(size):
                        self.assertEqual(len(successor.board[row]), size)
                        for column in range(size):
                            color = successor.at((row, column))
                            self.assertIn(color, (EMPTY, BLACK, WHITE))
                            if color != EMPTY:
                                self.assertTrue(successor.group_and_liberties((row, column))[1])
                    state = successor
                if result.termination_reason == "move_limit":
                    self.assertEqual(len(result.moves), 60)

    def test_replay_rejects_illegal_moves_and_moves_after_end(self):
        for moves in (((0, 0), (0, 0)), (PASS, PASS, (0, 0))):
            with self.subTest(moves=moves), self.assertRaises(IllegalMove):
                replay_moves(moves)


class RecordTests(unittest.TestCase):
    def test_cli_saves_seeds_and_replayable_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "batch.json"
            with redirect_stdout(io.StringIO()):
                main(["--size", "2", "--games", "3", "--seed", "10",
                      "--max-moves", "40", "--output", str(path)])
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["seed"], 10)
            records = load_records(path)
            self.assertEqual(len(records), 3)
            for index, record in enumerate(records):
                self.assertEqual(record["black_seed"], 10 + 2 * index)
                self.assertEqual(record["white_seed"], 11 + 2 * index)
                repeated = run_game(RandomAgent(record["black_seed"]),
                                    RandomAgent(record["white_seed"]), size=record["size"],
                                    komi=record["komi"], max_moves=record["max_moves"])
                expected = repeated.to_dict()
                for key in ("moves", "score", "winner", "termination_reason", "game_length"):
                    self.assertEqual(record[key], expected[key])
            output = io.StringIO()
            with redirect_stdout(output):
                main(["--replay", str(path)])
            self.assertIn("Game 3:", output.getvalue())
            self.assertIn("To play:", output.getvalue())

    def test_saved_outcome_is_checked_against_replay(self):
        result = run_game(ScriptedAgent(PASS), ScriptedAgent(PASS))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.json"
            for key, bad_value in (("score", None), ("winner", BLACK),
                                   ("termination_reason", "move_limit"), ("game_length", 3)):
                with self.subTest(key=key):
                    record = result.to_dict()
                    record[key] = bad_value
                    path.write_text(json.dumps({"schema_version": 1, "games": [record]}),
                                    encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "disagrees with replay"):
                        load_records(path)


if __name__ == "__main__":
    unittest.main()
