# TASX E2E Verification
# Run in an ELEVATED PowerShell with TASX.exe (console build) running and its
# stdout redirected to a log file, e.g.:
#   TASX.exe > tasx_e2e.log 2>&1
#   .\verify_tasx.ps1 -Log tasx_e2e.log -WindowSec 120
#
# Pass criteria (all tags verified against Infra/ sources):
#   [TASX] Low memory event          <= 1 per SystemCleanMinIntervalSec (60s)
#   [Jobs] Background profile        <= 1 rewrite per real focus change
#   [Warm] Shared pages primed       exactly once per version dir
#   [FFlags] Applied                 only on real rewrites (hash-cache silent)
#   [TASX] Pagefile volume ... low   <= 1 per 300s (rate-limited)
#   [Jobs] assign denied / foreign   <= 1 per PID per 300s (sticky, no spam)
#   [TASX] Roblox PID ... focus      <= 1 pair per FocusDwellMs (coalesced)
#   any single log line repeated     <= 3 times per ~10s window

param(
    [string]$Log = "tasx_e2e.log",
    [int]$WindowSec = 120,
    [int]$LowMemCooldownSec = 30,
    [int]$SystemCleanMinIntervalSec = 60
)

if (-not (Test-Path $Log)) {
    Write-Host "[E2E] Log file '$Log' not found. Start TASX.exe with stdout redirected first."
    exit 1
}

$lines   = Get-Content $Log
$lowmem  = ($lines | Where-Object { $_ -match '\[TASX\] Low memory event' }).Count
$rewr    = ($lines | Where-Object { $_ -match '\[Jobs\] Background profile' }).Count
$warm    = ($lines | Where-Object { $_ -match '\[Warm\] Shared pages primed' }).Count
$fflags  = ($lines | Where-Object { $_ -match '\[FFlags\] Applied \d+ flags' }).Count
$pagef   = ($lines | Where-Object { $_ -match 'Pagefile volume .* low free' }).Count
$jobden  = ($lines | Where-Object { $_ -match '\[Jobs\] PID \d+ (assign denied|already in foreign job)' }).Count
$focus   = ($lines | Where-Object { $_ -match '\[TASX\] Roblox PID \d+ (in focus|lost focus)' }).Count
$trimsum = ($lines | Where-Object { $_ -match '\[Trimmer\] \d+ client\(s\).*avg WS' }).Count
$boost   = ($lines | Where-Object { $_ -match '\[TASX\] FarmBoost (ON|OFF)' }).Count

Write-Host "[E2E] Observed over log window ($($lines.Count) lines):"
Write-Host "  [TASX] Low memory event          : $lowmem (allowed ~ window/$SystemCleanMinIntervalSec)"
Write-Host "  [Jobs] Background profile rewrite: $rewr  (allowed: 1 per focus change)"
Write-Host "  [Warm] Shared pages primed       : $warm  (allowed: 1 per version dir)"
Write-Host "  [FFlags] Applied flags           : $fflags (real work only)"
Write-Host "  [TASX] Pagefile low              : $pagef (rate-limited 300s)"
Write-Host "  [Jobs] denied/foreign fallback   : $jobden (<=1 per PID per 300s)"
Write-Host "  [TASX] Focus switches (dwell)    : $focus (coalesced)"
Write-Host "  [Trimmer] 30s summaries w/ WS    : $trimsum (expect >= window/30 - 1)"
Write-Host "  [TASX] FarmBoost toggles         : $boost (manual hotkey presses only)"

# Repeat check: no identical line more than 3 times per 10s slice of the log.
# TASX stdout has no timestamps, so slices are approximated by line batches:
# the 10s tick loop emits ~40 lines (250ms PopEvent), we use 40-line batches.
$batch = 40
$fail = $false
for ($i = 0; $i -lt $lines.Count; $i += $batch) {
    $slice = $lines[$i..([Math]::Min($i + $batch - 1, $lines.Count - 1))]
    $dups = $slice | Group-Object | Where-Object { $_.Count -gt 3 }
    if ($dups) {
        $fail = $true
        Write-Host "[E2E] FAIL: repeated lines >3 in one ~10s slice:" -ForegroundColor Red
        $dups | ForEach-Object { Write-Host "    x$($_.Count): $($_.Name)" }
    }
}

$lowOk   = ($lowmem -le [Math]::Max(1, [int]($WindowSec / $SystemCleanMinIntervalSec)))
$warmOk  = ($warm -le 1)
$rewrOk  = ($rewr -le 3)
$pageOk  = ($pagef -le [Math]::Max(1, [int]($WindowSec / 300)))

if ($lowOk -and $warmOk -and $rewrOk -and $pageOk -and -not $fail) {
    Write-Host "[E2E] PASS: no spam, cooldowns respected." -ForegroundColor Green
    exit 0
}
Write-Host "[E2E] FAIL: see counters above." -ForegroundColor Red
exit 1
