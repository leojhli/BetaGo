# BetaGo

A C++20 Go engine with the same playable wooden 9x9 board, capture and legality
checks, area scoring, seeded random agents, and classical Monte Carlo tree
search (MCTS). The long-term goal is a small self-learning Go engine inspired by
[KataGo](https://github.com/lightvector/KataGo).
Currently it provides the rules environment, random and MCTS play, game
recording, and search statistics.

## Build

On this Windows machine, run from the repository root:

```powershell
.\build.ps1
```

The script builds optimized `build/play.exe`, `build/runner.exe`, and
`build/tests.exe`. On its first run it downloads a pinned portable C++ compiler
into `.tools/` and verifies the archive's checksum. Nothing is installed globally.
Later builds reuse the compiler and unchanged object files. Use `-DebugBuild`
for an unoptimized build with debug information.

The visual board uses the same Tk 8.6 toolkit as the original interface, directly
from C++. The build copies the Tk libraries from the local `.tools/tk/` cache
into `build/runtime/`. On another machine, supply a 64-bit Tk 8.6 root:

```powershell
.\build.ps1 -TkRoot 'C:\path\to\tcltk'
```

A standalone Tk installation uses `bin/tcl86t.dll`, `bin/tk86t.dll`, and
`lib/tcl8.6` / `lib/tk8.6`. Once bundled, the runtime is independent of that
source installation. Keep `runtime/` beside `play.exe`.
Copy the complete `build/` folder, including `licenses/`, when sharing the app.
The copied Tk DLLs also use Microsoft's Visual C++ runtime, which is already
installed on this machine; another Windows machine may need that runtime.

A conventional CMake build is also provided for a complete C++20 toolchain:

```powershell
cmake -S . -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out --config Release
ctest --test-dir out -C Release --output-on-failure
```

For a Windows CMake build, copy `build/runtime/` beside the generated `play.exe`
before running the visual board or its tests. The rules library, runner, and
rules tests are portable C++; the current GUI loader and helper build script
target Windows.

## Play on screen

```powershell
.\build\play.exe
.\build\play.exe --random --seed 10
.\build\play.exe --mcts --seed 10
.\build\play.exe --watch-mcts --seed 10
```

Click an intersection to play. Black and White alternate on the same computer.
Use **Pass**, **Undo**, and **New game** to explore positions. Two consecutive
passes end the game and display the score. The wooden board keeps coordinates
on all four sides, shaded stones, and a contrasting ring around the last move.

**Watch capture demo** plays the original short scripted capture sequence.
**Watch random game** starts a game between two seeded random agents, one move
at a time. **Play vs MCTS** starts a game with human Black and MCTS White.
**Watch MCTS vs random** starts MCTS Black against random White. The corresponding
startup options are `--mcts` and `--watch-mcts`. MCTS runs one simulation per Tk
callback so the board can process controls while thinking. The status shows
search progress and rollout truncations.

Stop playback to continue the position yourself. Undo, New game, and stopping
also cancel an unfinished search and return to local two-player interaction.

Use `--delay-ms 250` for a shorter pause between watched moves or
`--max-moves 100` for a shorter watched game. Human games have no move limit.
Search options are `--simulations 128`, `--rollout-limit 200`, and
`--exploration 1.4142135623730951`; these values are the defaults. More
simulations require more thinking time. The visual board remains 9x9 with komi
7.5. A second pass displays the winner; the move limit displays truncation
without a final score. Visual playback does not save JSON records.

## Run and replay batches

These commands run games and replay final boards in the terminal:

```powershell
.\build\runner.exe --size 3 --games 5 --seed 10 --max-moves 100 --output results/tiny.json
.\build\runner.exe --size 9 --games 3 --seed 10 --max-moves 500 --output results/9x9.json
.\build\runner.exe --replay results/9x9.json
.\build\runner.exe --size 3 --black mcts --white random --simulations 128 --seed 10 --output results/mcts_games.json
```

Both players default to `random`, choosing uniformly from all legal actions,
including pass. Select `--black mcts` or `--white mcts` to use search for either
player; each option accepts `random` or `mcts`. Each agent owns its generator.
The batch assigns Black `seed + 2 * game_index` and White the next integer,
starting at game index zero. Seeds are signed 64-bit integers. Repeating settings
reproduces moves and outcomes; elapsed time varies. Existing seeded games and
JSON records remain compatible.

MCTS settings apply to both search agents: `--simulations 128` sets the exact
number of simulations per move, `--rollout-limit 200` caps moves in each random
rollout, and `--exploration 1.4142135623730951` sets UCT's exploration weight.
Simulation counts must be positive; rollout limits and exploration weights
must be nonnegative. A rollout limit of zero still evaluates terminal tree
leaves exactly but immediately truncates nonterminal leaves.

Each JSON record contains size, komi, move limit, both seeds, moves, game length
(including passes), elapsed seconds, termination reason, score, and winner.
Placements are `[row, column]`; pass is `null`. Winner is `1` for Black, `2` for
White, or `null` for a completed draw. `two_passes` marks completion;
`move_limit` marks an unfinished game with `null` score and winner. A second pass
on the last allowed action still completes the game.

Batches record agent types and MCTS settings. Each search entry records its
move number and player, simulations, root visits and accumulated value, active
search time, simulation throughput, completed and truncated rollout counts,
and root-child moves with visits and accumulated values. Child values use the
child state's player perspective. Random-only games retain their existing
record format.

Replay checks legality and recorded outcomes using the rules engine. The output
file is overwritten when running a batch; use different paths to keep
experiments. Generated `results/` files are ignored by Git.

## How search chooses a move

MCTS builds a fresh tree for each decision. A node holds a position separately
from its visit count `N` and accumulated value `W`. Values describe the player
to move at that node: a terminal win is `+1`, a loss is `-1`, and a draw is `0`.

Each simulation performs four steps:

1. **Selection:** follow fully expanded nodes using UCT. Unvisited children
   take priority; otherwise choose the largest value of
   `-W_child / N_child + C * sqrt(log(N_parent) / N_child)`.
2. **Expansion:** choose one unexpanded legal action uniformly, including pass,
   and create its successor node.
3. **Simulation:** play uniformly random legal actions from that leaf until
   two passes end the game or the rollout limit is reached.
4. **Backup:** add the result to every node on the selected path, incrementing
   each visit count and negating the value at every parent step.

The minus sign in selection converts the child's average to the parent's
perspective. `C` controls exploration; less-visited children receive a larger
exploration bonus. Backup negates values because the players alternate turns.
After the simulation budget, choose the root child with the most visits.
Visit-count ties choose the first expanded child without consuming randomness.

Unfinished rollouts contribute zero as an approximation. These are recorded as
truncations, not actual drawn games; a high truncation rate limits the
information available to search. A terminal result reached on the final
allowed rollout move is still evaluated exactly. Search does not use neural
networks, learned values, or tactical rollout heuristics.

The default 128 simulations is a modest educational budget. Initially a 9x9
board has 82 legal actions including pass. Budgets at or below the number of
legal actions only expand each sampled root action once, leaving visit-count
ties and weak decisions. Larger budgets allow UCT to revisit actions and make
deeper comparisons. A few wins against random play do not establish strength.

Measure search on an empty board with:

```powershell
.\build\runner.exe --benchmark --size 9 --simulations 128 --seed 10
.\build\runner.exe --benchmark --size 3 --simulations 500 --seed 10 --output results/tiny_search.json
```

This reports simulations per second and rollout truncations, saving settings,
root-child statistics, and the selected move. The default output is
`results/search_benchmark.json`. A benchmark file describes a single search;
it is not a replayable game record. Timing counts active simulation work and
excludes pauses between GUI callbacks.

## Run the tests

```powershell
.\build\tests.exe --oracle tests/fixtures/migration.json
.\build\play.exe --self-test
```

Or build and run both with `.\build.ps1 -Test`. Rules tests cover captures,
shared liberties, suicide, ko, passes, scoring, immutable successors, seeded
replay, move limits, and JSON validation. Migration fixtures preserve 98 original
positions and four original seeded games, including negative and large seeds.
Search tests cover hand-scored UCT choices, alternating backup signs, terminal
rewards, exact simulation counts, rollout cutoffs, legal outputs, unchanged input
states, deterministic seeds, and equivalent incremental searches. The GUI
checks exercise real Tk events and controls in a hidden window.

## Rules contract

- Black starts. Points are zero-based `(row, column)`; neighbors are orthogonal.
- `EMPTY=0`, `BLACK=1`, `WHITE=2`. `Move` is `std::optional<Point>`; `PASS` is empty.
- Captures remove entire opponent groups with no liberties before suicide is
  checked. Suicide is forbidden.
- Simple ko forbids a placement that repeats the board before the last action.
  Longer repetition cycles are not prevented.
- Two consecutive passes end play; a placement resets the pass count. No further
  actions are legal after termination.
- Area scoring counts stones plus empty regions bordered exclusively by one
  color. Regions bordering both colors or neither color are neutral.
- White receives configurable komi, default 7.5. Prisoners add no separate points.
- Capture dead stones before passing; remaining stones count toward area. There
  is no automatic dead-group adjudication or tournament-rule compatibility claim.

`GameState::play` returns a new state and throws `IllegalMove` for illegal actions.
Positions expose read-only board access. `score()` can inspect live positions;
`winner()` requires termination and returns an optional color, empty for a draw.
Small square boards are supported for experiments. Direct construction validates
shape and values, rather than proving the position arose through legal play.

## Study the code

1. Read `GameState::new_game`, `at`, and `neighbors` in [src/state.cpp](src/state.cpp).
2. Trace `region` and `group_and_liberties`: a stack explores connectivity and a
   set counts each liberty once.
3. Read `play`: copy, place, capture, check suicide, check ko, return a successor.
4. Read pass handling, `score`, and `winner`.
5. [src/random.cpp](src/random.cpp) samples the legal action list. Its generator
   preserves the original seed sequences and deterministic wood-grain pattern.
6. [src/runner.cpp](src/runner.cpp) alternates agents, records games, and replays.
7. Read [include/betago/mcts.hpp](include/betago/mcts.hpp), then follow
   `MctsSearch::simulate` in [src/mcts.cpp](src/mcts.cpp). Predict a small tree's
   values before tracing `select_child`, `terminal_value`, and `backpropagate`.
8. [tests/test_mcts.cpp](tests/test_mcts.cpp) supplies known-outcome trees and
   rollout examples to check those predictions.
9. [src/window.cpp](src/window.cpp) draws the original board and handles controls.
   [src/tk.cpp](src/tk.cpp) loads Tk through its C API.
10. [tests/test_main.cpp](tests/test_main.cpp) provides hand-checked diagrams,
    integration tests, and migration checks.

The C++ port deliberately keeps flood fills, explicit state copies, and a single
legality definition. Future optimization should follow measurements. Neural
networks and training are not implemented yet.
