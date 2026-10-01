# External GTP matches

The arena can run one external reference profile per experiment on Windows.
Each game starts a fresh engine process. BetaGo checks every generated move
with its own rules and scores only games ending through two consecutive
passes. The GUI, neural search, and self-play commands keep their existing
behavior.

These are configuration-specific comparisons. A reference label is
**unvalidated metadata**, including labels copied from a KataGo HumanSL
profile. This version produces no kyu/dan estimate or 19x19 rank claim.
[KataGo's HumanSL guidance](https://github.com/lightvector/KataGo/blob/master/docs/Analysis_Engine.md#human-sl-analysis-guide)
describes rank-conditioned imitation; it does not supply a calibrated 9x9 rank ladder.

## Use the local KataGo setup

The project now includes a working [katago.json](../profiles/katago.json) profile
and [katago-cpu.cfg](../profiles/katago-cpu.cfg). The executable and model live
under ignored `.tools/katago/`. Restore those local dependencies with:

```powershell
.\setup-katago.ps1
.\match-katago.ps1
```

The setup script downloads pinned official files and checks their SHA-256
checksums. It installs nothing globally. This machine uses the plain Eigen
x64 CPU build of [KataGo v1.18.1](https://github.com/lightvector/KataGo/releases/tag/v1.18.1)
through Windows emulation on its Qualcomm ARM64 processor. The normal
`kata1-b6c96-s175395328-d26788732.txt.gz` model is a small historical model
from the [official network list](https://katagotraining.org/networks/);
its file format version is 8 and it supports the required rules.

The shortcut compares `results/selfplay/best.json` with this profile on 9x9
at komi 7.5. Defaults are one color-swapped pair, 64 BetaGo simulations per
move, a 400-move cap, and output `results/katago_smoke.json`. The shortcut also
validates the saved report with offline replay. KataGo uses one search thread, at most 32 visits,
and a two-second search limit. Search and neural-evaluation seeds are passed
to KataGo, while `seeded_stochastic` remains false and uncertainty bounds
remain unavailable. For a longer comparison:

```powershell
.\match-katago.ps1 -Pairs 5 -Output results/katago_comparison.json
```

These commands evaluate the current checkpoint in the terminal. They do not
train it or promote a replacement. The wooden-board GUI remains available
through `.\build\play.exe`.

## Create a custom profile

Profiles are JSON files. To use another executable or model, copy
[katago.example.json](../profiles/katago.example.json) to a new profile and
replace its executable, model, and configuration paths. Paths are resolved
against the profile directory.
Arguments are a JSON array of individual argv values; do not add shell quoting
inside them. BetaGo launches the executable directly without a shell.
An argv entry matching a declared file's `path` resolves against the profile
directory, including when the engine uses a different working directory.
KataGo model/configuration path arguments also resolve there. Other argv values
remain literal. Unknown profile fields are rejected to catch misspelled settings.

The profile specifies:

- `schema_version: 1`, `kind: "external_gtp_profile"`.
- `executable`, `arguments`, `working_directory`, and `display_name`.
- `adapter`: `generic` or `katago`.
- Optional `reference_label` and `notes`; labels have no effect on play or results.
- `files`: an array of `{ "role": "model", "path": "..." }` entries for each
  configuration/model file. Include additional files read by your configuration.
  Their bytes, the profile, and the executable are fingerprinted in the report.
- Optional `timeouts`: `startup_ms` (120000), `command_ms` (30000), and
  `shutdown_ms` (2000). Startup covers the handshake and rule verification.
- Optional documented seeding, described below. Unconfigured engine seeds are
  recorded as unconfigured even though every arena identity has a seed.

To integrate a documented engine seed, include `{seed}` in the relevant argv
entry and describe the engine's behavior in `seed_documentation`. For KataGo,
the documented configuration parameters are `searchRandSeed` and `nnRandSeed`;
one argument pair can be:

```json
["-override-config", "searchRandSeed={seed},nnRandSeed={seed},numSearchThreads=1"]
```

These parameters are described in KataGo's
[example GTP configuration](https://github.com/lightvector/KataGo/blob/master/cpp/configs/gtp_example.cfg).
Declare `seeded_stochastic: true` only after documenting how the selected
engine settings produce independently seeded stochastic play. The example
profile leaves that declaration false. BetaGo records and checks integration,
but a user declaration cannot certify a strength ladder or independence.

Generic engines must support GTP version 2 and advertise `boardsize`,
`clear_board`, `komi`, `play`, `genmove`, and `quit`. Numbered response IDs must
match, with a blank line terminating each response. Responses may use LF or
CRLF and arrive in arbitrary fragments. Each response is limited to 1 MiB;
stderr is drained separately, retaining the last 64 KiB. The
[GTP specification](https://www.lysator.liu.se/~gunnar/gtp/gtp2-spec-draft2/gtp2-spec.html)
defines these commands and response framing.

Coordinates skip I and use the same orientation as the GUI: on 9x9, A1 is
`[8,0]`, J1 is `[8,8]`, A9 is `[0,0]`, and J9 is `[0,8]`. Single-letter
coordinates limit external games to sizes 1 through 25. A successful
`genmove` already changes the engine's board, so BetaGo sends only the
opponent's accepted moves as `play` commands. Full move history is preserved.

## Run a small experiment

The local profile is ready to use. The equivalent direct command for a
one-pair smoke experiment is:

```powershell
.\build\runner.exe --arena `
  --agent-a neural-mcts --a-checkpoint results/selfplay/best.json `
  --a-simulations 64 `
  --agent-b external-gtp --b-gtp-profile profiles/katago.json `
  --size 9 --komi 7.5 --pairs 1 --max-moves 400 `
  --seed 0 --output results/katago_smoke.json

.\build\runner.exe --replay results/katago_smoke.json
```

Use a distinct output filename. The runner protects checkpoint, profile,
executable, and declared model/configuration inputs from output overwrites.
It does not train, promote checkpoints, or modify the external configuration.
The external profile can instead be A using `--a-gtp-profile`. Profile options
are valid only for their `external-gtp` arena agent.

Check both attempts, their engine name/version, effective rules, loaded model
metadata, actual launch arguments, and cleanup warnings in the report. A
400-move cap can truncate a game; only a two-pass game has a winner and score.
After the smoke experiment succeeds, increase `--pairs` for a longer
comparison using the same profile. Large calibration runs are separate tasks.

KataGo's documented launch is `gtp -model MODEL -config CONFIG`; a HumanSL
experiment can add `-human-model HUMAN_MODEL` and the corresponding declared
file. Configure search budgets and optional `humanSLProfile` in your supplied
configuration. The adapter uses documented configuration overrides to disable
resignation, pondering, and artificial move delays. It reads those effective
values back instead of assuming the overrides worked.

The adapter sets and reads back simple ko, area scoring, forbidden suicide,
no tax, no button, no handicap bonus, and `friendlyPassOk=false`; komi is set
separately and verified. Unsupported commands or mismatched settings fail the
attempt. Loaded models and relevant effective search settings are recorded.
These interfaces are described in
[KataGo's GTP extensions](https://github.com/lightvector/KataGo/blob/master/docs/GTP_Extensions.md).

BetaGo's raw final-board area score remains authoritative. Dead stones must
be captured before passing. KataGo can still differ in cycle/no-result
handling and internal game termination or adjudication. Generic GTP cannot
verify an engine's full rule settings, and the report discloses that limit.
The match infrastructure does not assert complete tournament-rule equivalence.

## Failures, replay, and uncertainty

External experiments use schema 2. Existing built-in experiments continue
using schema 1, and both schemas can be replayed without an engine process.
Each schema-2 attempt has `status` (`completed`, `truncated`, `failed`, or
`resigned`), a legal move prefix, decisions, external session identities,
timing, and diagnostics. A failure includes its actor, command, typed code,
and diagnostic evidence. Launch failures, unexpected exits, timeouts,
malformed responses, wrong IDs, GTP rejections, unsupported capabilities,
and illegal moves are distinct.
The JSON report uses UTF-8. Invalid bytes in captured diagnostics are replaced
with U+FFFD when saving, so a native engine's output cannot prevent progress
from being recorded. Invalid UTF-8 in a protocol response is a malformed response.

The arena continues through the scheduled attempts without retries. It saves
progress atomically after every attempt. Failures and resignations have null
winner and score and never count as losses. A problem shutting down an already
synchronized completed game is a cleanup warning; its scored result remains.
The CLI returns status 2 when any attempt failed or resigned, while retaining
the report. Truncations remain unscored and are reported separately.

Summary rates include only two-pass completed games, with wins, losses,
draws, and results by color. Complete pairs contain two scored color-swapped
games; the report also counts incomplete pairs. Failed or truncated games can
selectively exclude difficult positions, so even a completed-game rate must
be read with those counts.

Each pair uses A's `seed + 2*pair_index` and B's next integer. Each fresh
color-swapped process receives the same identity seed. `{seed}` substitution
in arguments is recorded alongside the resolved argv and whether seeding was
configured. A seed does not itself prove stochastic independence. Paired
95% bounds are unavailable by default: repeated deterministic games do not
increase confidence. Eligibility requires documented seeded stochastic engine
behavior and actual argument integration. Refer to the profile's seed
documentation before enabling this declaration. Eligible bounds still use
only complete pairs and inherit the existing independent-pair assumption.
External search counters not exposed by GTP are null.

## Automated verification

```powershell
.\build.ps1 -Test
# Or after building:
.\build\tests.exe --oracle tests/fixtures/migration.json
.\build\play.exe --self-test
```

`fake_gtp.exe` provides deterministic scripts, strict board synchronization,
fragmented responses, protocol faults, hangs, stderr flooding, and owned child
processes. Tests use short deadlines and separate files under
`results/gtp_tests`; they do not require KataGo. CMake builds the same fake
engine and passes its target path to native tests.

The portable client lives in `include/betago/gtp.hpp` and `src/gtp.cpp`.
`src/gtp_process.cpp` contains hidden Windows launching, bounded pipe handling,
and job-object cleanup. `ArenaAgentSession` provides per-game lifecycle and
accepted-move synchronization, while the runner validates saved legal prefixes.

## Verification on 2026-09-30

The optimized Windows build passed `build.ps1 -Test`: all 151 existing native
tests and 24 GTP tests passed, including the 98 reference positions and saved
seeded-game compatibility checks. GUI interaction checks passed, and the
608x876 wooden-board canvas matched the milestone-8 canvas byte for byte.
The existing self-play checkpoint and run file retained their SHA-256 hashes.

CLI checks covered a 9x9 neural-MCTS versus fake-engine experiment at 64
simulations, schema-1 and schema-2 offline replay, failure continuation,
unscored resignations, cleanup warnings, malformed-byte diagnostics, rejection
of unused options, and input-file overwrite protection. Fake-engine results
are infrastructure checks and do not measure BetaGo's real playing strength.

The portable GTP client and unsupported-platform transport stub also
cross-compiled for x86_64 Linux. CMake target wiring was updated; CMake was
unavailable on this machine, so that build path was not executed here.

The real-KataGo smoke experiment also passed using the local CPU profile and
64 BetaGo simulations per move. Both games ended through two passes:

| KataGo color | BetaGo score | KataGo score | Moves | Game time |
| --- | ---: | ---: | ---: | ---: |
| White | 0 | 88.5 | 116 | 38.706 seconds |
| Black | 7.5 | 81 | 117 | 41.535 seconds |

The report is `results/katago_smoke.json`. Both games replayed successfully
without launching KataGo. There were no engine failures, resignations,
truncations, or cleanup warnings; the existing self-play checkpoint and run
file retained their SHA-256 hashes. These two games verify the integration
and record a comparison with this particular historical model and budget;
they do not establish BetaGo's human rank.
