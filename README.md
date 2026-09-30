# BetaGo

A C++20 Go engine with the same playable wooden 9x9 board, capture and legality
checks, area scoring, and seeded random agents. The long-term goal is a small
self-learning Go engine inspired by [KataGo](https://github.com/lightvector/KataGo).
Currently it provides the rules environment, random play, and game recording.

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
```

Click an intersection to play. Black and White alternate on the same computer.
Use **Pass**, **Undo**, and **New game** to explore positions. Two consecutive
passes end the game and display the score. The wooden board keeps coordinates
on all four sides, shaded stones, and a contrasting ring around the last move.

**Watch capture demo** plays the original short scripted capture sequence.
**Watch random game** starts a game between two seeded random agents, one move
at a time. Stop playback to continue the position yourself. Undo and New game
also cancel playback.

Use `--delay-ms 250` for faster random playback or `--max-moves 100` for a shorter
limit. The visual board remains 9x9 with komi 7.5. A second pass displays the
winner; the move limit displays truncation without a final score. Visual
playback does not save JSON records.

## Run and replay batches

These commands run games and replay final boards in the terminal:

```powershell
.\build\runner.exe --size 3 --games 5 --seed 10 --max-moves 100 --output results/tiny.json
.\build\runner.exe --size 9 --games 3 --seed 10 --max-moves 500 --output results/9x9.json
.\build\runner.exe --replay results/9x9.json
```

Both players choose uniformly from all legal actions, including pass. This is
a baseline with no tactical judgment. Each random agent owns its generator.
The batch assigns Black `seed + 2 * game_index` and White the next integer,
starting at game index zero. Seeds are signed 64-bit integers. Repeating settings
reproduces moves and outcomes; elapsed time varies. Existing seeded games and
JSON records remain compatible.

Each JSON record contains size, komi, move limit, both seeds, moves, game length
(including passes), elapsed seconds, termination reason, score, and winner.
Placements are `[row, column]`; pass is `null`. Winner is `1` for Black, `2` for
White, or `null` for a completed draw. `two_passes` marks completion;
`move_limit` marks an unfinished game with `null` score and winner. A second pass
on the last allowed action still completes the game.

Replay checks legality and recorded outcomes using the rules engine. The output
file is overwritten when running a batch; use different paths to keep
experiments. Generated `results/` files are ignored by Git.

## Run the tests

```powershell
.\build\tests.exe --oracle tests/fixtures/migration.json
.\build\play.exe --self-test
```

Or build and run both with `.\build.ps1 -Test`. Rules tests cover captures,
shared liberties, suicide, ko, passes, scoring, immutable successors, seeded
replay, move limits, and JSON validation. Migration fixtures preserve 98 original
positions and four original seeded games, including negative and large seeds.
The GUI checks exercise real Tk events and controls in a hidden window.

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
7. [src/window.cpp](src/window.cpp) draws the original board and handles controls.
   [src/tk.cpp](src/tk.cpp) loads Tk through its C API.
8. [tests/test_main.cpp](tests/test_main.cpp) provides hand-checked diagrams,
   integration tests, and migration checks.

The C++ port deliberately keeps flood fills, explicit state copies, and a single
legality definition. Future optimization should follow measurements. Search,
neural networks, and training are not implemented yet.
