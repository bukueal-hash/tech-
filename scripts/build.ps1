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

# Offsets are generated from the offline sources of truth (the FrostDumper
# index and the CL drop in sdk/), never hand-edited. This audit fails the build
# when the generated header would differ from what is on disk, or when a source
# that can see a slot outranks the shipped value - so a wrong offset cannot be
# merged just because it compiles.
function Find-Python {
    foreach ($candidate in @("python", "py")) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    return $null
}

function Invoke-OffsetsAudit {
    $gen = Join-Path $root "tools\gen_offsets.py"
    $reconcile = Join-Path $root "tools\reconcile_offsets.py"
    if (-not (Test-Path $gen)) {
        Write-Host "[!] tools/gen_offsets.py missing - skipping the offset audit."
        return
    }
    $python = Find-Python
    if (-not $python) {
        Write-Host "[!] python not found on PATH - skipping the offset audit."
        return
    }

    Write-Host "[*] Audit: Offsets.h against the offline dumps..."
    $genOutput = & $python $gen --check 2>&1
    $genCode = $LASTEXITCODE
    foreach ($line in $genOutput) { Write-Host "    $line" }
    if ($genCode -ne 0) {
        Write-Error ("Offsets.h does not match the offline sources (exit $genCode): " +
            "run python tools/gen_offsets.py and fix tools/ before building.")
    }

    # gen_offsets --check already runs the reconciliation as part of its own
    # gate (a source that can see a slot outranking the shipped value is a
    # failure there), so this second pass only adds the scoreboard summary.
    if (Test-Path $reconcile) {
        $scores = & $python $reconcile --check --quiet 2>&1
        $scoreCode = $LASTEXITCODE
        foreach ($line in $scores) { Write-Host "    $line" }
        if ($scoreCode -ne 0) {
            Write-Error ("Reconciliation found an offset a higher-authority source " +
                "disagrees with (exit $scoreCode).")
        }
    }

    # The gap report is the SDK-adoption scoreboard: every constant the drop
    # carries must be wired into a feature or explicitly waived with a reason
    # (tools/gap_dispositions.json). Unaccounted entries fail the build.
    $gap = Join-Path $root "tools\sdk_gap_report.py"
    if (Test-Path $gap) {
        Write-Host "[*] Audit: sdk.txt adoption scoreboard..."
        $gapOut = & $python $gap --check 2>&1
        $gapCode = $LASTEXITCODE
        foreach ($line in $gapOut) { Write-Host "    $line" }
        if ($gapCode -ne 0) {
            Write-Error ("SDK gap check failed (exit $gapCode): wire the constants " +
                "or add tools/gap_dispositions.json entries.")
        }
    }
    Write-Host "[+] Offsets audited against the dumps."
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

# ── Build inputs ─────────────────────────────────────────────────────────────
# What counts as a compile input, and how new the newest one is. The full
# verification of an MSBuild run lives further down ("Build verification"):
# timestamps alone cannot see a relink that never recompiled, so inputs are
# hashed there and MSBuild's log is read for evidence.

$script:SourceExtensions = @('.cpp', '.c', '.cc', '.h', '.hpp', '.hxx', '.inl', '.rc', '.vcxproj', '.filters')
# Intermediate/generated trees and copied data are not compile inputs.
$script:SourceSkip = @('\Data\', '\lib\', '\obj\', '\x64\', '\.vs\', '\Generated\', '\Intermediate\')

function Get-NewestSourceTime {
    param([string]$ProjectDir, [datetime]$Now)
    $newest = [datetime]::MinValue
    if (-not (Test-Path $ProjectDir)) { return $newest }

    $files = @(Get-ChildItem -LiteralPath $ProjectDir -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($f in $files) {
        if ($script:SourceExtensions -notcontains $f.Extension.ToLowerInvariant()) { continue }
        $skip = $false
        foreach ($s in $script:SourceSkip) {
            if ($f.FullName -like "*$s*") { $skip = $true; break }
        }
        if ($skip) { continue }
        # A future-dated file (clock skew, network copy) would otherwise fail every
        # build forever; report it and leave it out of the comparison.
        if ($f.LastWriteTimeUtc -gt $Now.AddMinutes(2)) {
            Write-Host "[!] Source is future-dated, ignored for the freshness check: $($f.FullName)"
            continue
        }
        if ($f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
    }
    return $newest
}

function Get-OutputTime {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    return (Get-Item $Path).LastWriteTimeUtc
}

function Format-Stamp {
    param([datetime]$Utc)
    return $Utc.ToLocalTime().ToString("HH:mm:ss")
}

# ── Build verification ───────────────────────────────────────────────────────
# MSBuild's own incremental decision cannot be trusted on this machine:
# /p:TrackFileAccess=false leaves the dependency cache in a state where a project
# is declared up to date and exits 0 without compiling anything, and a backdated
# edit (content changed, mtime not moved forward) is invisible to a timestamp
# comparison even when MSBuild does look. A timestamp-only audit therefore
# reported "relinked" for a binary that could not contain the change. "Build OK"
# now has to be backed by evidence:
#
#   1. a content hash of every compile input, compared with the stamp written by
#      the last verified build - that is what decides whether output is owed;
#   2. MSBuild's own log must show a compile for every changed translation unit,
#      and for each project with changed inputs a compile *and* a link;
#   3. every output must have been rewritten, and its bytes must differ from the
#      stamped ones unless the log shows the link that produced it (a
#      deterministic build may legitimately reproduce an identical binary).
#
# Anything short of that escalates once to /t:Rebuild and then fails the build.

# Translation units only. A .rc is built by the ResourceCompile task, which emits no
# per-file line of its own - demanding compile evidence for it failed a correct build.
$script:CompileExtensions = @('.cpp', '.c', '.cc')
$script:BuildStampPath = Join-Path $outDir ".build-stamp.json"

function Get-FileHashSafe {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        # FileShare.ReadWrite: the overlay may still hold the exe open, and the
        # hash is only ever compared against the next build's snapshot.
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            return [System.BitConverter]::ToString($sha.ComputeHash($fs)).Replace('-', '')
        } finally { $fs.Dispose() }
    } catch {
        return $null
    } finally {
        $sha.Dispose()
    }
}

function Get-RelativeBuildPath {
    param([string]$FullName)
    if ($FullName.Length -lt $root.Length) { return $FullName }
    if (-not $FullName.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { return $FullName }
    return $FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
}

function Get-InputManifest {
    <#
      Content hash of every compile input. The timestamp rides along for reporting
      only: the hash is what decides whether the build owes output, because a
      backdated save or a restored file keeps its old mtime.
    #>
    param([string]$ProjectDir, [datetime]$Now)
    $manifest = @{}
    if (-not (Test-Path $ProjectDir)) { return $manifest }

    foreach ($f in @(Get-ChildItem -LiteralPath $ProjectDir -Recurse -File -ErrorAction SilentlyContinue)) {
        if ($script:SourceExtensions -notcontains $f.Extension.ToLowerInvariant()) { continue }
        $skip = $false
        foreach ($s in $script:SourceSkip) {
            if ($f.FullName -like "*$s*") { $skip = $true; break }
        }
        if ($skip) { continue }
        if ($f.LastWriteTimeUtc -gt $Now.AddMinutes(2)) {
            Write-Host "[!] Source is future-dated, ignored for the build check: $($f.FullName)"
            continue
        }
        $hash = Get-FileHashSafe -Path $f.FullName
        $manifest[(Get-RelativeBuildPath -FullName $f.FullName)] = [pscustomobject]@{
            hash = $hash
            size = $f.Length
            time = $f.LastWriteTimeUtc
        }
    }
    return $manifest
}

function Get-ManifestHash {
    param([hashtable]$Manifest)
    $lines = @()
    foreach ($rel in ($Manifest.Keys | Sort-Object)) {
        $lines += "$rel|$($Manifest[$rel].hash)"
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes([string]::Join("`n", $lines))
        return [System.BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
    } finally { $sha.Dispose() }
}

function Get-ChangedInputs {
    param([hashtable]$Manifest, $Stamp)
    $changed = @()
    if ($null -eq $Stamp) {
        foreach ($rel in ($Manifest.Keys | Sort-Object)) { $changed += $rel }
        return $changed
    }
    $old = @{}
    foreach ($entry in @($Stamp.inputs)) {
        $parts = "$entry" -split '\|', 2
        if ($parts.Count -ge 2) { $old[$parts[0]] = $parts[1] }
    }
    foreach ($rel in ($Manifest.Keys | Sort-Object)) {
        if (-not $old.ContainsKey($rel) -or $old[$rel] -ne $Manifest[$rel].hash) { $changed += $rel }
    }
    foreach ($rel in $old.Keys) {
        if (-not $Manifest.ContainsKey($rel)) { $changed += "$rel (removed)" }
    }
    return $changed
}

function Get-ProjectFileMap {
    <#
      Which project owns each source, read from the project files themselves, so a
      changed translation unit can be required to have been compiled by its own
      project instead of by "something".
    #>
    $byPath = @{}
    $byLeaf = @{}
    $excluded = @{}
    $projects = @(
        [pscustomobject]@{ key = 'app'; path = (Join-Path $root 'Project\Project.vcxproj') },
        [pscustomobject]@{ key = 'tests'; path = (Join-Path $root 'Project\Project.Tests.vcxproj') }
    )
    foreach ($p in $projects) {
        if (-not (Test-Path $p.path)) { continue }
        $text = Get-Content -LiteralPath $p.path -Raw
        $dir = Split-Path -Parent $p.path
        # Every item element, not just ClCompile/ClInclude: a changed app.rc has to be
        # attributed to its own project, else the check demands both projects compiled.
        foreach ($m in [regex]::Matches($text, '<(?<tag>[A-Za-z]+)\s+Include="(?<inc>[^"]+)"')) {
            $tag = $m.Groups['tag'].Value
            if ($tag -eq 'ProjectReference') { continue }
            $inc = $m.Groups['inc'].Value
            if ($inc -match '\$\(|%|[|<>"?*]') { continue }   # expanded property or non-path (ProjectConfiguration "Debug|x64")
            $full = $inc
            if (-not [System.IO.Path]::IsPathRooted($full)) {
                $full = [System.IO.Path]::GetFullPath((Join-Path $dir $inc))
            }
            $rel = Get-RelativeBuildPath -FullName $full
            if (-not $byPath.ContainsKey($rel)) { $byPath[$rel] = $p.key }
            $leaf = [System.IO.Path]::GetFileName($rel).ToLowerInvariant()
            if (-not $byLeaf.ContainsKey($leaf)) { $byLeaf[$leaf] = @() }
            if ($byLeaf[$leaf] -notcontains $p.key) { $byLeaf[$leaf] += $p.key }
        }
        # A ClCompile with <ExcludedFromBuild>true</ExcludedFromBuild> for this
        # configuration (ThirdParty\ImGui\imgui_demo.cpp) is never compiled, so
        # requiring a compile line for it failed a build that was correct.
        foreach ($b in [regex]::Matches($text,
                # [^>/] on the attributes: a self-closing element must not swallow the
                # body of the next one, or its exclusion lands on the wrong file.
                '<(?<tag>ClCompile|ClInclude|ResourceCompile)\s+Include="(?<inc>[^"]+)"(?<rest>[^>/]*)>(?<body>.*?)</\k<tag>>',
                'Singleline')) {
            $ex = [regex]::Match($b.Groups['body'].Value, '<ExcludedFromBuild(?<attrs>[^>]*)>\s*true\s*<')
            if (-not $ex.Success) { continue }
            $cond = [regex]::Match($ex.Groups['attrs'].Value, 'Condition="([^"]*)"')
            if ($cond.Success -and ($cond.Groups[1].Value -notmatch 'Release\|x64')) { continue }
            $inc = $b.Groups['inc'].Value
            if ($inc -match '\$\(|%|[|<>"?*]') { continue }
            $full = $inc
            if (-not [System.IO.Path]::IsPathRooted($full)) {
                $full = [System.IO.Path]::GetFullPath((Join-Path $dir $inc))
            }
            $excluded[(Get-RelativeBuildPath -FullName $full)] = $true
        }
    }
    return @{ byPath = $byPath; byLeaf = $byLeaf; excluded = $excluded }
}

function Read-BuildStamp {
    if (-not (Test-Path $script:BuildStampPath)) { return $null }
    try {
        return (Get-Content -LiteralPath $script:BuildStampPath -Raw | ConvertFrom-Json)
    } catch {
        Write-Host "[!] Build stamp is unreadable - this build is checked in full."
        return $null
    }
}

function Save-BuildStamp {
    param([string]$InputHash, [hashtable]$Manifest, [string[]]$Outputs, [string]$Target)
    $inputs = @()
    foreach ($rel in ($Manifest.Keys | Sort-Object)) { $inputs += "$rel|$($Manifest[$rel].hash)" }
    $outs = @()
    foreach ($out in $Outputs) {
        if (-not (Test-Path $out)) { continue }
        $item = Get-Item $out
        $outs += [pscustomobject]@{
            path = $out
            hash = (Get-FileHashSafe -Path $out)
            size = $item.Length
            time = $item.LastWriteTimeUtc.ToString('o')
        }
    }
    $payload = [pscustomobject]@{
        version = 1
        builtAt = (Get-Date).ToUniversalTime().ToString('o')
        configuration = 'Release|x64'
        target = $Target
        inputHash = $InputHash
        inputs = $inputs
        outputs = $outs
    }
    Set-Content -LiteralPath $script:BuildStampPath -Value ($payload | ConvertTo-Json -Depth 5) -Encoding UTF8
}

function Get-BuildEvidence {
    <# What MSBuild actually did, read off its own log. #>
    param([string[]]$Log)
    $compiled = @{}
    $linked = @{}
    $compileRe = [regex]'^(?<indent>\s+)(?<name>[^\s\\/:*?"<>|]+\.(?:cpp|c|cc|rc))\s*$'
    foreach ($raw in @($Log)) {
        $line = "$raw"
        $m = $compileRe.Match($line)
        if ($m.Success) { $compiled[$m.Groups['name'].Value.ToLowerInvariant()] = $true }
        if ($line -match '->\s*(\S+)\.exe\s*$') {
            $linked[[System.IO.Path]::GetFileName($Matches[1]).ToLowerInvariant() + '.exe'] = $true
        }
    }
    return @{ compiled = $compiled; linked = $linked }
}

function Get-BuildProblems {
    <#
      Evidence check for one MSBuild invocation. An empty list means the build can
      be called trustworthy; anything in it is printed and fails the build.
    #>
    param(
        [bool]$Owed,
        [string[]]$Changed,
        [hashtable]$ProjectMap,
        [hashtable]$Evidence,
        [hashtable]$OutputSpecs,
        [hashtable]$Before,
        [datetime]$NewestSource
    )
    $problems = @()
    $needsProject = @{}

    # a. every changed translation unit must have been compiled
    foreach ($rel in $Changed) {
        $clean = $rel -replace ' \(removed\)$', ''
        # <ExcludedFromBuild> for this configuration: the file is not part of the
        # build at all, so it can neither be compiled nor go stale.
        if ($ProjectMap.excluded.ContainsKey($clean)) { continue }
        $proj = $null
        if ($ProjectMap.byPath.ContainsKey($clean)) { $proj = $ProjectMap.byPath[$clean] }
        if ($null -eq $proj) {
            # unknown owner (the .sln, a header no project lists): require evidence
            # from both projects rather than guess which one it feeds
            $needsProject['app'] = $true
            $needsProject['tests'] = $true
        } else {
            $needsProject[$proj] = $true
        }
        $ext = [System.IO.Path]::GetExtension($clean).ToLowerInvariant()
        if ($script:CompileExtensions -contains $ext) {
            $leaf = [System.IO.Path]::GetFileName($clean).ToLowerInvariant()
            if (-not $Evidence.compiled.ContainsKey($leaf)) {
                $problems += "MSBuild reported success but never compiled $clean"
            }
        }
    }

    # b. each project with changed inputs must have compiled something
    if ($Owed) {
        $compiledProjects = @{}
        foreach ($leaf in $Evidence.compiled.Keys) {
            if ($ProjectMap.byLeaf.ContainsKey($leaf)) {
                foreach ($p in $ProjectMap.byLeaf[$leaf]) { $compiledProjects[$p] = $true }
            }
        }
        foreach ($p in $needsProject.Keys) {
            if (-not $compiledProjects.ContainsKey($p)) {
                $problems += "no $p translation unit was compiled although its inputs changed"
            }
        }
    }

    # c. every output: rewritten, linked or byte-changed when output is owed
    foreach ($out in $OutputSpecs.Keys) {
        $leaf = Split-Path -Leaf $out
        if (-not (Test-Path $out)) {
            $problems += "$leaf is missing after the build"
            continue
        }
        $item = Get-Item $out
        $hadTime = $Before.ContainsKey($out) -and $null -ne $Before[$out].time
        $rewritten = (-not $hadTime) -or ($item.LastWriteTimeUtc -gt $Before[$out].time)
        if ($Owed -and -not $rewritten) {
            $problems += "$leaf was not rewritten by this build (still $(Format-Stamp $item.LastWriteTimeUtc))"
        }
        if ($Owed -and $needsProject.ContainsKey($OutputSpecs[$out])) {
            $linked = $Evidence.linked.ContainsKey($leaf.ToLowerInvariant())
            $newHash = Get-FileHashSafe -Path $out
            $bytesChanged = ($null -ne $Before[$out].hash) -and ($newHash -ne $Before[$out].hash)
            if (-not $linked -and -not $bytesChanged) {
                $problems += "$leaf was not linked and its bytes did not change"
            }
        }
        if (-not $Owed -and $null -ne $Before[$out].hash) {
            # TrackFileAccess=false (mandatory, see the FileTracker lock note) means
            # MSBuild cannot prove outputs are up to date, so even a no-op Build
            # relinks both exes here. That relink is the build's own work - only an
            # output change the build's own link log never claimed belongs to
            # something outside the build.
            $newHash = Get-FileHashSafe -Path $out
            if ($newHash -ne $Before[$out].hash -and
                -not $Evidence.linked.ContainsKey($leaf.ToLowerInvariant())) {
                $problems += ("$leaf changed although no input did, and the build's own " +
                    "log never linked it - something outside the build wrote it")
            }
        }
        if ($Owed -and $NewestSource -ne [datetime]::MinValue `
            -and $item.LastWriteTimeUtc -lt $NewestSource) {
            $problems += "$leaf is older than the newest input ($(Format-Stamp $NewestSource))"
        }
    }
    return $problems
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
    $outputSpecs = @{}
    $outputSpecs[$exePath] = 'app'
    $outputSpecs[$testExe] = 'tests'

    Invoke-OffsetsAudit

    # Hash the inputs and read the stamp the last verified build wrote: that pair
    # decides whether this build owes output at all, which no timestamp can - a
    # backdated edit changes content without moving mtime.
    $projectDir = Join-Path $root "Project"
    $auditStart = (Get-Date).ToUniversalTime()
    $newestSource = Get-NewestSourceTime -ProjectDir $projectDir -Now $auditStart
    $manifest = Get-InputManifest -ProjectDir $projectDir -Now $auditStart
    $inputHash = Get-ManifestHash -Manifest $manifest
    $stamp = Read-BuildStamp
    $projectMap = Get-ProjectFileMap

    $before = @{}
    foreach ($out in $outputSpecs.Keys) {
        $before[$out] = [pscustomobject]@{
            time = (Get-OutputTime -Path $out)
            hash = (Get-FileHashSafe -Path $out)
        }
    }

    $missing = @($outputSpecs.Keys | Where-Object { -not (Test-Path $_) })
    $changed = @(Get-ChangedInputs -Manifest $manifest -Stamp $stamp)
    # An explicit full rebuild is itself a request to relink both binaries,
    # even when the source manifest is unchanged. Without this, Rebuild
    # would be judged against the no-input-change branch and could fail its
    # own evidence check whenever the linker produced byte-identical output.
    $owed = $FullRebuild -or ($null -eq $stamp) -or ($stamp.inputHash -ne $inputHash) -or ($missing.Count -gt 0)

    if ($null -eq $stamp) {
        Write-Host "[i] No verified-build stamp yet - checking this build in full."
    } elseif ($owed) {
        Write-Host "[*] $($changed.Count) input(s) changed since the verified build at $($stamp.builtAt):"
        foreach ($rel in @($changed | Select-Object -First 12)) { Write-Host "      $rel" }
        if ($changed.Count -gt 12) { Write-Host "      ... and $($changed.Count - 12) more" }
    } else {
        Write-Host "[i] Inputs are unchanged since the verified build at $($stamp.builtAt)."
    }

    # /m:1 (single node): the parallel MSBuild file nodes race on FileTracker
    # tlog + .obj writes ("user-mapped section" MSB6003 / C1083 "Invalid
    # argument" on a different obj every run). Serializing the build makes it
    # deterministic.
    # TrackFileAccess=false avoids the intermittent FileTracker "user-mapped
    # section" tlog lock failures - and it is exactly why the result has to be
    # verified here instead of trusted from MSBuild's exit code.
    $problems = @()
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        $target = $buildTarget
        if ($attempt -eq 2) { $target = "Rebuild" }

        Write-Host "[*] Building Release|x64 ($target, single-node)..."
        $msbuildLog = @()
        & $msbuild $sln /m:1 /p:Configuration=Release /p:Platform=x64 /t:$target /v:minimal /p:TrackFileAccess=false |
            Tee-Object -Variable msbuildLog
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }

        $evidence = Get-BuildEvidence -Log $msbuildLog
        $problems = @(Get-BuildProblems -Owed $owed -Changed $changed -ProjectMap $projectMap `
            -Evidence $evidence -OutputSpecs $outputSpecs -Before $before -NewestSource $newestSource)

        if ($problems.Count -eq 0) {
            break
        }
        if ($attempt -lt 2) {
            Write-Host "[!] The incremental build did not produce verifiable output:"
            foreach ($p in $problems) { Write-Host "      - $p" }
            Write-Host "[!] Retrying with /t:Rebuild..."
        }
    }

    if ($problems.Count -gt 0) {
        Write-Error ("Build reported success but its output cannot be trusted:`n  - " +
            ($problems -join "`n  - ") +
            "`nThe binary on disk may not contain the sources. Delete Project\x64 (and Build\) and rebuild.")
    }

    foreach ($out in $outputSpecs.Keys) {
        $item = Get-Item $out
        $rewritten = (-not $before.ContainsKey($out)) -or ($null -eq $before[$out].time) `
            -or ($item.LastWriteTimeUtc -gt $before[$out].time)
        if ($rewritten) {
            Write-Host "[+] $(Split-Path -Leaf $out): relinked at $(Format-Stamp $item.LastWriteTimeUtc) ($($item.Length) bytes)"
        } else {
            Write-Host "[i] $(Split-Path -Leaf $out): up to date ($(Format-Stamp $item.LastWriteTimeUtc), $($item.Length) bytes)"
        }
    }

    # Only a verified build may write the stamp, because the next build trusts it
    # to decide whether output is owed.
    Save-BuildStamp -InputHash $inputHash -Manifest $manifest -Outputs @($outputSpecs.Keys) -Target $buildTarget
    Write-Host "[+] Build OK: $exePath (verified against $($manifest.Count) hashed inputs)"
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