"""Small diagrams make expected rules behavior easy to check by hand."""

import unittest

from game import BLACK, EMPTY, PASS, WHITE, GameState, IllegalMove, Score


def position(*rows, to_play=BLACK, komi=0):
    symbols = {".": EMPTY, "X": BLACK, "O": WHITE}
    return GameState(tuple(tuple(symbols[c] for c in row) for row in rows),
                     to_play=to_play, komi=komi)


class BoardTests(unittest.TestCase):
    def test_new_board(self):
        state = GameState.new()
        self.assertEqual(state.size, 9)
        self.assertEqual(state.to_play, BLACK)
        self.assertEqual(sum(row.count(EMPTY) for row in state.board), 81)
        self.assertEqual(len(state.legal_moves()), 82)
        self.assertIn("To play: Black", str(state))

    def test_neighbors(self):
        state = GameState.new(3)
        self.assertEqual(set(state.neighbors((0, 0))), {(0, 1), (1, 0)})
        self.assertEqual(len(state.neighbors((0, 1))), 3)
        self.assertEqual(len(state.neighbors((1, 1))), 4)
        with self.assertRaises(ValueError):
            state.at((-1, 0))

    def test_groups_and_distinct_liberties(self):
        state = position("XX.", "X..", "..X")
        group, liberties = state.group_and_liberties((0, 0))
        self.assertEqual(group, {(0, 0), (0, 1), (1, 0)})
        self.assertEqual(liberties, {(0, 2), (1, 1), (2, 0)})
        self.assertEqual(state.group_and_liberties((2, 2))[0], {(2, 2)})
        with self.assertRaises(ValueError):
            state.group_and_liberties((1, 1))

    def test_diagonals_do_not_connect(self):
        state = position("X..", ".X.", "...")
        self.assertEqual(state.group_and_liberties((0, 0))[0], {(0, 0)})

    def test_validation_and_immutable_input(self):
        rows = [[EMPTY] * 3 for _ in range(3)]
        state = GameState(rows)
        rows[0][0] = BLACK
        self.assertEqual(state.at((0, 0)), EMPTY)
        for size in (0, -1, 1.5):
            with self.assertRaises(ValueError):
                GameState.new(size)
        for board in ((), ((0, 0),), ((3,),)):
            with self.assertRaises(ValueError):
                GameState(board)


class MoveTests(unittest.TestCase):
    def test_single_capture(self):
        state = position(".X.", "XO.", ".X.")
        result = state.play((1, 2))
        self.assertEqual(result.at((1, 1)), EMPTY)
        self.assertEqual(result.to_play, WHITE)
        self.assertEqual(state.at((1, 1)), WHITE)

    def test_multi_stone_capture(self):
        # White's connected edge group has exactly one liberty at (1, 2).
        state = position("XOO", ".X.", "...")
        result = state.play((1, 2))
        self.assertEqual(result.board[0], (BLACK, EMPTY, EMPTY))

    def test_multiple_groups_captured(self):
        state = position("XOX", "O.X", "XX.")
        result = state.play((1, 1))
        self.assertEqual(result.at((0, 1)), EMPTY)
        self.assertEqual(result.at((1, 0)), EMPTY)

    def test_suicide_and_capture_before_suicide(self):
        state = position(".O.", "O.O", ".O.")
        with self.assertRaises(IllegalMove):
            state.play((1, 1))
        self.assertNotIn((1, 1), state.legal_moves())
        capture = position("XOX", "O.O", "XOX").play((1, 1))
        self.assertEqual(capture.group_and_liberties((1, 1))[1],
                         {(0, 1), (1, 0), (1, 2), (2, 1)})

    def test_ko_and_recapture_after_intervening_moves(self):
        state = position(".XO..", "XO.O.", ".XO..", ".....", ".....")
        captured = state.play((1, 2))
        with self.assertRaisesRegex(IllegalMove, "ko"):
            captured.play((1, 1))
        self.assertNotIn((1, 1), captured.legal_moves())
        continued = captured.play((4, 4)).play((4, 0))
        recaptured = continued.play((1, 1))
        self.assertEqual(recaptured.at((1, 2)), EMPTY)

    def test_pass_updates_ko_history(self):
        state = position(".XO..", "XO.O.", ".XO..", ".....", ".....")
        continued = state.play((1, 2)).play(PASS).play((4, 0))
        self.assertEqual(continued.play((1, 1)).at((1, 2)), EMPTY)

    def test_illegal_actions_and_independent_successors(self):
        parent = GameState.new(3)
        first = parent.play((0, 0))
        sibling = parent.play((1, 1))
        for move in ((0, 0), (-1, 0), (3, 0), (0.5, 0), "bad"):
            with self.assertRaises(IllegalMove):
                first.play(move)
        self.assertEqual(parent.at((0, 0)), EMPTY)
        self.assertEqual(sibling.at((0, 0)), EMPTY)
        self.assertEqual(first.at((1, 1)), EMPTY)

    def test_passes_and_terminal_state(self):
        state = GameState.new(3)
        passed = state.play(PASS)
        self.assertEqual(passed.board, state.board)
        self.assertEqual(passed.to_play, WHITE)
        self.assertFalse(passed.is_terminal())
        self.assertEqual(passed.play((0, 0)).consecutive_passes, 0)
        ended = passed.play(PASS)
        self.assertTrue(ended.is_terminal())
        self.assertEqual(ended.legal_moves(), [])
        for move in (PASS, (0, 0)):
            with self.assertRaises(IllegalMove):
                ended.play(move)


class ScoringTests(unittest.TestCase):
    def test_empty_board_and_komi(self):
        self.assertEqual(GameState.new(3).score(), Score(0, 7.5))

    def test_exclusive_territory_and_neutral_region(self):
        state = position("XXX..", "X.X..", "XXX..", ".....", "....O")
        self.assertEqual(state.score(), Score(9, 1))
        white = position("OOO", "O.O", "OOO", komi=0.5)
        self.assertEqual(white.score(), Score(0, 9.5))

    def test_winner_requires_end_and_supports_draw(self):
        state = GameState.new(3, komi=0)
        with self.assertRaises(ValueError):
            state.winner()
        self.assertIsNone(state.play(PASS).play(PASS).winner())
        self.assertEqual(GameState.new(3).play(PASS).play(PASS).winner(), WHITE)
        black = position("XXX", "X.X", "XXX").play(PASS).play(PASS)
        self.assertEqual(black.winner(), BLACK)


if __name__ == "__main__":
    unittest.main()
