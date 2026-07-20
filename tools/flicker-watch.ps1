# Autonomous flicker-fix loop watcher.
# Tails debug-c190fb.log and emits sentinel lines the agent can monitor:
#   RAID_START / RAID_END  - raid gate edges (debounced)
#   FLICKER_SCORE ...      - every flicker_score NDJSON line summarized
#   CHECKPOINT ...         - every ~3 minutes of active raid data

param(
    [string]$LogPath = "F:\Test\ARCs\debug-c190fb.log",
    [int]$PollMs = 1500,
    [int]$RaidDebounceSec = 8,
    [int]$CheckpointSec = 180
)

$ErrorActionPreference = "Continue"
Write-Host "[flicker-watch] watching $LogPath"
Write-Host "[flicker-watch] sentinels: RAID_START RAID_END FLICKER_SCORE CHECKPOINT"

$offset = 0
if (Test-Path $LogPath) {
    $offset = (Get-Item $LogPath).Length
}

$raidActive = $false
$pendingRaid = $null
$pendingSince = $null
$raidActiveAccumSec = 0
$lastCheckpoint = Get-Date
$lastRaidHb = Get-Date

function Emit-Sentinel([string]$line) {
    $ts = (Get-Date).ToString("HH:mm:ss")
    Write-Host ("[{0}] {1}" -f $ts, $line)
}

while ($true) {
    Start-Sleep -Milliseconds $PollMs

    if (-not (Test-Path $LogPath)) {
        $offset = 0
        continue
    }

    $len = (Get-Item $LogPath).Length
    if ($len -lt $offset) {
        # Log rotated / deleted
        $offset = 0
    }
    if ($len -eq $offset) {
        # Debounce pending raid edge even without new bytes
        if ($null -ne $pendingRaid -and $null -ne $pendingSince) {
            if (((Get-Date) - $pendingSince).TotalSeconds -ge $RaidDebounceSec) {
                if ($pendingRaid -ne $raidActive) {
                    $raidActive = $pendingRaid
                    if ($raidActive) {
                        Emit-Sentinel "RAID_START"
                        $raidActiveAccumSec = 0
                        $lastCheckpoint = Get-Date
                    } else {
                        Emit-Sentinel ("RAID_END accumSec={0}" -f [int]$raidActiveAccumSec)
                    }
                }
                $pendingRaid = $null
                $pendingSince = $null
            }
        }
        if ($raidActive) {
            $raidActiveAccumSec = [int]((Get-Date) - $lastCheckpoint).TotalSeconds + $raidActiveAccumSec
            # fix accum: use dedicated start
        }
        continue
    }

    $fs = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $null = $fs.Seek($offset, [System.IO.SeekOrigin]::Begin)
        $sr = New-Object System.IO.StreamReader($fs)
        while ($null -ne ($line = $sr.ReadLine())) {
            if ($line -match '"message":"raid_hb".*"active":(\d+)') {
                $active = [int]$Matches[1] -eq 1
                $lastRaidHb = Get-Date
                if ($active -ne $raidActive) {
                    if ($null -eq $pendingRaid -or $pendingRaid -ne $active) {
                        $pendingRaid = $active
                        $pendingSince = Get-Date
                    }
                } else {
                    $pendingRaid = $null
                    $pendingSince = $null
                }
            }
            elseif ($line -match '"message":"flicker_score".*"bots":(\d+),"players":(\d+),"world":(\d+),"posFail":(\d+),"visMiss":(\d+),"evictReadmit":(\d+),"distEdge":(\d+),"other":(\d+)(?:,"projFail":(\d+),"labelMiss":(\d+),"paintBots":(\d+),"paintPlayers":(\d+),"paintWorld":(\d+))?,"fixN":(\d+)') {
                $paintB = if ($Matches[11]) { [int]$Matches[11] } else { 0 }
                $paintP = if ($Matches[12]) { [int]$Matches[12] } else { 0 }
                $paintW = if ($Matches[13]) { [int]$Matches[13] } else { 0 }
                $total = [int]$Matches[1] + [int]$Matches[2] + [int]$Matches[3] + $paintB + $paintP + $paintW
                # Only emit non-zero scores (zero windows were flooding notifications).
                if ($total -gt 0) {
                    Emit-Sentinel ("FLICKER_SCORE total={0} bots={1} players={2} world={3} paintB={4} paintP={5} paintW={6} posFail={7} visMiss={8} evict={9} dist={10} proj={11} label={12} other={13} fixN={14}" -f `
                        $total, $Matches[1], $Matches[2], $Matches[3], $paintB, $paintP, $paintW, `
                        $Matches[4], $Matches[5], $Matches[6], $Matches[7], `
                        $(if ($Matches[9]) { $Matches[9] } else { 0 }), `
                        $(if ($Matches[10]) { $Matches[10] } else { 0 }), `
                        $Matches[8], $Matches[14])
                }
            }
        }
        $offset = $fs.Position
    } finally {
        $fs.Close()
    }

    if ($null -ne $pendingRaid -and $null -ne $pendingSince) {
        if (((Get-Date) - $pendingSince).TotalSeconds -ge $RaidDebounceSec) {
            if ($pendingRaid -ne $raidActive) {
                $raidActive = $pendingRaid
                if ($raidActive) {
                    Emit-Sentinel "RAID_START"
                    $script:raidDataStart = Get-Date
                    $lastCheckpoint = Get-Date
                } else {
                    $accum = 0
                    if ($script:raidDataStart) {
                        $accum = [int]((Get-Date) - $script:raidDataStart).TotalSeconds
                    }
                    Emit-Sentinel ("RAID_END accumSec={0}" -f $accum)
                }
            }
            $pendingRaid = $null
            $pendingSince = $null
        }
    }

    if ($raidActive) {
        $elapsed = [int]((Get-Date) - $lastCheckpoint).TotalSeconds
        if ($elapsed -ge $CheckpointSec) {
            Emit-Sentinel ("CHECKPOINT raidSec={0}" -f $elapsed)
            $lastCheckpoint = Get-Date
        }
    }
}
