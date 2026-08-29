param(
    [switch]$Run,           # build, then launch ArcRaiders
    [switch]$RunOnly,       # launch the existing exe without building
    [switch]$FullRebuild,   # MSBuild /t:Rebuild instead of /t:Build
    [switch]$Test,          # build, then run the test suite (Pillar 1)
    [switch]$Package        # build, then zip Build/ into dist/
)

# Single entry point: clean checkout -> green build + optional tests/package.
# No log redirection. All output goes to the console.
# (Formerly rebuild-run.ps1; refactored under Pillar 2 of docs/aplus-plan.md)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exeName = "ArcRaiders"
$outDir = Join-Path $root "Build"
$exePath = Join-Path $outDir "$exeName.exe"
$testExe = Join-Path $outDir "Tests\ArcRaiders.Tests.exe"
$sln = Join-Path $root "ArcRaiders.sln"
$distDir = Join-Path $root "dist"

function Find-MsBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($p) { return $p }
    }
    $fallback = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    if (Test-Path $fallback) { return $fallback }
    throw "MSBuild not found (checked vswhere and $fallback)"
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-TaskKill {
    param([string]$ImageName)
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & taskkill.exe /F /IM $ImageName /T 2>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $oldEap
    }
}

function Test-ExeUnlocked {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        return $true
    }
    try {
        $fs = [System.IO.File]::Open(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        $fs.Close()
        return $true
    } catch {
        return $false
    }
}

function Stop-ArcRaiders {
    param([int]$MaxWaitSec = 20)

    for ($attempt = 1; $attempt -le 4; $attempt++) {
        $procs = @(Get-Process -Name $exeName -ErrorAction SilentlyContinue)
        if ($procs.Count -eq 0) {
            break
        }

        foreach ($proc in $procs) {
            Write-Host "[*] Stopping $($exeName) (PID $($proc.Id), attempt $attempt)..."
            $oldEap = $ErrorActionPreference
            $ErrorActionPreference = "SilentlyContinue"
            try {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            } finally {
                $ErrorActionPreference = $oldEap
            }
        }

        Invoke-TaskKill -ImageName "$exeName.exe"
        Start-Sleep -Milliseconds 600
    }

    $deadline = (Get-Date).AddSeconds($MaxWaitSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name $exeName -ErrorAction SilentlyContinue)) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if (Get-Process -Name $exeName -ErrorAction SilentlyContinue) {
        return $false
    }

    $unlockDeadline = (Get-Date).AddSeconds($MaxWaitSec)
    while ((Get-Date) -lt $unlockDeadline) {
        if (Test-ExeUnlocked -Path $exePath) {
            Write-Host "[+] $exeName stopped and exe unlocked."
            return $true
        }
        Start-Sleep -Milliseconds 250
    }

    return (Test-ExeUnlocked -Path $exePath)
}

function Ensure-ElevatedIfNeeded {
    if ($env:ARC_REBUILD_ELEVATED -eq "1") {
        return
    }
    if (-not (Get-Process -Name $exeName -ErrorAction SilentlyContinue)) {
        return
    }
    if (Test-IsAdmin) {
        return
    }

    Write-Host "[*] $exeName is running elevated - relaunching this script as admin (silent when UAC is off)..."
    $argList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command",
        "& '$PSCommandPath' $(if ($Run) {'-Run '})$(if ($RunOnly) {'-RunOnly '})$(if ($FullRebuild) {'-FullRebuild '})$(if ($Test) {'-Test '})$(if ($Package) {'-Package '})"
    )
    $proc = Start-Process -FilePath "powershell.exe" `
        -ArgumentList $argList `
        -Verb RunAs `
        -PassThru

    if ($null -eq $proc) {
        Write-Error "Elevated relaunch was cancelled or failed."
    }
    # NOTE: do NOT use Start-Process -Wait here. PS 5.1 -Wait waits on the whole
    # process tree, and the elevated child launches ArcRaiders.exe - so -Wait
    # blocked until the overlay was closed. WaitForExit() waits only on the
    # elevated PowerShell itself.
    $proc.WaitForExit()
    exit $proc.ExitCode
}

Ensure-ElevatedIfNeeded
$env:ARC_REBUILD_ELEVATED = "1"

if (-not $RunOnly) {
    $stopped = Stop-ArcRaiders
    if (-not $stopped) {
        Write-Error "Could not stop $exeName or unlock $exePath - aborting (no alternate output)."
    }

    $msbuild = Find-MsBuild
    $buildTarget = if ($FullRebuild) { "Rebuild" } else { "Build" }
    Write-Host "[*] Building Release|x64 ($buildTarget, single-node)..."
    # /m:1 (single node): the parallel MSBuild file nodes race on FileTracker
    # tlog + .obj writes ("user-mapped section" MSB6003 / C1083 "Invalid
    # argument" on a different obj every run). Serializing the build makes it
    # deterministic.
    # TrackFileAccess=false avoids the intermittent FileTracker
    # "user-mapped section" tlog lock failures.
    & $msbuild $sln /m:1 /p:Configuration=Release /p:Platform=x64 /t:$buildTarget /v:minimal /p:TrackFileAccess=false
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    Write-Host "[+] Build OK: $exePath"
}

if ($Test) {
    if (Test-Path $testExe) {
        Write-Host "[*] Running tests: $testExe"
        & $testExe
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Tests failed (exit $LASTEXITCODE)."
        }
        Write-Host "[+] Tests passed."
    } else {
        Write-Host "[!] No test executable found ($testExe) - Pillar 1 (tests) not started yet."
    }
}

if ($Package) {
    if (-not (Test-Path $exePath)) {
        Write-Error "Executable missing: $exePath (build first)"
    }
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $zip = Join-Path $distDir "ArcRaiders-$stamp.zip"
    Compress-Archive -Path (Join-Path $outDir "*") -DestinationPath $zip
    Write-Host "[+] Packaged: $zip"
}

if ($Run) {
    if (-not (Test-Path $exePath)) {
        Write-Error "Executable missing: $exePath (build first)"
    }
    $null = Stop-ArcRaiders
    Write-Host "[*] Starting $exePath ..."
    Start-Process -FilePath $exePath -WorkingDirectory $outDir
    Write-Host "[+] Launched $(Split-Path -Leaf $exePath)"
}

exit 0