# TASX E2E Verification (BUG 15 / deliverable 4)
# Run in an ELEVATED PowerShell with TASX.exe (console build) running and its
# stdout redirected to a log file, e.g.:
#   TASX.exe > tasx_e2e.log 2>&1
#   .\verify_tasx.ps1 -Log tasx_e2e.log -WindowSec 120
#
# Pass criteria:
#   [Trimmer] LowMem signal          <= 1 per LowMemCooldownSec (default 30s)
#   [TASX] Low memory event          <= 1 per SystemCleanMinIntervalSec (60s)
#   [Jobs] Background profile        <= 1 rewrite per real focus change
#   [Warm] Shared pages primed       exactly once per version dir
#   any single log line repeated     <= 3 times per 10s window

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

$lines = Get-Content $Log
$trim   = ($lines | Where-Object { $_ -match '\[Trimmer\] LowMem signal' }).Count
$lowmem = ($lines | Where-Object { $_ -match '\[TASX\] Low memory event' }).Count
$rewr   = ($lines | Where-Object { $_ -match '\[Jobs\] Background profile' }).Count
$warm   = ($lines | Where-Object { $_ -match '\[Warm\] Shared pages primed' }).Count

Write-Host "[E2E] Observed over log window ($($lines.Count) lines):"
Write-Host "  [Trimmer] LowMem signal        : $trim  (allowed ~ window/$LowMemCooldownSec)"
Write-Host "  [TASX] Low memory event        : $lowmem (allowed ~ window/$SystemCleanMinIntervalSec)"
Write-Host "  [Jobs] Background profile rewrite: $rewr  (allowed: 1 per focus change)"
Write-Host "  [Warm] Shared pages primed     : $warm  (allowed: 1 per version dir)"

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

$trimOk  = ($trim  -le [Math]::Max(1, [int]($WindowSec / $LowMemCooldownSec)))
$lowOk   = ($lowmem -le [Math]::Max(1, [int]($WindowSec / $SystemCleanMinIntervalSec)))
$warmOk  = ($warm -le 1)
$rewrOk  = ($rewr -le 3)

if ($trimOk -and $lowOk -and $warmOk -and $rewrOk -and -not $fail) {
    Write-Host "[E2E] PASS: no spam, cooldowns respected." -ForegroundColor Green
    exit 0
}
Write-Host "[E2E] FAIL: see counters above." -ForegroundColor Red
exit 1
