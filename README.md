# BetaGo

A Python Go engine with a playable 9x9 board, capture and legality checks,
and area scoring.

The long-term goal is a small self-learning Go engine inspired by
[KataGo](https://github.com/lightvector/KataGo), using neural-guided search and
self-play in Python. Currently, the project provides the rules engine and a
local two-player interface.

## Play on screen

```powershell
python play.py
```

Click an intersection to play. Black and White alternate on the same computer.
Use **Pass**, **Undo**, and **New game** to explore positions. Two consecutive
passes end the game and display the score. A contrasting ring marks the last move.
The wooden board includes coordinates on all four sides and shaded stones.

**Watch capture demo** plays a short scripted sequence showing a surrounded
white stone being captured. You can stop it at any time and continue playing.
This is a demonstration, not a computer opponent.

The window uses Tkinter, included with the standard Windows Python installer.

## Run the tests

Python 3.10+ is required. No third-party packages are needed.

```powershell
python -m unittest discover -v
```

## Try the environment

Start `python` from the repository root, then enter:

```python
from game import GameState, PASS, BLACK, WHITE

state = GameState.new()          # Empty 9x9 board; Black to play
next_state = state.play((2, 3))  # Coordinates are (row, column), zero-based
print(next_state)               # X = Black, O = White, . = empty
print(state.at((2, 3)))          # Still empty: play creates a new state
print(next_state.group_and_liberties((2, 3)))
print(len(next_state.legal_moves()))

ended = next_state.play(PASS).play(PASS)
print(ended.score())
print(ended.winner())            # BLACK, WHITE, or None for a draw
```

That deliberately premature ending illustrates a scoring limitation: with only
Black on the board, all connected empty space borders Black exclusively and is
counted as Black's area. Realistic final scores require players to finish play.

## Rules contract

- Black starts; moves alternate. Neighbors are orthogonal, never diagonal.
- Captures remove entire opponent groups with no liberties. Captures are
  resolved before suicide is checked; suicide is forbidden.
- Simple ko forbids a placement that repeats the board before the previous
  action. Longer repetition cycles are not prevented.
- Passing is `PASS` (`None`). Two consecutive passes end the game; a placement
  resets the pass count. Terminal states allow no further actions.
- Area scoring counts stones plus empty regions bordered exclusively by one
  color. Regions bordered by both colors or neither color are neutral.
- White gets configurable komi (default 7.5). Prisoners add no separate points.
- Remaining stones count even if tactically dead: capture them before passing.
  There is no automatic dead-group adjudication or tournament-rule claim.

`play` raises `IllegalMove` for illegal actions and never modifies its input.
`score` can inspect any board; `winner` requires a terminated game. Smaller
boards can be created with `GameState.new(size=3, komi=0)` for experiments.
Direct construction with a board supports test diagrams; it checks the board's
shape and values, not whether the position could arise through legal play.

## Code layout

1. In [game/state.py](game/state.py), read `new`, `at`, and `neighbors`.
2. Trace `_region` and `group_and_liberties`: a stack explores connectivity and
   a set prevents counting the same liberty twice.
3. Read `play`: place, capture, check suicide, check ko, return a successor.
4. Read pass handling, `score`, and `winner`.
5. [tests/test_game.py](tests/test_game.py) covers rules using small board diagrams.
6. [play.py](play.py) draws the board and routes clicks through the rules engine.

Boards are tuples of tuples. A move temporarily uses mutable rows, then freezes
the result. This keeps parent and sibling states independent for future MCTS.
Legal-move generation tries each empty point through `play`, favoring a single
clear definition of legality over speed. No search statistics live in GameState.
