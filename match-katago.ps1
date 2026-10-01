param(
    [ValidateRange(1, 100000)][int]$Pairs = 1,
    [ValidateRange(1, 1000000)][int]$Simulations = 64,
    [ValidateRange(1, 1000000)][int]$MaxMoves = 400,
    [ValidateRange(0, 2147483647)][int]$Seed = 0,
    [string]$Checkpoint = 'results/selfplay/best.json',
    [string]$Output = 'results/katago_smoke.json'
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    foreach ($taskInput in @('build/runner.exe', 'profiles/katago.json',
        '.tools/katago/v1.18.1-eigen/katago.exe',
        '.tools/katago/kata1-b6c96-s175395328-d26788732.txt.gz', $Checkpoint)) {
        if (-not (Test-Path -LiteralPath $taskInput -PathType Leaf)) {
            throw "Required match input is missing: $taskInput. See docs/external_gtp.md."
        }
    }
    Write-Host "Playing $Pairs color-swapped pair(s) against the local KataGo CPU profile..."
    & './build/runner.exe' --arena `
        --agent-a neural-mcts --a-checkpoint $Checkpoint --a-simulations $Simulations `
        --agent-b external-gtp --b-gtp-profile 'profiles/katago.json' `
        --size 9 --komi 7.5 --pairs $Pairs --max-moves $MaxMoves `
        --seed $Seed --output $Output
    $taskMatchStatus = $LASTEXITCODE
    if ($taskMatchStatus -eq 0 -or $taskMatchStatus -eq 2) {
        & './build/runner.exe' --replay $Output
        if ($LASTEXITCODE -ne 0) { throw "Saved match replay validation failed: $Output" }
    }
    exit $taskMatchStatus
} finally {
    Pop-Location
}
