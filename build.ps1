param(
    [string]$TkRoot = '',
    [switch]$DebugBuild,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $taskTools = Join-Path $PSScriptRoot '.tools'
    $taskCompiler = Join-Path $taskTools 'zig-x86_64-windows-0.14.1/zig.exe'
    if (-not (Test-Path -LiteralPath $taskCompiler)) {
        New-Item -ItemType Directory -Force -Path $taskTools | Out-Null
        $taskArchive = Join-Path $taskTools 'zig.zip'
        Write-Host 'Downloading the portable C++ compiler into .tools (no system installation)...'
        Invoke-WebRequest 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' -OutFile $taskArchive
        $taskExpectedHash = '554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c'
        if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskExpectedHash) {
            throw 'Compiler download checksum mismatch.'
        }
        Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskTools -Force
    }
    New-Item -ItemType Directory -Force -Path 'build', '.tools/cache/local', '.tools/cache/global' | Out-Null
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskTools 'cache/local'
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskTools 'cache/global'
    $taskFlags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Iinclude', '-Ithird_party', '-Ithird_party/tcl', '-Ibuild/generated')
    if ($DebugBuild) { $taskFlags += @('-O0', '-g') } else { $taskFlags += '-O3' }
    $taskFlagSignature = $taskFlags -join ' '
    $taskStamp = 'build/compiler-flags.txt'
    $taskFlagsChanged = -not (Test-Path $taskStamp) -or ((Get-Content $taskStamp -Raw).Trim() -ne $taskFlagSignature)
    # Identify the compiled source snapshot, including code not committed yet.
    $taskGitAvailable = 'false'; $taskGitDirty = 'false'; $taskRevision = ''
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $taskGitRevision = & git rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0) {
            $taskRevision = ($taskGitRevision -join '').Trim(); $taskGitAvailable = 'true'
            $taskGitStatus = & git status --porcelain --untracked-files=normal 2>$null
            if ($LASTEXITCODE -eq 0) {
                if ($taskGitStatus) { $taskGitDirty = 'true' }
            } else { $taskGitAvailable = 'false' }
        }
    }
    $taskSourcePaths = @('build.ps1', 'CMakeLists.txt', 'cmake/build_info.hpp.in')
    $taskSourcePaths += Get-ChildItem 'include', 'src', 'tests' -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.hpp') } |
        ForEach-Object { $_.FullName.Substring($PSScriptRoot.Length + 1).Replace('\', '/') }
    [Array]::Sort($taskSourcePaths, [StringComparer]::Ordinal)
    $taskFingerprintLines = foreach ($taskSourcePath in $taskSourcePaths) {
        $taskFileHash = (Get-FileHash -LiteralPath $taskSourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        "${taskSourcePath}:$taskFileHash"
    }
    $taskHasher = [Security.Cryptography.SHA256]::Create()
    try {
        $taskFingerprintBytes = [Text.Encoding]::UTF8.GetBytes(($taskFingerprintLines -join "`n") + "`n")
        $taskSourceDigest = [BitConverter]::ToString($taskHasher.ComputeHash($taskFingerprintBytes)).Replace('-', '').ToLowerInvariant()
    } finally { $taskHasher.Dispose() }
    $taskBuildSignature = "$taskRevision|$taskGitAvailable|$taskGitDirty|$taskFlagSignature|$taskSourceDigest"
    $taskInfoHeader = 'build/generated/build_info.hpp'; $taskInfoStamp = 'build/build-info-signature.txt'
    if (-not (Test-Path $taskInfoHeader) -or -not (Test-Path $taskInfoStamp) -or
        (Get-Content $taskInfoStamp -Raw).Trim() -ne $taskBuildSignature) {
        $taskInfoText = Get-Content -LiteralPath 'cmake/build_info.hpp.in' -Raw
        $taskInfoValues = @{
            BETAGO_GIT_AVAILABLE = $taskGitAvailable; BETAGO_GIT_DIRTY = $taskGitDirty
            BETAGO_GIT_REVISION = $taskRevision; BETAGO_SOURCE_SHA256 = $taskSourceDigest
            BETAGO_BUILD_TIME = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
            BETAGO_COMPILER = "Zig $(& $taskCompiler version) / Clang C++"
            BETAGO_BUILD_FLAGS = $taskFlagSignature
            BETAGO_BUILD_PROFILE = $(if ($DebugBuild) { 'Debug' } else { 'Release' })
        }
        foreach ($taskInfoKey in $taskInfoValues.Keys) {
            $taskInfoText = $taskInfoText.Replace("@$taskInfoKey@", [string]$taskInfoValues[$taskInfoKey])
        }
        New-Item -ItemType Directory -Force -Path 'build/generated' | Out-Null
        Set-Content -LiteralPath $taskInfoHeader -Value $taskInfoText -Encoding UTF8
        Set-Content -LiteralPath $taskInfoStamp -Value $taskBuildSignature
    }
    $taskHeaderTime = (Get-ChildItem 'include', 'third_party' -Recurse -File | Where-Object { $_.Extension -in @('.h', '.hpp') } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
    $taskObjects = @{}
    foreach ($taskName in @('state', 'random', 'mcts', 'runner', 'arena', 'features', 'dataset', 'network', 'tk', 'window', 'runner_main', 'play_main', 'network_main', 'test_main', 'test_mcts', 'test_arena', 'test_neural')) {
        $taskSource = if ($taskName -in @('test_main', 'test_mcts', 'test_arena', 'test_neural')) { "tests/$taskName.cpp" } else { "src/$taskName.cpp" }
        $taskObject = "build/$taskName.o"
        if (-not $taskFlagsChanged -and (Test-Path $taskObject)) {
            $taskObjectTime = (Get-Item $taskObject).LastWriteTimeUtc
            $taskInfoCurrent = $taskName -notin @('runner_main', 'network_main') -or $taskObjectTime -gt (Get-Item $taskInfoHeader).LastWriteTimeUtc
            if ($taskObjectTime -gt (Get-Item $taskSource).LastWriteTimeUtc -and $taskObjectTime -gt $taskHeaderTime -and $taskInfoCurrent) {
                $taskObjects[$taskName] = $taskObject
                continue
            }
        }
        Write-Host "Compiling $taskSource"
        & $taskCompiler c++ @taskFlags -c $taskSource -o $taskObject
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $taskSource" }
        $taskObjects[$taskName] = $taskObject
    }
    Set-Content -LiteralPath $taskStamp -Value $taskFlagSignature
    $taskCore = @($taskObjects.state, $taskObjects.random, $taskObjects.mcts, $taskObjects.runner)
    $taskNeural = @($taskObjects.features, $taskObjects.dataset, $taskObjects.network)
    foreach ($taskTarget in @('runner', 'play', 'network', 'tests')) {
        $taskTargetObjects = switch ($taskTarget) {
            'runner' { $taskCore + @($taskObjects.arena, $taskObjects.runner_main) }
            'play' { $taskCore + @($taskObjects.tk, $taskObjects.window, $taskObjects.play_main) }
            'network' { $taskCore + $taskNeural + @($taskObjects.network_main) }
            'tests' { $taskCore + $taskNeural + @($taskObjects.arena, $taskObjects.test_main, $taskObjects.test_mcts, $taskObjects.test_arena, $taskObjects.test_neural) }
        }
        Write-Host "Linking build/$taskTarget.exe"
        & $taskCompiler c++ @taskTargetObjects -o "build/$taskTarget.exe"
        if ($LASTEXITCODE -ne 0) { throw "Linking failed: $taskTarget" }
    }
    $taskRuntime = Join-Path $PSScriptRoot 'build/runtime'
    if ($TkRoot -or -not (Test-Path "$taskRuntime/tcl8.6/init.tcl") -or -not (Test-Path "$taskRuntime/bundled.txt")) {
        if (-not $TkRoot) {
            $taskCachedTk = Join-Path $taskTools 'tk'
            if (Test-Path "$taskCachedTk/lib/tcl8.6/init.tcl") {
                $TkRoot = $taskCachedTk
            } else {
                $taskTcl = Get-Command tclsh86t.exe -ErrorAction SilentlyContinue
                if ($taskTcl) { $TkRoot = Split-Path -Parent (Split-Path -Parent $taskTcl.Source) }
            }
        }
        if (-not $TkRoot) { throw 'Provide -TkRoot with a 64-bit Tcl/Tk 8.6 installation.' }
        $taskDllDirectory = Join-Path $TkRoot 'bin'
        $taskLibraryDirectory = Join-Path $TkRoot 'lib'
        if (-not $TkRoot -or -not (Test-Path "$taskLibraryDirectory/tcl8.6/init.tcl") -or
            -not (Test-Path "$taskDllDirectory/tcl86t.dll") -or -not (Test-Path "$taskDllDirectory/tk86t.dll")) {
            throw 'Provide -TkRoot with a 64-bit Tcl/Tk 8.6 installation containing bin DLLs and lib scripts.'
        }
        New-Item -ItemType Directory -Force -Path $taskRuntime | Out-Null
        foreach ($taskDll in @('tcl86t.dll', 'tk86t.dll')) {
            Copy-Item -LiteralPath (Join-Path $taskDllDirectory $taskDll) -Destination $taskRuntime -Force
        }
        # The original Tcl DLL also imports zlib; include that dependency.
        $taskZlib = Join-Path $taskDllDirectory 'zlib1.dll'
        if (Test-Path -LiteralPath $taskZlib) {
            Copy-Item -LiteralPath $taskZlib -Destination $taskRuntime -Force
        }
        foreach ($taskLibrary in @('tcl8.6', 'tk8.6', 'tcl8')) {
            $taskLibraryPath = Join-Path $taskLibraryDirectory $taskLibrary
            if (Test-Path -LiteralPath $taskLibraryPath) {
                Copy-Item -LiteralPath $taskLibraryPath -Destination $taskRuntime -Recurse -Force
            }
        }
        Copy-Item -LiteralPath 'third_party/tcl/license.terms' -Destination "$taskRuntime/TCL-LICENSE.txt" -Force
        Copy-Item -LiteralPath 'third_party/tcl/TK-LICENSE.txt' -Destination $taskRuntime -Force
        Set-Content -LiteralPath "$taskRuntime/bundled.txt" -Value 'Tk 8.6 runtime bundled by build.ps1'
    }
    New-Item -ItemType Directory -Force -Path 'build/licenses' | Out-Null
    Copy-Item -LiteralPath 'third_party/nlohmann/LICENSE.MIT', 'third_party/MT19937-LICENSE.txt', 'third_party/ZLIB-LICENSE.txt', '.tools/zig-x86_64-windows-0.14.1/LICENSE' -Destination 'build/licenses' -Force
    foreach ($taskCppLibrary in @('libcxx', 'libcxxabi', 'libunwind')) {
        Copy-Item -LiteralPath ".tools/zig-x86_64-windows-0.14.1/lib/$taskCppLibrary/LICENSE.TXT" -Destination "build/licenses/$taskCppLibrary-LICENSE.txt" -Force
    }
    Copy-Item -LiteralPath '.tools/zig-x86_64-windows-0.14.1/lib/libc/mingw/COPYING' -Destination 'build/licenses/mingw-COPYING.txt' -Force
    Write-Host 'Built: build/play.exe, build/runner.exe, build/network.exe, build/tests.exe'
    if ($Test) {
        & './build/tests.exe' --oracle tests/fixtures/migration.json
        if ($LASTEXITCODE -ne 0) { throw 'C++ tests failed.' }
        & './build/play.exe' --self-test
        if ($LASTEXITCODE -ne 0) { throw 'Visual board tests failed.' }
    }
} finally {
    Pop-Location
}
