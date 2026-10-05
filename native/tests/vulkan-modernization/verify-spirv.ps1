# ============================================================================
# SPIR-V target verification entry (M0-B / hardened in M0-C)
#
# Runs the bundled-glslang compile harness per REQUESTED target (1.1/1.2/1.3),
# then validates every produced .spv with the Vulkan SDK spirv-val under the
# MATCHING --target-env, plus an independent SPIR-V header check (magic,
# version word, >= 5 words, size % 4 == 0) so a SPV 1.5 blob can never be
# passed off as a 1.6 result.
#
# Anti-masking: every normal run writes into a FRESH run-* subdirectory and
# only that directory is validated. Stale products from earlier runs can never
# satisfy the current run.
#
# Exit codes (non-zero always propagates):
#   0 = every requested target: harness PASS + exactly vertex/fragment/compute
#       products + header checks + spirv-val PASS
#   1 = any failure (harness FAIL/crash, missing dir/file in the matrices that
#       must be complete, header mismatch, spirv-val error, unknown target)
#   2 = at least one target UNSUPPORTED by the bundled glslang and nothing
#       failed otherwise (expected for 1.3 on glslang 11.5.0; NOT a PASS)
#
# Usage examples:
#   powershell -ExecutionPolicy Bypass -File verify-spirv.ps1
#   powershell -ExecutionPolicy Bypass -File verify-spirv.ps1 -Targets 1.1,1.2
#   powershell -ExecutionPolicy Bypass -File verify-spirv.ps1 -SkipHarnessRun -Targets 1.1,1.2 -OutDir spirv-out\run-XXXX
# ============================================================================

[CmdletBinding()]
param(
    [string]$HarnessExe = "",
    [string]$OutDir = "",
    [string]$SpirvVal = "",
    [string[]]$Targets = @("1.1", "1.2", "1.3"),
    [switch]$SkipHarnessRun
)

$ErrorActionPreference = 'Stop'
$validTargets = @("1.1", "1.2", "1.3")
$expectedWord = @{ "1.1" = 0x00010300; "1.2" = 0x00010500; "1.3" = 0x00010600 }
$magicWord = 0x07230203
$stages = @("vertex", "fragment", "compute")

function Fail([string]$msg) { Write-Host "  FAIL: $msg" -ForegroundColor Red; script:Set-Variable -Name 'script:anyFail' -Value $true -Scope Script }

# --- validate parameters -----------------------------------------------------
# Normalize: -File invocation passes "1.1,1.2" as ONE string; split comma lists.
$normalized = @()
foreach ($t in $Targets) { foreach ($p in ($t -split ',')) { $p = $p.Trim(); if ($p) { $normalized += $p } } }
$Targets = $normalized
if ($Targets.Count -eq 0) { Write-Error "No targets selected."; exit 1 }
foreach ($t in $Targets) {
    if ($validTargets -notcontains $t) {
        Write-Error "Unknown target '$t'. Valid targets: $($validTargets -join ', ')"
        exit 1
    }
}

# --- resolve spirv-val -------------------------------------------------------
if (-not $SpirvVal) {
    if ($env:VULKAN_SDK -and (Test-Path "$env:VULKAN_SDK\Bin\spirv-val.exe")) {
        $SpirvVal = "$env:VULKAN_SDK\Bin\spirv-val.exe"
    } else {
        Write-Error "spirv-val not found: pass -SpirvVal <path> or set VULKAN_SDK."
        exit 1
    }
}
if (-not (Test-Path $SpirvVal)) { Write-Error "spirv-val not found at '$SpirvVal'"; exit 1 }

# --- resolve harness ---------------------------------------------------------
if (-not $SkipHarnessRun) {
    if (-not $HarnessExe) {
        $candidate = Join-Path $PSScriptRoot "build\win64-vs\Release\spirv_target_harness.exe"
        if (Test-Path $candidate) { $HarnessExe = $candidate }
        else { Write-Error "Harness exe not found at '$candidate'; build first or pass -HarnessExe."; exit 1 }
    }
    if (-not (Test-Path $HarnessExe)) { Write-Error "Harness exe not found at '$HarnessExe'"; exit 1 }
}

$script:anyFail = $false
$script:anyUnsupported = $false
$script:produced = @()   # files to spirv-val afterwards

if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "spirv-out" }

if ($SkipHarnessRun) {
    # --- validate an EXISTING directory against the COMPLETE selected matrix --
    # No UNSUPPORTED concept here: for every selected target the dir and all
    # three stage products must exist. Anything missing is a FAIL.
    Write-Host "=== [1/2] matrix check on existing dir: $OutDir ===" -ForegroundColor Cyan
    foreach ($t in $Targets) {
        $dir = Join-Path $OutDir "vk$t"
        if (-not (Test-Path $dir)) { Fail "missing output dir $dir for target $t (matrix incomplete)"; continue }
        foreach ($s in $stages) {
            $f = Join-Path $dir "$s.spv"
            if (-not (Test-Path $f)) { Fail "missing product $f for target $t"; continue }
            $script:produced += [pscustomobject]@{ File = $f; Target = $t }
        }
    }
} else {
    # --- run the harness into a FRESH run directory (anti-masking) -----------
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $runDir = Join-Path $OutDir "run-$stamp"
    Write-Host "=== [1/2] harness runs (bundled glslang) -> $runDir ===" -ForegroundColor Cyan

    foreach ($t in $Targets) {
        Write-Host "--- target $t ---"
        & $HarnessExe $t --out-dir $runDir
        $code = $LASTEXITCODE
        Write-Host "  harness exit code for ${t}: $code"
        if ($code -eq 0) {
            # PASS verdict requires exactly the three stage products IN THIS RUN
            $dir = Join-Path $runDir "vk$t"
            if (-not (Test-Path $dir)) { Fail "target $t reported PASS but produced no directory"; continue }
            foreach ($s in $stages) {
                $f = Join-Path $dir "$s.spv"
                if (-not (Test-Path $f)) { Fail "target $t reported PASS but product missing: $f"; continue }
                $script:produced += [pscustomobject]@{ File = $f; Target = $t }
            }
        } elseif ($code -eq 2) {
            Write-Host "  target $t : UNSUPPORTED by bundled glslang (NOT a PASS)" -ForegroundColor Yellow
            $script:anyUnsupported = $true
        } else {
            Fail "target $t harness failed with exit $code"
        }
    }
}

# --- independent header verification -----------------------------------------
Write-Host "=== [2/2] SPIR-V header + spirv-val verification ===" -ForegroundColor Cyan
foreach ($p in $script:produced) {
    $bytes = [System.IO.File]::ReadAllBytes($p.File)
    if ($bytes.Length -lt 20) { Fail "$($p.File): shorter than a 5-word SPIR-V header ($($bytes.Length) bytes)"; continue }
    if (($bytes.Length % 4) -ne 0) { Fail "$($p.File): size $($bytes.Length) not a multiple of 4"; continue }
    $w0 = [BitConverter]::ToUInt32($bytes, 0)
    $w1 = [BitConverter]::ToUInt32($bytes, 4)
    $want = $expectedWord[$p.Target]
    if ($w0 -ne $magicWord) { Fail ("{0}: BAD MAGIC 0x{1:X8}" -f $p.File, $w0); continue }
    if ($w1 -ne $want) {
        Fail ("{0}: version word 0x{1:X8} != expected 0x{2:X8} (cross-target substitution forbidden)" -f $p.File, $w1, $want)
        continue
    }
    Write-Host ("  header OK: {0} magic=0x{1:X8} version=0x{2:X8} (target {3})" -f (Split-Path $p.File -Leaf), $w0, $w1, $p.Target) -ForegroundColor Green

    & $SpirvVal --target-env "vulkan$($p.Target)" $p.File
    if ($LASTEXITCODE -ne 0) {
        Fail "spirv-val exit $LASTEXITCODE for $($p.File) env=vulkan$($p.Target)"
    } else {
        Write-Host "  spirv-val PASS: $(Split-Path $p.File -Leaf) env=vulkan$($p.Target)" -ForegroundColor Green
    }
}

# --- verdict ------------------------------------------------------------------
$exit = 0
if ($script:anyFail) { $exit = 1 }
elseif ($script:anyUnsupported) { $exit = 2 }
Write-Host ""
Write-Host "=== verification summary ==="
Write-Host "  targets requested : $($Targets -join ', ')"
Write-Host "  fail flag         : $($script:anyFail)"
Write-Host "  unsupported flag  : $($script:anyUnsupported)"
Write-Host "  overall exit      : $exit"
if ($exit -eq 2) {
    Write-Host "  NOTE: exit 2 = at least one target UNSUPPORTED by the bundled glslang" -ForegroundColor Yellow
    Write-Host "        (expected for target 1.3 on glslang 11.5.0). It is NOT a PASS." -ForegroundColor Yellow
}
exit $exit
