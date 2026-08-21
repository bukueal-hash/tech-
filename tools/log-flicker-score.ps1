# Summarize flicker_score + supporting evidence from the debug log.
param(
    [string]$LogPath = "F:\Test\ARCs\debug-c190fb.log",
    [int]$LastN = 20
)

if (-not (Test-Path $LogPath)) {
    Write-Host "LOG MISSING: $LogPath"
    exit 1
}

Write-Host "=== flicker_score (last $LastN) ==="
Select-String -Path $LogPath -Pattern '"message":"flicker_score"' -SimpleMatch |
    Select-Object -Last $LastN |
    ForEach-Object {
        if ($_.Line -match '"bots":(\d+),"players":(\d+),"world":(\d+),"posFail":(\d+),"visMiss":(\d+),"evictReadmit":(\d+),"distEdge":(\d+),"other":(\d+)(?:,"projFail":(\d+),"labelMiss":(\d+),"paintBots":(\d+),"paintPlayers":(\d+),"paintWorld":(\d+))?,"fixN":(\d+)') {
            $pb = if ($Matches[11]) { $Matches[11] } else { '-' }
            $pp = if ($Matches[12]) { $Matches[12] } else { '-' }
            $pw = if ($Matches[13]) { $Matches[13] } else { '-' }
            $pj = if ($Matches[9]) { $Matches[9] } else { '-' }
            $lm = if ($Matches[10]) { $Matches[10] } else { '-' }
            'scan b={0} p={1} w={2} | paint b={3} p={4} w={5} | posFail={6} visMiss={7} evict={8} dist={9} proj={10} label={11} other={12} fixN={13}' -f `
                $Matches[1],$Matches[2],$Matches[3],$pb,$pp,$pw,$Matches[4],$Matches[5],$Matches[6],$Matches[7],$pj,$lm,$Matches[8],$Matches[14]
        }
    }

Write-Host "=== supporting (last 4 each) ==="
foreach ($msg in @('bot_pos_freeze','bot_nopos','cam_refresh_gap','bot_label_miss')) {
    Write-Host ("-- {0} --" -f $msg)
    Select-String -Path $LogPath -Pattern ('"message":"{0}"' -f $msg) -SimpleMatch |
        Select-Object -Last 4 |
        ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
}

Write-Host "=== latest raid_hb ==="
Select-String -Path $LogPath -Pattern '"message":"raid_hb"' -SimpleMatch |
    Select-Object -Last 2 |
    ForEach-Object {
        if ($_.Line -match '"active":(\d+).*"reason":"([^"]*)".*"wName":"([^"]*)"') {
            'active={0} reason={1} wName={2}' -f $Matches[1], $Matches[2], $Matches[3]
        }
    }
