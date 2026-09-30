# BetaGo

A C++20 Go engine with the same playable wooden 9x9 board, capture and legality
checks, area scoring, seeded random agents, and classical Monte Carlo tree
search (MCTS). The long-term goal is a small self-learning Go engine inspired by
[KataGo](https://github.com/lightvector/KataGo).
Currently it provides the rules environment, random and MCTS play, game
recording, search statistics, an arena for repeatable agent comparisons, a
small CPU policy/value network, neural MCTS guided by its policy and value,
and a resumable self-play training loop with checkpoint evaluation and native performance profiling.

## Build

On this Windows machine, run from the repository root:

```powershell
.\build.ps1
```

The script builds optimized `build/play.exe`, `build/runner.exe`,
`build/network.exe`, `build/selfplay.exe`, `build/profile.exe`, and `build/tests.exe`. On its first run it downloads a pinned portable C++ compiler
into `.tools/` and verifies the archive's checksum. Nothing is installed globally.
Later builds reuse the compiler and unchanged object files. Use `-DebugBuild`
for an unoptimized build with debug information.

Windows Smart App Control can block locally built unsigned executables even
after compilation succeeds. A trusted signed build or an appropriate Windows
development environment is then needed to launch them. See Microsoft's
[Smart App Control signing guidance](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control).

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
before running the visual board or its tests. The rules, network, search,
runner, and native tests are portable C++; the current GUI loader and helper build script
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
player; each option also accepts `policy` or `neural-mcts` with a checkpoint
as described below. Random and classical MCTS agents each own their generator.
The batch assigns Black `seed + 2 * game_index` and White the next integer,
starting at game index zero. Seeds are signed 64-bit integers. Repeating settings
reproduces moves and outcomes; elapsed time varies. Neural agents use fixed
checkpoint predictions and deterministic ties. Existing seeded games and
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
`--agent-a` and `--agent-b`; both also accept `policy` and `neural-mcts` with a
checkpoint. Compare two classical search budgets
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
so later changes to code or machines are visible. The arena evaluates random,
classical MCTS, direct policy, and neural MCTS agents. The self-play command
coordinates game generation, network training and this checkpoint evaluation.

## Train the policy/value network

The network is implemented in C++20 using the standard library. Forward passes,
backpropagation, and SGD with momentum run on the CPU, with no ML runtime or GPU
installation. This small implementation is intended for study and correctness
checks. Its checkpoint can also guide neural MCTS in the visual board and arena.

Generate nine scripted 9x9 capture-and-pass examples, then fit them:

```powershell
.\build\network.exe --make-demo results/neural_demo.json
.\build\network.exe --train results/neural_demo.json --verify-fit
.\build\network.exe --inspect results/policy_value.json --position results/neural_demo.json --example 0
```

Training prints losses and saves `results/policy_value.json` plus
`results/neural_training.json`. The report includes initial/final losses,
predictions, sampled training history, settings, and compiled source identity.
The checkpoint contains dimensions, parameter layout and values, feature
schema, train/eval mode, update count, momentum buffers, and the last optimizer
settings. Inspecting without `--position` evaluates an empty board with komi 7.5;
use `--komi` to change that inspection position.

The demo's policy targets are the scripted actions, including pass. Its value
targets are derived from the actual final winner under the rules engine, from
each saved position's player-to-move perspective: win `+1`, loss `-1`, draw `0`.
The examples use komi 0.5 and preserve the previous board for simple ko.
Fitting these examples proves memorization and functioning gradients. It does
not measure Go playing strength. The existing wooden GUI and classical agents
continue to run with their previous commands.

Defaults are 500 epochs, the full dataset per batch, eight convolution channels,
16 hidden value units, seed 0, learning rate 0.02, momentum 0.9, and L2 zero.
An epoch visits each example once; a smaller batch gives several updates per
epoch. For example:

```powershell
.\build\network.exe --train results/neural_demo.json --epochs 500 --batch-size 3 --seed 10 --output results/small_batch.json --report results/small_batch_training.json
.\build\network.exe --train results/neural_demo.json --resume results/policy_value.json --epochs 50 --output results/continued.json --report results/continued_training.json
```

`--channels`, `--value-hidden`, `--learning-rate`, `--momentum`, `--l2`,
`--log-every`, `--output`, and `--report` customize training. A resumed checkpoint
supplies its architecture and momentum history; training options supply the
optimizer settings for the new run. The minibatch shuffle starts again from
`--seed` on each invocation. Checkpoints preserve the next update for the same
batch and optimizer, but do not save the CLI sampler's position. Full-batch
continuation has no shuffle dependency. Outputs are overwritten, so choose
distinct paths to retain experiments.

`--verify-fit` requires mean policy KL at most 0.05, maximum per-example KL at
most 0.15, value MSE at most 0.01, and maximum value error at most 0.2. KL compares
the prediction with the target distribution, including soft targets. The
command saves its results and exits 2 if this check fails; malformed inputs or
invalid settings exit 1. These thresholds check the supplied training set.

Inputs use seven contiguous channel-first planes. For one 9x9 position the
shape is `[7, 9, 9]`; a packed batch has shape `[batch, 7, 9, 9]`:

| Plane | Meaning |
| --- | --- |
| 0 | Current player's stones |
| 1 | Opponent's stones |
| 2 | Exact simple-ko forbidden placements |
| 3 | Constant 1 after one consecutive pass, otherwise 0 |
| 4 | Constant terminal flag after two passes |
| 5 | Constant `tanh(signed_komi / board_area)`, with `+komi` for White and `-komi` for Black |
| 6 | Legal placements, checked through the rules engine |

Actions are row-major: `row * board_size + column`; pass is the final action
at `board_size * board_size`. Legal-action masks exclude occupied points,
suicide, and ko. Nonterminal pass is always legal. A terminal position has no
legal actions and its policy is all zeros; its network value remains a learned
prediction. Training targets must be nonterminal, finite, normalized, legal
policy distributions and values in `[-1, 1]`.

The shared trunk applies a 3x3 convolution with one-cell zero padding and ReLU,
producing `[8, 9, 9]` by default. Flattening gives 648 activations. A dense policy
head produces 82 logits and a stable softmax over legal actions only. A separate
value head uses 16 ReLU units and a scalar tanh output. Board sizes 1 through 19
are supported; each checkpoint fixes its board size. There are no residual
blocks, dropout, or batch normalization in this first network.

The objective is batch-mean policy cross-entropy plus batch-mean squared value
error, plus optional `0.5 * l2 * sum(parameters^2)`. L2 includes biases as well as
weights. Gradients from both heads add in the shared trunk. SGD updates
`velocity = momentum * velocity + gradient`, then
`parameter -= learning_rate * velocity`. Train/eval modes make update intent
explicit: prediction is identical in either mode, and updates require train
mode. Checkpoints reject incompatible shapes, schemas, and nonfinite numbers.

## Use neural MCTS

Load the saved 9x9 checkpoint to play against neural MCTS on the same wooden
board, or watch it play random White:

```powershell
.\build\play.exe --neural --checkpoint results/policy_value.json --simulations 128
.\build\play.exe --watch-neural --checkpoint results/policy_value.json --simulations 128 --seed 10
```

Human play starts as Black. The checkpoint switches the existing search
buttons to **Play vs neural MCTS** and **Watch neural MCTS vs random**.
Using only `--checkpoint` opens local play with those buttons available.
Omit the checkpoint to use the original classical MCTS buttons. The board,
window dimensions, pass/undo/reset controls, capture demo, and random playback
are preserved. One PUCT simulation runs per Tk callback; stopping, undoing,
resetting, switching modes, or closing cancels the partial tree and timer.
The status shows simulations and network evaluations. `--c-puct 1.5` controls
neural exploration; `--exploration` and `--rollout-limit` belong to classical
search. The visual board requires a 9x9 checkpoint.

The runner accepts four agent kinds: `random`, `mcts`, `policy`, and
`neural-mcts`. `policy` selects the highest legal network probability in one
inference. `neural-mcts` performs PUCT search and chooses the most visited root
action. For example:

```powershell
.\build\runner.exe --black neural-mcts --white random --checkpoint results/policy_value.json --size 9 --simulations 16 --max-moves 30 --seed 10 --output results/neural_games.json
.\build\runner.exe --replay results/neural_games.json
.\build\runner.exe --benchmark --agent neural-mcts --checkpoint results/policy_value.json --size 9 --simulations 128 --output results/neural_search.json
```

For comparisons using the same checkpoint and rules:

```powershell
.\build\runner.exe --arena --agent-a policy --agent-b neural-mcts --checkpoint results/policy_value.json --size 9 --komi 0.5 --pairs 2 --simulations 16 --max-moves 30 --seed 10 --output results/policy_vs_neural.json
.\build\runner.exe --arena --agent-a neural-mcts --agent-b mcts --checkpoint results/policy_value.json --size 9 --komi 0.5 --pairs 2 --a-simulations 16 --b-simulations 16 --b-rollout-limit 20 --max-moves 30 --seed 10 --output results/neural_vs_classical.json
```

These short runs check comparison and replay, with truncations reported as
unfinished games. The demo checkpoint memorized nine scripted examples and
has no broad training. Neural search is therefore a functional foundation,
without an established playing-strength improvement. Equal simulation counts
also represent different work for random rollouts and network evaluations.
Color exchange still applies. The generic arena's uncertainty formula assumes
independent seed pairs; deterministic neural comparisons repeat the same games
across pairs and do not meet that assumption. The self-play loop reports null
intervals for these comparisons and saves its candidate promotion decision.

`--checkpoint` supplies a model to neural agents without an explicit override.
Normal batches can use `--black-checkpoint` and `--white-checkpoint`; the arena
can use `--a-checkpoint` and `--b-checkpoint` to compare different models.
`--simulations` is a common search-budget fallback, with arena overrides
`--a-simulations` / `--b-simulations`. Neural exploration can be overridden by
`--a-c-puct` / `--b-c-puct`. Agent-specific options must match their mode, and
unused checkpoint or PUCT options are rejected. All checkpoint sizes are
checked before play begins. Evaluation loads immutable snapshots and makes
no optimizer updates.

Saved neural configurations include checkpoint path, dimensions, feature
schema, initialization seed, training update count, parameter count, and a
content fingerprint labeled `fnv1a64`. This fingerprint identifies the input
file for reproducibility; it is not a cryptographic authenticity check. Search
records include every legal root action, its prior, visits, and value sum for
the child player. Policy-only records include legal probabilities and value
for the player to move. The arena separates network and exact terminal
evaluations from classical rollout completion/cutoff counts. With no rollout
work, the rollout truncation rate is `null`.

PUCT adds a policy-guided exploration bonus to each move's estimated value:

```text
score = -W_child / N_child + c_puct * P_child * sqrt(N_parent + 1) / (N_child + 1)
```

An unvisited child uses value zero. `P_child` is the network policy after
illegal actions are masked and legal weights are normalized. The `+1` in the
parent term gives priors an effect on the first simulation. Increasing visits
reduces a child's exploration bonus. Unlike UCT, PUCT can revisit a promising
move before visiting every root action.

A search first infers and expands the root, without adding a visit. Each
simulation then selects children until it reaches an unexpanded leaf or a
terminal position. A nonterminal leaf supplies its policy and current-player
value from the network; a terminal leaf uses the exact rules result and skips
inference. Backup increments visits and negates value at each parent. The root
inference is included in search time and evaluation counts: network plus
terminal evaluations equals simulations plus one. No random rollouts occur.

Final move selection uses the most visits, then the largest prior, then legal
row-major order with pass last. Selection score ties use that same legal order.
Inference and search are deterministic for a fixed checkpoint and settings;
identity seeds continue to control random agents and classical MCTS. There is
no noise or temperature sampling in these evaluation agents. Self-play has
separate exploration settings described below. All-zero legal policy weights fall
back to uniform legal priors; invalid shapes, negative/nonfinite weights, and
out-of-range values are rejected.

## Learn through self-play

Run a first 9x9 iteration from a fresh seeded network, then open the accepted
checkpoint in the same visual board:

```powershell
.\build\selfplay.exe --output results/selfplay --iterations 1
.\build\play.exe --neural --checkpoint results/selfplay/best.json
```

Training runs in the console and writes checkpoints; `play.exe` displays the
board. The wooden board, shaded stones, coordinates and controls stay the same.
The GUI loads a checkpoint when it starts; reopen it to use a later checkpoint.
For more iterations, restore the run's settings, replay and accepted model:

```powershell
.\build\selfplay.exe --resume results/selfplay --iterations 10
```

A new run requires a new output directory. `--checkpoint MODEL` starts from an
existing network, including its optimizer history, rather than initializing
weights again. Its board dimensions must match self-play; it supplies the
architecture. Resume accepts only `--iterations`; to change settings, start a
new output directory with the desired checkpoint. Each iteration uses recorded
separate seeds for game sampling, minibatches and arena identities. Checkpoint
and replay fingerprints detect changed committed inputs; they are content
identifiers, not cryptographic authentication.

The default iteration generates four games with 64 PUCT simulations per move,
komi 7.5 and a 200-move limit. Each pre-action state saves a policy target
`pi[a] = root_visits[a] / simulations`, including pass. Illegal actions have
zero mass. The actual move is sampled from `pi[a]^(1/temperature)` during the
first 20 moves, at temperature 1 by default; subsequent moves use the regular
maximum-visit choice. Setting temperature to zero or temperature-moves to zero
selects maximum visits throughout. Temperature affects action selection, while
training targets always retain the raw normalized visits.

Self-play also mixes each root's legal neural priors with 25% uniform legal
probability by default. This is a simple exploration floor, not Dirichlet
noise. It applies only to the root of each self-play search, without changing
leaf values or evaluation search. `--root-uniform-mix 0` disables it.
`--simulations`, `--c-puct`, `--temperature`, `--temperature-moves`,
`--root-uniform-mix` and `--max-moves` control these choices.

After two passes end a game, every saved state's value target is +1 if its
player to move won, -1 if that player lost, or 0 for a genuine draw. Move-limit
games retain their moves and visit statistics for inspection but provide no
training examples. There is no resignation, forced pass, or guessed terminal
value. If all games truncate and the replay is empty, the iteration records a
skip, leaves the checkpoint unchanged, and the command exits 2. A later
iteration can try again. When older completed games exist, training can use
that replay even if the current batch produced no new complete games.

The replay keeps the most recent 100 **whole completed games**, removing the
oldest first; `--replay-games` changes that limit. The trainer samples positions
uniformly with replacement, so longer games contribute more positions. Each
candidate continues the accepted model's weights and momentum, takes 100
updates of 32 examples, and uses SGD with learning rate 0.01, momentum 0.9 and
L2 coefficient 0.0001. These have `--updates`, `--batch-size`, `--learning-rate`,
`--momentum` and `--l2` options. Failed candidates do not change the accepted
model's optimizer state.

The objective is mean policy cross-entropy plus mean squared value error plus
`0.5 * l2 * sum(parameters^2)`, including biases. Cross-entropy teaches the
network the search's visit distribution; value MSE teaches the eventual result
from that position's player perspective. L2 discourages large parameters.
Reports separate all three terms and policy KL (cross-entropy minus target
entropy). Initial/final metrics use the full current replay; per-update metrics
describe the sampled batch **before** its update. Lower training loss shows
fitting these examples; it does not establish better Go play.

Each candidate plays the accepted model with colors exchanged, using 64
simulations per decision for both and a 400-move limit. Evaluation uses no
self-play uniform mixture or temperature sampling. `--eval-simulations`,
`--eval-max-moves` and `--eval-pairs` control the arena. Promotion requires every
evaluation game to finish and candidate score (win=1, draw=0.5, loss=0) to meet
`--promotion-score`, default 0.55. Rejected candidates are still saved.
This is an empirical gate, not a guarantee of improvement: deterministic PUCT
from an empty board produces the same two color games when seed pairs repeat.
The loop therefore reports null paired confidence intervals and discloses
those duplicates. More repeated pairs do not add independent evidence.

The run directory contains:

| Artifact | Meaning |
| --- | --- |
| `run.json` | Settings, next iteration, committed replay/model paths and history |
| `initial.json` | Frozen starting weights and optimizer state |
| `best.json` | Copy of the currently accepted checkpoint for the GUI |
| `iteration-000001-attempt-1/games.json` | Self-play moves, raw states, root visits, outcomes and seeds |
| `iteration-000001-attempt-1/examples.json` | Labels from this batch's completed games; absent if none completed |
| `iteration-000001-attempt-1/replay.json` | Recent completed-game buffer after this iteration |
| `iteration-000001-attempt-1/candidate.json` | Trained candidate, even when rejected; absent after an empty-replay skip |
| `iteration-000001-attempt-1/arena.json` | Paired candidate/incumbent games and measured outcomes |
| `iteration-000001-attempt-1/report.json` | Training losses, evaluation decision and elapsed time |

The manifest is replaced only after an iteration finishes. Resume recovers the
last committed model and buffer; an interrupted attempt is retained, and a new
attempt directory uses the same iteration seeds. Use one process per run
directory. Full replay metrics and explicit CPU search are intentionally
simple; profile them before adding batching or other optimizations.

For a small, fast exercise of the whole loop:

```powershell
.\build\selfplay.exe --output results/selfplay_tiny --size 3 --komi 0.5 --games 2 --simulations 16 --eval-simulations 16 --updates 20 --batch-size 8 --max-moves 100 --eval-max-moves 100 --seed 10
```

A 3x3 checkpoint cannot be used in the 9x9 GUI. Neither one iteration nor a
particular milestone guarantees strong moves. Strength depends on useful
completed games, search and model capacity, training stability and evaluation
against varied opponents and positions. This loop now supplies the learning
mechanism and records the evidence to judge its progress.

## Measured performance (milestone 8)

The native profiler measures a fixed collection of seeded positions, 64-simulation
searches, a capped self-play game, and ten CPU training updates of 32 examples.
The training targets in this benchmark are synthetic uniform policies; the
benchmark measures execution speed, not playing strength. It reads checkpoints
without updating your training run or its accepted model.

```powershell
.\build\profile.exe --checkpoint results/selfplay/best.json --output results/profile_before.json
# After rebuilding a performance change, use the same immutable checkpoint:
.\build\profile.exe --checkpoint results/selfplay/best.json --compare results/profile_before.json --output results/profile_after.json
```

Use a versioned `iteration-.../candidate.json` when a training process could
replace `best.json` during the comparison. Options include `--repeats 5`,
`--simulations 64`, and `--selfplay-moves 32`. Omit `--checkpoint` for a fixed
fresh 9x9 network. A prefix that reaches its move limit is reported as truncated;
it supplies no final outcome labels or training examples.

Reports record the positions, model and optimizer fingerprints, compiler flags,
machine details, and all timing samples. Timing medians are collected with
profiling disabled after a warm-up. A separate opt-in profiling pass counts
copies, group work, move probes, encoding, inference, expansion, and training.
Inclusive durations overlap; exclusive durations exclude nested scopes. Scope
timing adds clock overhead, so use the unprofiled medians for speed comparisons.
Group benchmark units represent sweeps over every occupied point in a position.

`--compare` checks the workload, machine, compiler flags and exact fingerprints
of predictions, loss/gradients, two momentum updates, search statistics and
seeded self-play moves/visit targets. It records speed ratios and returns a
nonzero status on a mismatch. Run comparisons on a quiet machine; elapsed time
will still vary. Profiling is local to the calling thread and inactive during
normal play and training.

The measured improvement uses compact internal liberty walks with direct board
neighbors and returns as soon as a liberty is found. Placement successors copy
only the board and history they need. Capture ordering, suicide and simple-ko
checks, pass behavior, sorted public group results, and checkpoint formats remain
the same. Independent reference tests cover these rules and feature planes,
including 19x19 positions and larger boards supported by the rules engine.

On this machine, five unprofiled samples using the fixed milestone 7 iteration 1
checkpoint produced these median times:

| Workload | Before | After | Speed ratio |
| --- | ---: | ---: | ---: |
| Five 9x9 searches, 64 simulations each | 0.312 s | 0.115 s | 2.70x |
| Seeded 32-move self-play prefix, 64 simulations per move | 1.753 s | 0.905 s | 1.94x |

All five behavior fingerprints matched exactly. These are local measurements
of a fixed workload, not a promise for every position or a measure of strength.
The unchanged public group API and neural calculations provide useful control
workloads. Results are saved in `results/m8_baseline.json` and
`results/m8_after.json`. All 151 native tests and the GUI checks passed; the
608x876 canvas dump matched the milestone 7 canvas byte for byte.

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
Neural tests check feature planes and ko history, action and batch shapes,
masked probabilities, stable cross-entropy, central finite-difference gradients,
shared-head gradients, batch averaging, optimizer updates, tiny-dataset fitting,
and checkpoint prediction/optimizer equivalence. They use small deterministic
examples rather than a playing-strength assertion.
PUCT tests use controlled policies and values to check masking, exploration,
alternating backups, exact terminal outcomes, inference counts, incremental
parity, legal outputs, and model lifetime. Neural arena tests check checkpoint
snapshots, replay, color exchange, deterministic games, and separate inference
and rollout accounting. GUI checks also exercise partial neural-search
cancellation and verify the original dimensions and canvas.
Self-play tests verify raw visit distributions, separate action sampling,
root-only exploration, exact outcome perspectives, replayed ko history,
truncated-game exclusion, FIFO persistence, deterministic minibatches and
checkpoint continuation, plus the full tiny-board path through arena evaluation.

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
13. Trace `encode_position` in [src/features.cpp](src/features.cpp), then inspect
    the raw positions and targets in the generated demo JSON. Follow
    [src/dataset.cpp](src/dataset.cpp) to see how the final winner labels turns.
14. Read [include/betago/network.hpp](include/betago/network.hpp), then trace
    `forward`, `objective`, and `train_batch` in [src/network.cpp](src/network.cpp).
    Match each parameter block's shape with the loops that use it.
15. Read [tests/test_neural.cpp](tests/test_neural.cpp) to compare analytical
    derivatives with finite differences. Trace checkpoint save/load and repeat
    the same next batch to understand why optimizer history matters.
16. Read [src/neural_mcts.cpp](src/neural_mcts.cpp): trace legal prior
    normalization, PUCT selection, leaf evaluation, and alternating backup.
    Compare those steps with classical `MctsSearch::simulate`.
17. Predict the root value after two passes on a 1x1 board, then check the
    controlled examples in [tests/test_neural_mcts.cpp](tests/test_neural_mcts.cpp).
    Follow `prepare_agent` in [src/arena.cpp](src/arena.cpp) to see how evaluation
    freezes a checkpoint before creating the agents.

18. Read [src/selfplay.cpp](src/selfplay.cpp) to trace root visits into targets,
    temperature sampling and final-result labels. Follow one pass and check the
    value sign against the pre-action player's color.
19. Read [src/replay.cpp](src/replay.cpp) for FIFO game retention and sampling,
    then [src/selfplay_main.cpp](src/selfplay_main.cpp) for freezing the incumbent,
    training a candidate, evaluating it and committing a resumable iteration.
20. [tests/test_selfplay.cpp](tests/test_selfplay.cpp) exercises that complete
    path with tiny networks and deterministic outcomes.
21. [src/profile_main.cpp](src/profile_main.cpp) separates unprofiled timing
    samples from nested profiling scopes in [include/betago/profile.hpp](include/betago/profile.hpp).
    Compare internal `has_liberty` with the public group traversal, then inspect
    the independent reference checks in [tests/test_performance.cpp](tests/test_performance.cpp).

The C++ implementation keeps explicit state copies and a single legality
definition. Internal group walks now avoid building sets when a liberty check
is sufficient. Further optimization should follow measurements.
