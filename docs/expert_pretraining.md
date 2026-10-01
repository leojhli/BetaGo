# Expert game pretraining

BetaGo can now learn an initial policy and value function from recorded games,
then continue its existing neural MCTS self-play loop. All parsing, target
generation and network training run in C++20. The wooden board and search/rules
implementations remain shared with the existing application.

## Start the complete workflow

From the project directory, after `./build.ps1`:

```powershell
.\pretrain.ps1 -Games 20 -Epochs 20 -Iterations 10
```

The shortcut generates 20 **teacher self-play games** using the installed
`profiles/katago.json`, pretrains for 20 passes over the training positions, and
starts 10 neural MCTS self-play iterations. These are engine games, not human
professional records. Generation uses the profile's current CPU search budget
and is normally the longest stage. Each game starts a fresh hidden KataGo
process; that process plays both colors without duplicate `play` commands.

If `results/selfplay/best.json` exists, the shortcut freezes a copy into the new
experiment before generating any games. Otherwise it initializes a fresh
network. Use `-Checkpoint PATH` for another starting model or `-FromScratch` to
start fresh. The source checkpoint and existing self-play run are preserved.
Every invocation creates a new timestamped `results/expert-*` directory, unless
you supply a new `-RunDirectory`. Existing directories are rejected.

The same wooden board shows the teacher's actual accepted moves. During
supervised updates it shows the epoch count beside a fixed source position;
there are no new played moves during gradient updates. The viewer switches to
the new self-play run for its actual games and candidate evaluation. Closing a
viewer or pausing its display does not stop training. `-NoWatch` runs headless.
The viewer supports 9x9; other sizes require `-NoWatch`.

`-Iterations 0` stops after pretraining and prints a full continuation command
with `-Checkpoint` and the imported/generated komi (`-NoWatch` for another board
size). Use that full command to start self-play from the pretrained model.
After self-play has created `selfplay/run.json`, resume its saved model/settings
with its printed directory:

```powershell
.\train.ps1 -RunDirectory results/expert-EXPERIMENT/selfplay -Iterations 10
```

Interrupting a teacher run retains all finalized attempts in `data/corpus.json`.
An interrupted pretraining run retains its selected checkpoint and epoch report
from the last completed epoch. Teacher generation and supervised pretraining do
not have resume commands in this version: reuse the saved corpus in a new
experiment with `-Corpus PATH`. The self-play stage uses the existing resumable
iteration workflow.

## Use real games

Supply your own SGF files; the importer does not download or
verify whether their players are professionals.

```powershell
.\pretrain.ps1 -SgfDirectory games/9x9 -Epochs 20 -Iterations 10
```

Most professional Go games are 19x19. They cannot train a 9x9 model by cropping
the board. For a separate 19x19 experiment:

```powershell
.\pretrain.ps1 -SgfDirectory games/19x19 -Size 19 -FromScratch -NoWatch -Iterations 0
```

The importer supports SGF FF4 Go collections and selects the first/main
variation. It parses escaping and checks ignored branches' syntax, accepts
empty move values as pass and legacy `tt` pass for sizes through 19. SGF points
use column then row from the upper left (`ba` is row 0, column 1), distinct from
GTP's lower-left numbered coordinates. See the
[SGF FF4 specification](https://www.red-bean.com/sgf/sgf4.html).

Text encodings supported are UTF-8 and ISO-8859-1 (the SGF default when `CA` is
absent); other declared encodings are excluded with a reason.

Each record needs explicit square `SZ`, finite `KM` and a known `RE`: a positive
numeric Black/White victory, a winner without a margin (`B+`/`W+`), resignation,
or draw. Missing/unknown outcomes,
time wins, forfeits, setup stones, handicap and turn alterations are excluded.
The selected line must alternate colors and every move must be legal under
BetaGo's simple ko and forbidden suicide. An invalid collection excludes its
whole file, with a reason saved in `rejected_files`. Imported game values use
the **reported outcome**; BetaGo does not reinterpret Japanese territory scores
as its own raw area score. Original rule/player/result metadata and source
fingerprints are retained. A corpus must contain one board size and one komi.

## What the network learns

Every pre-action position retains its previous board, so ko legality remains
correct. Its policy target is a one-hot label for the recorded next move; its
value target is the final winner from the player-to-move perspective, or zero
for a draw. Teacher targets use BetaGo's local score after two passes. Failed,
resigned or capped teacher games retain evidence and legal prefixes but provide
no training labels. Generation continues without retries; an incomplete corpus
returns exit status 2 and the shortcut stops for review.

Duplicate whole games are filtered before a seeded game-level split. Conflicting
outcomes for the same game are rejected. At least two unique games are required;
the default validation fraction is 20%, rounded up with at least one game in
each partition. Eight rotations/reflections augment only the training partition;
held-out positions use the original orientation. Policy/pass targets, all
feature planes, prior-board history and legality transform together.

The trainer saves both the final epoch and the checkpoint with the lowest
held-out policy cross-entropy plus value mean squared error. Selection includes
the starting checkpoint, so a harmful update need not replace it. Lower loss or
higher move agreement measures imitation on held-out records; neither is a Go
rank or proof of stronger play. A few teacher games are an integration smoke
test, not a sufficiently broad training corpus. Improving playing strength must
be checked separately with the arena at fixed settings.

## Native commands and artifacts

```powershell
.\build\pretrain.exe --teacher-profile profiles/katago.json --games 20 --seed 0 --output results/teacher --live
.\build\pretrain.exe --import-sgf games/9x9 --size 9 --output results/imported
.\build\pretrain.exe --train results/teacher/corpus.json --checkpoint results/selfplay/best.json --epochs 20 --output results/pretrained --live
.\train.ps1 -RunDirectory results/pretrained_selfplay -Checkpoint results/pretrained/model.json -Iterations 10
```

Use a new output directory for each command. `--live-file PATH` with `--live`
allows the shortcut's viewer to follow both preparation stages. `--no-augment`
disables symmetry expansion. `--learning-rate`, `--momentum`, `--l2`,
`--batch-size`, `--validation-fraction` and `--seed` configure pretraining;
`--channels` and `--value-hidden` apply only to a new network. A checkpoint
supplies its architecture and optimizer history. See `pretrain.exe --help`.

| Artifact | Meaning |
| --- | --- |
| `initial.json` | Frozen source checkpoint, when warm-starting the shortcut |
| `data/corpus.json` | Games, provenance, per-attempt evidence or SGF rejections |
| `data/game-*.sgf` | Completed generated teacher games, exportable/importable |
| `pretrained/train.json`, `validation.json` | Labelled positions for the existing network trainer |
| `pretrained/model.json` | Best held-out checkpoint, including optimizer state |
| `pretrained/final.json` | Last epoch checkpoint |
| `pretrained/report.json` | Split identities/counts, deduplication, metrics and settings |
| `selfplay/run.json`, `best.json` | Existing resumable self-play and accepted model |

Corpus and epoch reports save atomically. The report records input identities,
selected epoch, initialized checkpoint, resolved model/optimizer settings,
timing and compiled build/machine information. Teacher session records include
executable/config/model fingerprints, resolved arguments and seeds, engine
name/version and effective rules. Generic GTP rules remain unverified; the
KataGo adapter verifies its supported settings. Seed changes do not guarantee
game diversity or calibrated confidence. No kyu/dan label is produced.

For an external playing-strength comparison:

```powershell
.\build\runner.exe --arena --agent-a neural-mcts --a-checkpoint results/pretrained/model.json --a-simulations 64 --agent-b external-gtp --b-gtp-profile profiles/katago.json --size 9 --komi 7.5 --pairs 5 --max-moves 400 --seed 0 --output results/pretrained_vs_katago.json
```

Compare a frozen baseline at the same settings in a separate output. This is a
configuration-specific 9x9 comparison; teacher imitation and reference labels
do not establish a human rank.

## Validation

`./build.ps1 -Test` includes SGF syntax/coordinate/history checks, fake GTP
teacher process and failure tests, game-level split and augmentation checks,
checkpoint compatibility, existing native tests and wooden-board GUI checks.
Automated tests do not require KataGo. CMake includes the same new sources and
tests; its existing Tk runtime requirement applies to GUI checks on Windows.

The local Windows smoke check generated two KataGo v1.18.1/b6c96 games with
32-visit CPU search (87 and 63 moves, both completed), roundtripped their SGF
exports, and pretrained a frozen checkpoint for three epochs. Selection kept
epoch two: held-out CE + MSE decreased from 5.324 to 4.141 and rose at epoch
three. That model continued through an isolated self-play iteration and won
both games of one color-swapped 64-simulation pair against the frozen baseline.
This tiny deterministic comparison does not establish broader playing strength
or a human rank. The SGF shortcut also completed pretraining and self-play under
imported komi 6.5, switching between its owned viewer processes. Original model
bytes were unchanged. Evidence is retained in
`results/expert_pretraining_smoke/validation.json`; 219 native tests, 15 CLI
integration checks and existing GUI checks passed. CMake wiring was updated,
but CMake was unavailable on this machine.
