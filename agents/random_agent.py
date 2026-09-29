"""A reproducible baseline that samples all legal actions uniformly."""

import random

from game.state import GameState, Move


class RandomAgent:
    def __init__(self, seed: int = 0):
        if type(seed) is not int:
            raise ValueError("Seed must be an integer")
        self.seed = seed
        # An independent generator avoids changing Python's global randomness.
        self.rng = random.Random(seed)

    def choose_move(self, state: GameState) -> Move:
        moves = state.legal_moves()
        if not moves:
            raise ValueError("Cannot choose a move after the game ends")
        # PASS has the same probability as each legal board intersection.
        return self.rng.choice(moves)
