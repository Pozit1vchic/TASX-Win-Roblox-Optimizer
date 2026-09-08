# TASX E2E Verification Concept (FINAL PROMPT STEP 7)
# Requires: TASX.exe running, 5 renamed cmd.exe clients (fake RobloxPlayerBeta.exe)
# Assertions: log-repeat <=3/10s, job rewrites <=1/focus change, warm primed once/version

$clients = @()
for ($i=0; $i -lt 5; $i++) {
    $proc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c pause" -WindowStyle Hidden -PassThru
    $clients += $proc.Id
    # Rename process image (simulated inject): not done by script; use external launcher
}

# Focus simulation: send foreground window switch via WinAPI (conceptual)
# LowMem simulation: use resource exhaustion tool or wait for natural event
# Monitor TASX log file (if LogFile set) or stdout capture

# Assertions after 60 s:
# - [Trimmer] LowMem count <= 2 (cooldown 30 s prevents hot spin)
# - [Jobs] Background profile rewritten <= 3 (hysteresis 500 ms coalesces rapid switches)
# - [Warm] Shared pages primed count = 1 (version-dir cache)
# - [FFlags] Telemetry-only write = 1 per version (atomic temp + replace)

Write-Host "E2E ready — execute with TASX.exe in background, observe log for 60 s"
Write-Host "Pass criteria: LowMem <=2, rewrites <=3, warm=1, fflags=1"
