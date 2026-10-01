param(
    [ValidateRange(2, 100000)][int]$Games = 20,
    [ValidateRange(1, 100000)][int]$Epochs = 20,
    [ValidateRange(0, 1000000)][int]$Iterations = 10,
    [string]$RunDirectory = '',
    [string]$TeacherProfile = 'profiles/katago.json',
    [string]$SgfDirectory = '',
    [string]$Corpus = '',
    [string]$Checkpoint = '',
    [ValidateRange(1, 19)][int]$Size = 9,
    [double]$Komi = 7.5,
    [long]$Seed = 0,
    [ValidateRange(1, 100000)][int]$BatchSize = 32,
    [double]$LearningRate = 0.01,
    [switch]$FromScratch,
    [switch]$NoWatch
)
$ErrorActionPreference = 'Stop'
function Resolve-TaskPath([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $PSScriptRoot $Path }
    return [IO.Path]::GetFullPath($Path)
}
function Open-TaskViewer([string]$FeedPath) {
    return Start-Process -FilePath (Join-Path $PSScriptRoot 'build/play.exe') `
        -ArgumentList @('--watch-training', ('"' + $FeedPath + '"')) `
        -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru
}
function Close-TaskViewer($Process) {
    if ($Process -and -not $Process.HasExited) {
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(2000)) {
            # Only this shortcut's observation helper is stopped; training continues.
            Stop-Process -InputObject $Process -ErrorAction SilentlyContinue
            $null = $Process.WaitForExit(2000)
        }
    }
}
Push-Location $PSScriptRoot
try {
    if ($SgfDirectory -and $Corpus) { throw 'Choose -SgfDirectory or -Corpus.' }
    if ($Checkpoint -and $FromScratch) { throw 'Choose -Checkpoint or -FromScratch.' }
    if (-not $NoWatch -and $Size -ne 9) { throw 'The wooden-board viewer supports 9x9. Use -NoWatch for another size.' }
    if ([double]::IsNaN($Komi) -or [double]::IsInfinity($Komi) -or
        [double]::IsNaN($LearningRate) -or [double]::IsInfinity($LearningRate) -or $LearningRate -le 0) {
        throw 'Komi must be finite and LearningRate must be finite and positive.'
    }
    $taskPretrainer = Join-Path $PSScriptRoot 'build/pretrain.exe'
    $taskSelfplay = Join-Path $PSScriptRoot 'build/selfplay.exe'
    $taskRequired = @($taskPretrainer)
    if ($Iterations -gt 0) { $taskRequired += $taskSelfplay }
    if (-not $NoWatch) { $taskRequired += Join-Path $PSScriptRoot 'build/play.exe' }
    foreach ($taskBinary in $taskRequired) {
        if (-not (Test-Path -LiteralPath $taskBinary -PathType Leaf)) { throw 'Build first with .\build.ps1.' }
    }
    if (-not $RunDirectory) {
        $RunDirectory = 'results/expert-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6)
    }
    $taskRunPath = Resolve-TaskPath $RunDirectory
    if (Test-Path -LiteralPath $taskRunPath) { throw 'Choose a new -RunDirectory; existing training is preserved.' }
    if ($Corpus) {
        $taskCorpusPath = Resolve-TaskPath $Corpus
        if (-not (Test-Path -LiteralPath $taskCorpusPath -PathType Leaf)) { throw 'Corpus file does not exist.' }
    } elseif ($SgfDirectory) {
        $taskSgfPath = Resolve-TaskPath $SgfDirectory
        if (-not (Test-Path -LiteralPath $taskSgfPath)) { throw 'SGF input does not exist.' }
    } else {
        $taskProfilePath = Resolve-TaskPath $TeacherProfile
        if (-not (Test-Path -LiteralPath $taskProfilePath -PathType Leaf)) { throw 'Teacher profile does not exist. Run .\setup-katago.ps1 first.' }
    }
    if (-not $FromScratch -and -not $Checkpoint -and (Test-Path -LiteralPath 'results/selfplay/best.json' -PathType Leaf)) {
        $Checkpoint = 'results/selfplay/best.json'
    }
    if ($Checkpoint) {
        $taskCheckpointSource = Resolve-TaskPath $Checkpoint
        if (-not (Test-Path -LiteralPath $taskCheckpointSource -PathType Leaf)) { throw 'Checkpoint does not exist.' }
        $taskSourceModel = Get-Content -LiteralPath $taskCheckpointSource -Raw | ConvertFrom-Json
        if ($taskSourceModel.settings.board_size -ne $Size) { throw 'Checkpoint board size differs. Use a matching checkpoint or -FromScratch.' }
    }
    New-Item -ItemType Directory -Path $taskRunPath | Out-Null
    $taskCheckpointArgs = @()
    if ($Checkpoint) {
        $taskFrozenCheckpoint = Join-Path $taskRunPath 'initial.json'
        Copy-Item -LiteralPath $taskCheckpointSource -Destination $taskFrozenCheckpoint
        $taskCheckpointArgs = @('--checkpoint', $taskFrozenCheckpoint)
        Write-Host "Pretraining a frozen copy of $taskCheckpointSource."
    } else { Write-Host 'Initializing a new network for supervised pretraining.' }
    $taskLivePath = Join-Path $taskRunPath 'live.json'
    $taskLiveArgs = @('--live', '--live-file', $taskLivePath)
    $taskViewerProcess = $null
    if (-not $NoWatch) {
        $taskViewerProcess = Open-TaskViewer $taskLivePath
        Write-Host "Live board opened (viewer PID $($taskViewerProcess.Id))."
    }
    $taskSeedText = $Seed.ToString([Globalization.CultureInfo]::InvariantCulture)
    if (-not $Corpus) {
        $taskDataDirectory = Join-Path $taskRunPath 'data'
        if ($SgfDirectory) {
            Write-Host "Importing SGF main lines from $taskSgfPath."
            & $taskPretrainer --import-sgf $taskSgfPath --size $Size --output $taskDataDirectory @taskLiveArgs
        } else {
            Write-Host "Generating $Games teacher games. Each move uses the external engine; this stage takes most of the time."
            $taskKomiText = $Komi.ToString([Globalization.CultureInfo]::InvariantCulture)
            & $taskPretrainer --teacher-profile $taskProfilePath --games $Games --size $Size --komi $taskKomiText `
                --seed $taskSeedText --output $taskDataDirectory @taskLiveArgs
        }
        $taskDataStatus = $LASTEXITCODE
        if ($taskDataStatus -ne 0) {
            Write-Host "Corpus retained at $taskDataDirectory/corpus.json. Review excluded attempts/files before training accepted games."
            exit $taskDataStatus
        }
        $taskCorpusPath = Join-Path $taskDataDirectory 'corpus.json'
    }
    $taskPretrainedPath = Join-Path $taskRunPath 'pretrained'
    $taskLearningRateText = $LearningRate.ToString([Globalization.CultureInfo]::InvariantCulture)
    & $taskPretrainer --train $taskCorpusPath --size $Size --epochs $Epochs --batch-size $BatchSize `
        --learning-rate $taskLearningRateText --seed $taskSeedText --output $taskPretrainedPath @taskCheckpointArgs @taskLiveArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $taskSelectedModel = Join-Path $taskPretrainedPath 'model.json'
    $taskSelfplayPath = Join-Path $taskRunPath 'selfplay'
    Write-Host "Pretrained model: $taskSelectedModel"
    # Imported SGF may use different komi; continue self-play under that same komi.
    $taskCorpusData = Get-Content -LiteralPath $taskCorpusPath -Raw | ConvertFrom-Json
    $taskTrainingKomi = ([double]$taskCorpusData.games[0].initial_state.komi).ToString([Globalization.CultureInfo]::InvariantCulture)
    if ($Iterations -eq 0) {
        $taskHeadlessOption = if ($Size -ne 9) { ' -NoWatch' } else { '' }
        Write-Host "To continue: .\train.ps1 -RunDirectory `"$taskSelfplayPath`" -Checkpoint `"$taskSelectedModel`" -Komi $taskTrainingKomi -Iterations 10$taskHeadlessOption"
        exit 0
    }
    Close-TaskViewer $taskViewerProcess
    if (-not $NoWatch) {
        $taskViewerProcess = Open-TaskViewer (Join-Path $taskSelfplayPath 'live.json')
        Write-Host "Self-play board opened (viewer PID $($taskViewerProcess.Id))."
    }
    Write-Host "Continuing with $Iterations self-play iteration(s) in $taskSelfplayPath."
    & $taskSelfplay --output $taskSelfplayPath --checkpoint $taskSelectedModel --size $Size --komi $taskTrainingKomi `
        --seed $taskSeedText --iterations $Iterations --live
    $taskStatus = $LASTEXITCODE
    if ($taskStatus -eq 0) {
        Write-Host "Accepted self-play model: $taskSelfplayPath/best.json"
        Write-Host "Resume later: .\train.ps1 -RunDirectory `"$taskSelfplayPath`" -Iterations 10"
    }
    exit $taskStatus
} finally { Pop-Location }
