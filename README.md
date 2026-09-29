# BetaGo

A Python Go engine with a playable 9x9 board, capture and legality checks,
and area scoring.

The long-term goal is a small self-learning Go engine inspired by
[KataGo](https://github.com/lightvector/KataGo), using neural-guided search and
self-play in Python. Currently, the project provides the rules engine and a
local two-player interface, plus a seeded random agent and a batch game runner.

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

**Watch random game** starts a new game with the two seeded random agents.
Stones appear one move at a time. **Stop random game**, **Undo**, and **New game**
stop playback; after stopping, you can continue the position yourself.
Start random playback directly with:

```powershell
python play.py --random --seed 10
```

Use `--delay-ms 250` for faster playback or `--max-moves 100` to set a shorter
limit. The visual board remains 9x9. With the same seed and limit, it plays the
same moves as the first 9x9 game in the terminal runner. It displays the winner
after two passes, or a truncation message without a score at the move limit.
Visual playback does not save a JSON record.

The window uses Tkinter, included with the standard Windows Python installer.

## Run random games

These commands run batches and replay final boards in the terminal. For animated
playback in a window, use `python play.py --random --seed 10` above.
Start with small boards, then try 9x9:

```powershell
python runner.py --size 3 --games 5 --seed 10 --max-moves 100 --output results/tiny.json
python runner.py --size 9 --games 3 --seed 10 --max-moves 500 --output results/9x9.json
python runner.py --replay results/tiny.json
```

Both players pick uniformly from all legal actions, including pass. This is a
baseline for experiments, with no tactical judgment. The on-screen interface
can show random play, local two-player play, and the scripted capture demonstration.

Each JSON game records its board size, komi, move limit, both player seeds,
moves, game length (including passes), elapsed seconds, termination reason,
score, and winner. Placements are `[row, column]`; pass is `null`. Winner uses
`1` for Black, `2` for White, and `null` for a draw in a completed game.
Two consecutive passes produce `two_passes`; reaching the action limit first
produces `move_limit`. Truncated games have `null` score and winner. They are
unfinished, with no final outcome. A second pass on the last allowed action
still counts as a completed game.

The batch seed assigns Black `seed + 2 * game_index` and White the next integer,
with game indices starting at zero. Repeating the command reproduces moves and
outcomes with the same code and Python version; runtime varies. The output file
is overwritten on each run, so use distinct paths to keep experiments. Generated
`results/` files are ignored by Git. Replay checks legality and recorded outcomes
and prints each final board; it does not need randomness.

You can also use the agent and runner directly:

```python
from agents import RandomAgent
from runner import run_game, replay_moves

result = run_game(RandomAgent(10), RandomAgent(11), size=3, max_moves=100)
print(result.termination_reason, result.score)
assert replay_moves(result.moves, size=3) == result.final_state
```

An agent implements `choose_move(state)`. The runner selects the current
player's agent, applies its choice through `GameState.play`, and records the
action. Each random agent owns a `random.Random` generator, so players do not
share or alter global random state. Recreate the agents to restart their seeds.
Illegal agent moves raise `IllegalMove`; choosing from a terminal state raises
`ValueError`. Random games help check integration and state invariants; they do
not establish scoring correctness or playing strength.

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
7. [agents/random_agent.py](agents/random_agent.py) samples the legal action list.
8. [runner.py](runner.py) alternates agents, records games, and replays moves.
9. [tests/test_runner.py](tests/test_runner.py) checks reproducibility, legal play,
   state invariants, truncation, and saved records.

Boards are tuples of tuples. A move temporarily uses mutable rows, then freezes
the result. This keeps parent and sibling states independent for future MCTS.
Legal-move generation tries each empty point through `play`, favoring a single
clear definition of legality over speed. No search statistics live in GameState.
