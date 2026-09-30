# BetaGo

A C++20 Go engine with the same playable wooden 9x9 board, capture and legality
checks, area scoring, seeded random agents, and classical Monte Carlo tree
search (MCTS). The long-term goal is a small self-learning Go engine inspired by
[KataGo](https://github.com/lightvector/KataGo).
Currently it provides the rules environment, random and MCTS play, game
recording, search statistics, and an arena for repeatable agent comparisons.

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

## Compare agents in the arena

The arena measures the existing agents. It plays each comparison twice with
colors exchanged, records both games, and reports outcomes and timing.

Start with a small board and modest search budget:

```powershell
.\build\runner.exe --arena --pairs 10 --size 3 --komi 0.5 --a-simulations 32 --seed 10 --max-moves 100 --output results/arena_mcts_random.json
.\build\runner.exe --replay results/arena_mcts_random.json
```

Agent A defaults to `mcts` and B to `random`. Select either with
`--agent-a random|mcts` and `--agent-b random|mcts`. Compare two search budgets
with:

```powershell
.\build\runner.exe --arena --agent-a mcts --agent-b mcts --pairs 10 --size 3 --komi 0.5 --a-simulations 32 --b-simulations 64 --seed 10 --max-moves 100 --output results/arena_32_vs_64.json
```

For a short 9x9 check of recording and timing, use:

```powershell
.\build\runner.exe --arena --pairs 1 --size 9 --simulations 8 --rollout-limit 20 --max-moves 4 --seed 10 --output results/arena_9x9_smoke.json
```

That four-move check is too short to measure playing strength. Larger boards
and budgets require substantially more time. The arena defaults are five
pairs, size 3, komi 7.5, move limit 100, seed 0, and output `results/arena.json`.
One pair means two games; use `--pairs`, rather than `--games`, in arena mode.
The `--black` and `--white` options belong to ordinary batches. Arena, replay,
and benchmark modes are mutually exclusive.
The 3x3 examples use komi 0.5 because 7.5 can dominate scoring on such a small
board and mask differences between agents. Changing komi changes the task;
keep it identical within a comparison. Exchanging colors balances assignments
but does not remove the effect of komi. The default remains 7.5, matching the
9x9 visual board.

Common `--simulations`, `--rollout-limit`, and `--exploration` settings provide
fallbacks for both agents. Override them independently with `--a-simulations`,
`--a-rollout-limit`, `--a-exploration`, and the corresponding `--b-` options.
Their defaults remain 128 simulations, 200 rollout actions, and exploration
`sqrt(2)`. These are fixed simulation budgets per move; equal simulation counts
do not guarantee equal thinking time. Compare wall times on a quiet machine
using the same build and rules. Equal-time search budgets are not implemented.

For pair index `i`, starting at zero, A receives `seed + 2*i` and B receives the
next integer. The first game has A as Black, and the second has B as Black.
Each game constructs fresh agents using the same identity seeds, so changing
colors does not inherit the preceding game's random-generator state. Repeating
the settings reproduces moves and outcomes; times and recording timestamps
vary.

The summary reports wins, losses, draws, and truncated games for each identity
and color, plus mean game length and mean move time. Only games completed by
two passes have an outcome. Win rate is `wins / completed_games`; draws remain
in the denominator. Score rate is `(wins + 0.5*draws) / completed_games`.
An unfinished game contributes neither a loss nor a draw, and rates with no
completed games are `null` (`n/a` in the terminal). Mean game length includes
truncated games; a separate completed-game mean excludes them.

Search throughput divides total simulations by total active search seconds,
rather than averaging per-move rates. Mean move time includes the full agent
decision. The summary also separates completed and truncated rollouts and
reports their truncation rate. Rollout cutoffs remain search approximations;
they are distinct from games stopped at the arena's move limit.

The 95% win-rate and score-rate bounds treat a complete color pair as one
observation. For each pair, average its two win indicators, or its two scores
with draws worth 0.5. Across `n` complete pairs, the Hoeffding bound has half
width `sqrt(log(40)/(2*n))`, clipped to `[0,1]`. This assumes independent seed
pairs and is conservative with small samples. Both games must complete for a
pair to enter these bounds. Individually completed games in an incomplete pair
still enter ordinary outcome rates. Bounds are `null` when no pairs complete.
Because move-limit exclusions can select which pairs survive, the bounds
describe the population where both color games complete; they do not resolve
the missing outcomes. The saved bound records its sample size, method,
population, and independence assumption.

Arena files use the existing schema version 1 and add `mode: "arena"`, A/B
configurations, match settings, metadata, and a summary. Each game retains the
replayable moves, score, and termination fields, adding pair identity, color
assignment, identity seeds, and every decision's player, agent, full runtime,
and optional search statistics. Replay validates game legality and recorded
outcomes using the same rules engine.

Metadata records the Git revision and dirty state at build time, a SHA-256
fingerprint of the authored code, build timestamp, compiler, flags, profile,
and C++ standard. It also records the run timestamp, host name, OS family,
architecture, available CPU identifier, and hardware-thread count. Unavailable
machine and Git values are `null`. Keep this metadata with comparison results
so later changes to code or machines are visible. The arena performs evaluation;
neural networks, self-play training, and reinforcement learning are not yet
implemented.

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
Arena tests verify exchanged colors, identity seeds, fresh generators, exact
W/L/D accounting, draws and truncations, per-color results, paired uncertainty,
weighted timing, deterministic games, replay compatibility, and invalid
settings using small controlled games.

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
11. Read the arena types in [include/betago/arena.hpp](include/betago/arena.hpp),
    then trace `run_arena` in [src/arena.cpp](src/arena.cpp): construct agents,
    exchange colors, record decisions, and summarize completed outcomes.
12. Predict the totals for a pair of pass-only games before reading
    [tests/test_arena.cpp](tests/test_arena.cpp). Trace `summarize_arena` to see
    why two games contribute one independent sample to the uncertainty bounds.

The C++ port deliberately keeps flood fills, explicit state copies, and a single
legality definition. Future optimization should follow measurements. Neural
networks and training are not implemented yet.
