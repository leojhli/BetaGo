# Portable, project-local KataGo dependencies. No drivers or system installation.
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This local KataGo setup requires Windows.'
}

$taskInstallRoot = Join-Path $PSScriptRoot '.tools/katago'
$taskEngineDirectory = Join-Path $taskInstallRoot 'v1.18.1-eigen'
$taskArchive = Join-Path $taskInstallRoot 'katago-v1.18.1-eigen-windows-x64.zip'
$taskExecutable = Join-Path $taskEngineDirectory 'katago.exe'
$taskModel = Join-Path $taskInstallRoot 'kata1-b6c96-s175395328-d26788732.txt.gz'
$taskReleaseUrl = 'https://github.com/lightvector/KataGo/releases/download/v1.18.1/katago-v1.18.1-eigen-windows-x64.zip'
$taskModelUrl = 'https://media.katagotraining.org/uploaded/networks/models/kata1/kata1-b6c96-s175395328-d26788732.txt.gz'
# Archive hash published by the official GitHub release. Executable/model hashes
# pin the exact official files used in the verified local smoke experiment.
$taskArchiveHash = '074485cf150c38aa3bb14ac9f54f2952ffefbceb44673709bbb8a83650bf95d6'
$taskExecutableHash = '0c162587d3cda7369b3f521ed3270382f76550c4035799e70f68318dc0620270'
$taskModelHash = '48d6754de3c4754f95bf6a5ca40957a49e5e915aaaeede133a17b9ccf8fa5fcb'

function Get-VerifiedKataGoFile {
    param([string]$Uri, [string]$Destination, [string]$ExpectedHash)
    if (Test-Path -LiteralPath $Destination) {
        if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
            throw "Existing file has an unexpected checksum: $Destination. It was left unchanged."
        }
        return
    }
    $taskPartialPath = $Destination + '.partial'
    Write-Host "Downloading $([IO.Path]::GetFileName($Destination))..."
    Invoke-WebRequest -Uri $Uri -OutFile $taskPartialPath -UseBasicParsing
    if ((Get-FileHash -LiteralPath $taskPartialPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
        throw "Download checksum mismatch: $taskPartialPath"
    }
    Move-Item -LiteralPath $taskPartialPath -Destination $Destination
}

New-Item -ItemType Directory -Force -Path $taskInstallRoot | Out-Null
Get-VerifiedKataGoFile -Uri $taskReleaseUrl -Destination $taskArchive -ExpectedHash $taskArchiveHash
if (-not (Test-Path -LiteralPath $taskExecutable)) {
    Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskEngineDirectory -Force
}
if ((Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskExecutableHash) {
    throw 'The existing KataGo executable has an unexpected checksum. It was left unchanged.'
}
Get-VerifiedKataGoFile -Uri $taskModelUrl -Destination $taskModel -ExpectedHash $taskModelHash

foreach ($taskLicense in @(
    @{ Uri = 'https://raw.githubusercontent.com/lightvector/KataGo/v1.18.1/LICENSE'; Name = 'LICENSE' },
    @{ Uri = 'https://katagotraining.org/network_license/'; Name = 'network_license.html' }
)) {
    $taskLicensePath = Join-Path $taskInstallRoot $taskLicense.Name
    if (-not (Test-Path -LiteralPath $taskLicensePath)) {
        Invoke-WebRequest -Uri $taskLicense.Uri -OutFile $taskLicensePath -UseBasicParsing
    }
}

& $taskExecutable version
if ($LASTEXITCODE -ne 0) { throw "KataGo could not run (status $LASTEXITCODE)." }
@{
    checked_utc = [DateTime]::UtcNow.ToString('o')
    version = 'v1.18.1'
    backend = 'Eigen CPU, Windows x64'
    release_url = $taskReleaseUrl
    archive_sha256 = $taskArchiveHash
    executable_sha256 = $taskExecutableHash
    model_url = $taskModelUrl
    model_sha256 = $taskModelHash
    profile = 'profiles/katago.json'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskInstallRoot 'setup.json') -Encoding UTF8
Write-Host 'KataGo is ready. Run .\match-katago.ps1 to evaluate the current BetaGo checkpoint.'
