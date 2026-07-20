param(
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [switch]$FullRebuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exeName = "ArcRaiders"
$outDir = Join-Path $root "Build"
$exePath = Join-Path $outDir "$exeName.exe"
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$sln = Join-Path $root "ArcRaiders.sln"

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
    if ($BuildOnly) {
        return
    }
    if (-not (Get-Process -Name $exeName -ErrorAction SilentlyContinue)) {
        return
    }
    if (Test-IsAdmin) {
        return
    }

    Write-Host "[*] ArcRaiders is running elevated - relaunching this script as admin (silent when UAC is off)..."
    Write-Host "[*] Elevated build progress: $root\build-elevated.log"
    $argList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command",
        "& '$PSCommandPath' $(if ($BuildOnly) {'-BuildOnly '})$(if ($RunOnly) {'-RunOnly '})$(if ($FullRebuild) {'-FullRebuild '}) *>&1 | Tee-Object -FilePath '$root\build-elevated.log'"
    )
    $proc = Start-Process -FilePath "powershell.exe" `
        -ArgumentList $argList `
        -Verb RunAs `
        -PassThru

    if ($null -eq $proc) {
        Write-Error "Elevated relaunch was cancelled or failed."
    }
    # NOTE: do NOT use Start-Process -Wait here. PS 5.1 -Wait waits on the whole
    # process tree, and the elevated child launches ArcRaiders.exe — so -Wait
    # blocked until the overlay was closed. WaitForExit() waits only on the
    # elevated PowerShell itself.
    $proc.WaitForExit()
    exit $proc.ExitCode
}

Ensure-ElevatedIfNeeded
$env:ARC_REBUILD_ELEVATED = "1"

$altExeName = "${exeName}_new"
$altExePath = Join-Path $outDir "$altExeName.exe"
$launchPath = $exePath

if (-not $RunOnly) {
    $stopped = Stop-ArcRaiders
    if (-not $stopped) {
        Write-Host "[!] Could not stop $exeName or unlock $exePath - building alternate output: $altExePath"
        $launchPath = $altExePath
    }

    if (-not (Test-Path $msbuild)) {
        Write-Error "MSBuild not found: $msbuild"
    }

    $buildTarget = if ($FullRebuild) { "Rebuild" } else { "Build" }
    Write-Host "[*] Building Release|x64 ($buildTarget, parallel)..."
    if ($launchPath -eq $altExePath) {
        & $msbuild $sln /m /p:Configuration=Release /p:Platform=x64 /t:$buildTarget /v:minimal /p:TargetName=$altExeName
    } else {
        & $msbuild $sln /m /p:Configuration=Release /p:Platform=x64 /t:$buildTarget /v:minimal
    }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    Write-Host "[+] Build OK: $launchPath"
}

if ($BuildOnly) {
    exit 0
}

if (-not (Test-Path $launchPath)) {
    if (Test-Path $exePath) {
        $launchPath = $exePath
    } elseif (Test-Path $altExePath) {
        $launchPath = $altExePath
    } else {
        Write-Error "Executable missing: $exePath (build first)"
    }
}

$null = Stop-ArcRaiders
Write-Host "[*] Starting $launchPath ..."
Start-Process -FilePath $launchPath -WorkingDirectory $outDir
Write-Host "[+] Launched $(Split-Path -Leaf $launchPath)"
exit 0
