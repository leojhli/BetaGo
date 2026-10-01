param(
    [ValidateRange(1, 1000000)][int]$Iterations = 10,
    [string]$RunDirectory = 'results/selfplay',
    [string]$Checkpoint = '',
    [switch]$NoWatch
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $taskTrainer = Join-Path $PSScriptRoot 'build/selfplay.exe'
    $taskViewer = Join-Path $PSScriptRoot 'build/play.exe'
    $taskRequiredBinaries = @($taskTrainer)
    if (-not $NoWatch) { $taskRequiredBinaries += $taskViewer }
    foreach ($taskBinary in $taskRequiredBinaries) {
        if (-not (Test-Path -LiteralPath $taskBinary -PathType Leaf)) {
            throw 'Build the project first with .\build.ps1.'
        }
    }
    if ([string]::IsNullOrWhiteSpace($RunDirectory)) { throw 'RunDirectory cannot be empty.' }
    if (-not [IO.Path]::IsPathRooted($RunDirectory)) { $RunDirectory = Join-Path $PSScriptRoot $RunDirectory }
    $taskRunPath = [IO.Path]::GetFullPath($RunDirectory)
    $taskManifestPath = Join-Path $taskRunPath 'run.json'
    if (Test-Path -LiteralPath $taskManifestPath -PathType Leaf) {
        if ($Checkpoint) { throw 'A resumed run uses its saved model. Omit -Checkpoint or choose a new -RunDirectory.' }
        $taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw | ConvertFrom-Json
        if ($taskManifest.kind -ne 'self_play_training_run' -or $taskManifest.schema_version -ne 1) {
            throw 'RunDirectory does not contain a supported training run.'
        }
        if (-not $NoWatch -and $taskManifest.settings.self_play.board_size -ne 9) {
            throw 'The wooden-board viewer supports 9x9. Use -NoWatch for another board size.'
        }
        $taskTrainingArgs = @('--resume', $taskRunPath)
        Write-Host "Resuming iteration $($taskManifest.next_iteration) with all saved training settings."
    } else {
        if (Test-Path -LiteralPath $taskRunPath) { throw 'Choose a new run directory or a directory containing run.json.' }
        $taskTrainingArgs = @('--output', $taskRunPath)
        if ($Checkpoint) {
            if (-not [IO.Path]::IsPathRooted($Checkpoint)) { $Checkpoint = Join-Path $PSScriptRoot $Checkpoint }
            $taskCheckpointPath = [IO.Path]::GetFullPath($Checkpoint)
            if (-not (Test-Path -LiteralPath $taskCheckpointPath -PathType Leaf)) { throw 'Checkpoint file does not exist.' }
            if (-not $NoWatch) {
                $taskModel = Get-Content -LiteralPath $taskCheckpointPath -Raw | ConvertFrom-Json
                if ($taskModel.settings.board_size -ne 9) { throw 'The wooden-board viewer requires a 9x9 checkpoint.' }
            }
            $taskTrainingArgs += @('--checkpoint', $taskCheckpointPath)
        }
        Write-Host "Starting a new training run in $taskRunPath."
    }
    $taskTrainingArgs += @('--iterations', [string]$Iterations, '--live')
    if (-not $NoWatch) {
        $taskLivePath = Join-Path $taskRunPath 'live.json'
        # Native argv is passed directly; quoting preserves the spaces in paths.
        # Hide the helper console. Tk maps its separate board window normally.
        $taskViewerProcess = Start-Process -FilePath $taskViewer `
            -ArgumentList @('--watch-training', ('"' + $taskLivePath + '"')) `
            -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru
        Write-Host "Live board opened (viewer PID $($taskViewerProcess.Id)). Pause view freezes only the display."
    }
    Write-Host "Training $Iterations iteration(s). Ctrl+C stops training; completed iterations can be resumed."
    & $taskTrainer @taskTrainingArgs
    $taskTrainingStatus = $LASTEXITCODE
    if ($taskTrainingStatus -eq 0) { Write-Host "Training finished. Accepted model: $(Join-Path $taskRunPath 'best.json')" }
    exit $taskTrainingStatus
} finally {
    Pop-Location
}
