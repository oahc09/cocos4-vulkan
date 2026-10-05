# WSI lifecycle aborted-frame / recreate-failure verification (M2-F).
#
# Runs each RED-expected case in its OWN process; one crash cannot hide the
# others. Strict per-case verdict (do NOT reuse the S1..S8 judge):
#   - exit 0
#   - validation ENABLED
#   - exactly ONE "<case> completed: failures=0 liveErrors=0 postErrors=0" line
#   - no VUID / SYNC-HAZARD / harness FAIL lines
# With the core unfixed, failures/crashes are EXPECTED evidence - they stay
# FAIL and are reported, never masked.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify-wsi-lifecycle.ps1
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [switch]$AutoBarrier,      # pass --auto-barrier to every case (fixed at startup)
    [switch]$MaintenanceOff    # run with CC_VULKAN_DISABLE_FEATURES=swapchainMaintenance1
)

$ErrorActionPreference = 'Continue'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator' }
$vmRoot = Join-Path $repo 'native\build\vulkan-modernization'
New-Item -ItemType Directory -Force -Path $vmRoot | Out-Null

function Test-LifecycleLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$CaseName = ""
    )
    $reasons = @()
    if ($ExitCode -ne 0) { $reasons += "exit code $ExitCode != 0" }
    if (-not ($Lines | Where-Object { $_ -match 'validation \+ best practices \+ sync: ENABLED' })) {
        $reasons += "validation not ENABLED"
    }
    $done = @($Lines | Where-Object { $_ -match "^$CaseName completed: " })
    if ($done.Count -ne 1) { $reasons += "completion marker count = $($done.Count) (expected 1)" }
    elseif ($done[0] -notmatch 'failures=0 liveErrors=0 postErrors=0') { $reasons += "completion footer not clean: $($done[0])" }
    if ($Lines | Where-Object { $_ -match 'WARNING: case did not reach' }) { $reasons += "case incomplete" }
    $vuids = ($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count
    if ($vuids -gt 0) { $reasons += "$vuids VUID/hazard lines" }
    $fails = ($Lines | Where-Object { $_ -match '^FAIL:' }).Count
    if ($fails -gt 0) { $reasons += "$fails harness FAIL lines" }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons; Footer = ($done | Select-Object -First 1) }
}

$cases = @('zero-before-submit', 'surface-before-submit', 'resize-after-record', 'create-out-of-date', 'two-window-abort', 'reacquire-stale-record')

$oldPath = $env:PATH; $oldLayer = $env:VK_LAYER_PATH; $oldImplicit = $env:VK_IMPLICIT_LAYER_PATH
$oldDisable = $env:CC_VULKAN_DISABLE_FEATURES
$oldLoc = Get-Location
try {
    & cmake --build $BuildDir --config Release --target vulkan_wsi_lifecycle_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_wsi_lifecycle_smoke.exe'
    if (-not (Test-Path $exe)) { throw "exe not produced: $exe" }

    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $emptyImplicit = Join-Path $vmRoot 'empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $emptyImplicit | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $emptyImplicit
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    Push-Location $vmRoot

    $all = $true
    foreach ($c in $cases) {
        $modeTag = $(if ($AutoBarrier) {'ab-on'} else {'ab-off'}) + $(if ($MaintenanceOff) {'-m1off'} else {'-m1on'})
        $log = Join-Path $vmRoot ("wsi-life-{0}-{1}-{2:yyyyMMdd-HHmmss-fff}-{3}.log" -f $c, $modeTag, (Get-Date), $PID)
        $extra = $(if ($AutoBarrier) {' --auto-barrier'} else {''})
        if ($MaintenanceOff) { $env:CC_VULKAN_DISABLE_FEATURES = 'swapchainMaintenance1' } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }
        cmd /c """$exe"" --case ""$c""$extra > ""$log"" 2>&1"
        $code = $LASTEXITCODE
        $lines = if (Test-Path $log) {
            $text = [System.IO.File]::ReadAllText($log)
            if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
            @($text -split "`r?`n")
        } else { @() }
        $v = Test-LifecycleLog -Lines $lines -ExitCode $code -CaseName $c
        Write-Host ("  [{0}] {1}: exit={2} {3}" -f $(if ($v.Pass) {'PASS'} else {'FAIL'}), $c, $code, $v.Footer)
        if (-not $v.Pass) {
            $all = $false
            $v.Reasons | Select-Object -First 4 | ForEach-Object { Write-Host "    reason: $_" -ForegroundColor Red }
            # first VUID class + first FAIL line as evidence
            $lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' } | Select-Object -First 2 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
            $lines | Where-Object { $_ -match '^FAIL:' } | Select-Object -First 2 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        }
    }
    Write-Host "`n=== verify-wsi-lifecycle overall: $(if ($all) {'PASS'} else {'FAIL'}) ==="
    exit $(if ($all) { 0 } else { 1 })
} catch {
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    $env:PATH = $oldPath
    if ($null -ne $oldLayer) { $env:VK_LAYER_PATH = $oldLayer } else { Remove-Item Env:VK_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldImplicit) { $env:VK_IMPLICIT_LAYER_PATH = $oldImplicit } else { Remove-Item Env:VK_IMPLICIT_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldDisable) { $env:CC_VULKAN_DISABLE_FEATURES = $oldDisable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }
    if ((Get-Location).Path -ne $oldLoc.Path) { Pop-Location }
}
