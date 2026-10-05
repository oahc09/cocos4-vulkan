# ============================================================================
# Real capability-negotiation acceptance matrix (M1-D, revised)
#
# Positive cases: engine exit 0 + effective API matches + validation clean +
# SPIR-V 9/9 via verify-spirv.ps1.
# Negative cases: exit EXACTLY 1, log contains the case's correct rejection
# reason AND "FAIL: Cocos Vulkan initialization". Crashes (NTSTATUS), other
# exit codes, missing logs/reasons are FAILURES - a non-zero exit alone is
# NOT acceptance evidence.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify-capabilities.ps1
#     [-Cases default,force-1.1,...]   # run a subset; unknown names rejected
#     [-SelfTest]                       # exit 0 only if invalid fixtures are
#                                       # rejected by the real verdict helper
#
# Environment is snapshotted and restored in finally; no registry changes.
# ============================================================================

[CmdletBinding()]
param(
    [string]$EngineExe = "",
    [string]$OutRoot = "",
    [string]$DefaultExpectedEffective = "1.3",
    [string]$SpirvVerify = "",
    [string[]]$Cases = @(),
    [switch]$SelfTest
)

$ErrorActionPreference = 'Continue'   # WinPS 5.1: Stop + native stderr would throw

# --- resolve paths -----------------------------------------------------------
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$userProvidedExe = -not [string]::IsNullOrEmpty($EngineExe)
if (-not $EngineExe) {
    $EngineExe = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator\vulkan-engine-smoke\Release\vulkan_engine_smoke.exe'
}
if (-not $SpirvVerify) { $SpirvVerify = Join-Path $repo 'native\tests\vulkan-modernization\verify-spirv.ps1' }
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'native\build\vulkan-modernization\capability-matrix' }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$runRoot = Join-Path $OutRoot "run-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$script:results = @()

function Add-Result([string]$name, [int]$exit, [string]$expected, [bool]$pass) {
    $script:results += [pscustomobject]@{ Case = $name; Exit = $exit; Expected = $expected; Pass = $pass }
    if (-not $pass) { $script:scriptFailedAny = $true }
}

function Test-NegativeRejection([int]$code, [string]$logText, [string]$reason) {
    return ($code -eq 1) -and ($logText -match $reason) -and
           ($logText -match 'FAIL: Cocos Vulkan initialization')
}

function Test-ClassicPsoEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^CLASSIC_PSO:[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^CLASSIC_PSO: calls=2 forwarded=2 succeeded=2 subpasses=0,1 metadata=OK\r?$')
}

function Test-LayoutKeyEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^LAYOUT_KEYS[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^LAYOUT_KEYS: sets=3 pipelines=3 driver=SUCCESS canonical=OK ordered=OK\r?$')
}

function Test-ShaderKeyEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^SHADER_KEYS[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^SHADER_KEYS: stages=2 driver=SUCCESS code=EXACT ownership=OK\r?$')
}

function Test-ComputeKeyEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^COMPUTE_KEYS[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^COMPUTE_KEYS: pipelines=3 modules=2 driver=SUCCESS equal=OK distinct=OK ownership=OK\r?$')
}

function Test-ClassicRenderPassKeyEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^CLASSIC_RP_KEY[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^CLASSIC_RP_KEY: calls=1 driver=SUCCESS subpasses=2 content=EXACT ownership=OK\r?$')
}

function Test-GraphicsKeyEvidence([string]$logText) {
    $lines = @([regex]::Matches($logText, '(?m)^GRAPHICS_KEYS[^\r\n]*\r?$'))
    return ($lines.Count -eq 1) -and
        ($lines[0].Value -match '^GRAPHICS_KEYS: pipelines=2 driver=SUCCESS content=EXACT subpasses=OK ownership=OK\r?$')
}

# --- matrix definition --------------------------------------------------------
$matrix = @(
    @{ Name = 'default';            Force = '';    Disable = '';                                                             Expect = $DefaultExpectedEffective },
    @{ Name = 'force-1.1';          Force = '1.1'; Disable = '';                                                             Expect = '1.1' },
    @{ Name = 'force-1.2';          Force = '1.2'; Disable = '';                                                             Expect = '1.2' },
    @{ Name = 'force-1.3';          Force = '1.3'; Disable = '';                                                             Expect = '1.3' },
    @{ Name = 'no-timeline';        Force = '1.3'; Disable = 'timelineSemaphore';                                               Expect = '1.3' },
    @{ Name = 'no-sync2';           Force = '1.3'; Disable = 'synchronization2';                                                 Expect = '1.3' },
    @{ Name = 'no-dynamic';         Force = '1.3'; Disable = 'dynamicRendering';                                                 Expect = '1.3' },
    @{ Name = 'no-m4';              Force = '1.3'; Disable = 'maintenance4';                                                     Expect = '1.3' },
    @{ Name = 'all-off';            Force = '1.3'; Disable = 'timelineSemaphore,synchronization2,dynamicRendering,maintenance4'; Expect = '1.3' },
    @{ Name = 'neg-force-1.0';      Force = '1.0'; Disable = '';     Neg = $true; Reason = 'Invalid CC_VULKAN_FORCE_API: 1\.0' },
    @{ Name = 'neg-unknown-disable'; Force = '1.3'; Disable = 'bogusToken'; Neg = $true; Reason = 'Unknown CC_VULKAN_DISABLE_FEATURES entry: bogusToken' }
)

if ($Cases.Count -gt 0) {
    # powershell -File passes a comma-separated CLI value as one string.
    $Cases = @($Cases | ForEach-Object { $_ -split ',' })
    $valid = $matrix | ForEach-Object { $_.Name }
    foreach ($c in $Cases) {
        if ($valid -notcontains $c) {
            Write-Error "unknown case '$c'. valid: $($valid -join ', ')"
            exit 1
        }
    }
    $matrix = $matrix | Where-Object { $Cases -contains $_.Name }
}

# --- fixture pseudo-cases (SelfTest only) -------------------------------------
# They replace the engine with harmless fixtures in the ignored dir and MUST
# be judged FAIL by the negative logic below: an unrelated exit 1 (no engine
# rejection reason in the log) and a simulated NTSTATUS crash can never pass.
$fixtures = @()
if ($SelfTest) {
    $fixtures = @(
        @{ Name = 'fixture-unrelated-exit1'; Command = 'cmd /c exit 1';         Reason = 'never-present-reason' },
        @{ Name = 'fixture-ntstatus-crash';  Command = 'powershell -NoProfile -Command exit -1073740940'; Reason = 'fixture-rejection' }
    )
}

# --- environment snapshot ------------------------------------------------------
$oldPath = $env:PATH
$oldLayerPath = $env:VK_LAYER_PATH
$oldImplicit = $env:VK_IMPLICIT_LAYER_PATH
$oldForce = $env:CC_VULKAN_FORCE_API
$oldDisable = $env:CC_VULKAN_DISABLE_FEATURES
$script:scriptFailedAny = $false

try {
    if (-not $userProvidedExe) {
        # default standard tree: build the CURRENT target first; a red
        # build must fail here - never judge a stale exe as current source.
        Write-Host "building vulkan_engine_smoke (default tree)..." -ForegroundColor Cyan
        & cmake --build (Join-Path $repo 'native\build\vulkan-modernization\win64-simulator') --config Release --target vulkan_engine_smoke --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "vulkan_engine_smoke build failed (exit $LASTEXITCODE); refusing to run a stale exe" }
    } else {
        Write-Host "external -EngineExe provided: caller is responsible for building it" -ForegroundColor Yellow
    }
    if (-not (Test-Path $EngineExe)) { throw "engine exe not found: $EngineExe (build it first)" }
    if (-not (Test-Path $SpirvVerify)) { throw "verify-spirv.ps1 not found: $SpirvVerify" }

    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $emptyImplicit = Join-Path $repo 'native\build\vulkan-modernization\empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $emptyImplicit | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $emptyImplicit
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH

    foreach ($case in $matrix) {
        $name = $case.Name
        $caseFailed = $false
        Write-Host "`n=== case ${name}: FORCE='$($case.Force)' DISABLE='$($case.Disable)' ===" -ForegroundColor Cyan
        if ($case.Force) { $env:CC_VULKAN_FORCE_API = $case.Force } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
        if ($case.Disable) { $env:CC_VULKAN_DISABLE_FEATURES = $case.Disable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }

        $caseDir = Join-Path $runRoot $name
        $log = "$caseDir.log"
        cmd /c """$EngineExe"" ""$caseDir"" > ""$log"" 2>&1"   # native stream merge (WinPS 5.1)
        $code = $LASTEXITCODE

        if (-not (Test-Path $log)) {
            Write-Host "  FAIL: no log produced for $name" -ForegroundColor Red
            Add-Result $name $code 'log' $false
            continue
        }
        $text = Get-Content $log -Raw

        if ($case.Neg) {
            # exact exit 1, correct rejection reason, engine FAIL line
            $caseFailed = -not (Test-NegativeRejection $code $text $case.Reason)
            if ($code -ne 1) {
                Write-Host "  FAIL: $name exited $code (must be EXACTLY 1; NTSTATUS/crash is not acceptance)" -ForegroundColor Red
                $caseFailed = $true
            }
            if ($text -notmatch $case.Reason) {
                Write-Host "  FAIL: $name log missing rejection reason /"$($case.Reason)/"" -ForegroundColor Red
                $caseFailed = $true
            }
            if ($text -notmatch 'FAIL: Cocos Vulkan initialization') {
                Write-Host "  FAIL: $name log missing engine FAIL line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not $caseFailed) { Write-Host "  OK: clean exit 1 with correct rejection reason" -ForegroundColor Green }
            Add-Result $name $code '1' (-not $caseFailed)
        } else {
            if ($code -ne 0) { Write-Host "  FAIL: engine exit $code for $name" -ForegroundColor Red; $caseFailed = $true }
            $m = [regex]::Match($text, 'Cocos effective API: (\d+\.\d+)')
            if (-not $m.Success) { Write-Host "  FAIL: no effective API line in $name log" -ForegroundColor Red; $caseFailed = $true }
            elseif ($m.Groups[1].Value -ne $case.Expect) { Write-Host "  FAIL: $name effective API $($m.Groups[1].Value) != expected $($case.Expect)" -ForegroundColor Red; $caseFailed = $true }
            if ($text -notmatch 'validation \+ best practices \+ sync: ENABLED') { Write-Host "  FAIL: $name validation not ENABLED" -ForegroundColor Red; $caseFailed = $true }
            if ($text -notmatch 'validation errors=0') { Write-Host "  FAIL: $name validation errors != 0" -ForegroundColor Red; $caseFailed = $true }
            if ($text -notmatch 'forced tier does NOT emulate a low-version GPU') { Write-Host "  FAIL: $name missing physical-vs-effective note" -ForegroundColor Red; $caseFailed = $true }
            if (-not (Test-ClassicPsoEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid CLASSIC_PSO evidence line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not (Test-LayoutKeyEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid LAYOUT_KEYS evidence line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not (Test-ShaderKeyEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid SHADER_KEYS evidence line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not (Test-ComputeKeyEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid COMPUTE_KEYS evidence line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not (Test-ClassicRenderPassKeyEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid CLASSIC_RP_KEY evidence line" -ForegroundColor Red
                $caseFailed = $true
            }
            if (-not (Test-GraphicsKeyEvidence $text)) {
                Write-Host "  FAIL: $name requires exactly one valid GRAPHICS_KEYS evidence line" -ForegroundColor Red
                $caseFailed = $true
            }

            & powershell -NoProfile -ExecutionPolicy Bypass -File $SpirvVerify -SkipHarnessRun -Targets 1.1,1.2,1.3 -OutDir $caseDir *> "$caseDir-spirv.log"
            if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL: verify-spirv exit $LASTEXITCODE for $name" -ForegroundColor Red; $caseFailed = $true }

            if (-not $caseFailed) { Write-Host "  OK: exit 0, effective=$($m.Groups[1].Value), validation clean, SPIR-V 9/9" -ForegroundColor Green }
            Add-Result $name $code '0' (-not $caseFailed)
        }
    }

    # SelfTest marker-parser fixtures: no marker / duplicate marker / fake
    # footer must not satisfy the PSO evidence requirement.
    if ($SelfTest) {
        $markerOk = 'CLASSIC_PSO: calls=2 forwarded=2 succeeded=2 subpasses=0,1 metadata=OK'
        $synthetics = @(
            @{ Name = 'fixture-pso-no-marker';     Text = "Cocos effective API: 1.3`n" },
            @{ Name = 'fixture-pso-duplicate';     Text = "$markerOk`n$markerOk`n" },
            @{ Name = 'fixture-pso-fake-footer';   Text = "$markerOk extra junk`n" },
            @{ Name = 'fixture-pso-valid-plus-bad'; Text = "$markerOk`nCLASSIC_PSO: calls=0 forwarded=0 succeeded=0 subpasses=0,1 metadata=BAD`n" },
            @{ Name = 'fixture-pso-valid';         Text = "$markerOk`n" }
        )
        foreach ($syn in $synthetics) {
            $expectPass = ($syn.Name -eq 'fixture-pso-valid')
            $judged = Test-ClassicPsoEvidence $syn.Text
            $pass = ($judged -eq $expectPass)
            Write-Host ("  selftest {0}: judged={1} expected={2} -> {3}" -f $syn.Name, $judged, $expectPass, $(if ($pass) {'OK'} else {'FAIL'}))
            Add-Result $syn.Name 0 'parser' $pass
        }
        $layoutOk = 'LAYOUT_KEYS: sets=3 pipelines=3 driver=SUCCESS canonical=OK ordered=OK'
        $layoutFixtures = @(
            @{ Name = 'layout-valid'; Text = "$layoutOk`n"; Expected = $true },
            @{ Name = 'layout-missing'; Text = ''; Expected = $false },
            @{ Name = 'layout-duplicate'; Text = "$layoutOk`n$layoutOk`n"; Expected = $false },
            @{ Name = 'layout-junk'; Text = "$layoutOk junk`n"; Expected = $false },
            @{ Name = 'layout-malformed'; Text = "$layoutOk`nLAYOUT_KEYS_bad`n"; Expected = $false },
            @{ Name = 'layout-wrong-count'; Text = ($layoutOk -replace 'sets=3', 'sets=0'); Expected = $false }
        )
        foreach ($fixture in $layoutFixtures) {
            $pass = ((Test-LayoutKeyEvidence $fixture.Text) -eq $fixture.Expected)
            Add-Result $fixture.Name 0 'parser' $pass
            Write-Host ("  selftest {0}: {1}" -f $fixture.Name, $(if ($pass) {'OK'} else {'FAIL'}))
        }
        $shaderOk = 'SHADER_KEYS: stages=2 driver=SUCCESS code=EXACT ownership=OK'
        $shaderFixtures = @(
            @{ Name = 'shader-valid'; Text = "$shaderOk`n"; Expected = $true },
            @{ Name = 'shader-missing'; Text = ''; Expected = $false },
            @{ Name = 'shader-duplicate'; Text = "$shaderOk`n$shaderOk`n"; Expected = $false },
            @{ Name = 'shader-junk'; Text = "$shaderOk junk`n"; Expected = $false },
            @{ Name = 'shader-malformed'; Text = "$shaderOk`nSHADER_KEYS_bad`n"; Expected = $false },
            @{ Name = 'shader-wrong-count'; Text = ($shaderOk -replace 'stages=2', 'stages=0'); Expected = $false }
        )
        foreach ($fixture in $shaderFixtures) {
            $pass = ((Test-ShaderKeyEvidence $fixture.Text) -eq $fixture.Expected)
            Add-Result $fixture.Name 0 'parser' $pass
            Write-Host ("  selftest {0}: {1}" -f $fixture.Name, $(if ($pass) {'OK'} else {'FAIL'}))
        }
        $computeOk = 'COMPUTE_KEYS: pipelines=3 modules=2 driver=SUCCESS equal=OK distinct=OK ownership=OK'
        $computeFixtures = @(
            @{ Name = 'compute-valid'; Text = "$computeOk`n"; Expected = $true },
            @{ Name = 'compute-missing'; Text = ''; Expected = $false },
            @{ Name = 'compute-duplicate'; Text = "$computeOk`n$computeOk`n"; Expected = $false },
            @{ Name = 'compute-junk'; Text = "$computeOk junk`n"; Expected = $false },
            @{ Name = 'compute-malformed'; Text = "$computeOk`nCOMPUTE_KEYS_bad`n"; Expected = $false },
            @{ Name = 'compute-wrong-count'; Text = ($computeOk -replace 'pipelines=3', 'pipelines=0'); Expected = $false },
            @{ Name = 'compute-bad-equality'; Text = ($computeOk -replace 'equal=OK', 'equal=BAD'); Expected = $false },
            @{ Name = 'compute-no-driver-success'; Text = ($computeOk -replace 'driver=SUCCESS', 'driver=FAILED'); Expected = $false }
        )
        foreach ($fixture in $computeFixtures) {
            $pass = ((Test-ComputeKeyEvidence $fixture.Text) -eq $fixture.Expected)
            Add-Result $fixture.Name 0 'parser' $pass
            Write-Host ("  selftest {0}: {1}" -f $fixture.Name, $(if ($pass) {'OK'} else {'FAIL'}))
        }
        $renderPassOk = 'CLASSIC_RP_KEY: calls=1 driver=SUCCESS subpasses=2 content=EXACT ownership=OK'
        $renderPassFixtures = @(
            @{ Name = 'render-pass-valid'; Text = "$renderPassOk`n"; Expected = $true },
            @{ Name = 'render-pass-missing'; Text = ''; Expected = $false },
            @{ Name = 'render-pass-duplicate'; Text = "$renderPassOk`n$renderPassOk`n"; Expected = $false },
            @{ Name = 'render-pass-junk'; Text = "$renderPassOk junk`n"; Expected = $false },
            @{ Name = 'render-pass-malformed'; Text = "$renderPassOk`nCLASSIC_RP_KEY_bad`n"; Expected = $false },
            @{ Name = 'render-pass-wrong-count'; Text = ($renderPassOk -replace 'calls=1', 'calls=0'); Expected = $false },
            @{ Name = 'render-pass-bad-content'; Text = ($renderPassOk -replace 'content=EXACT', 'content=BAD'); Expected = $false },
            @{ Name = 'render-pass-failed-driver'; Text = ($renderPassOk -replace 'driver=SUCCESS', 'driver=FAILED'); Expected = $false }
        )
        foreach ($fixture in $renderPassFixtures) {
            $pass = ((Test-ClassicRenderPassKeyEvidence $fixture.Text) -eq $fixture.Expected)
            Add-Result $fixture.Name 0 'parser' $pass
            Write-Host ("  selftest {0}: {1}" -f $fixture.Name, $(if ($pass) {'OK'} else {'FAIL'}))
        }
        $graphicsOk = 'GRAPHICS_KEYS: pipelines=2 driver=SUCCESS content=EXACT subpasses=OK ownership=OK'
        $graphicsFixtures = @(
            @{ Name = 'graphics-valid'; Text = "$graphicsOk`n"; Expected = $true },
            @{ Name = 'graphics-missing'; Text = ''; Expected = $false },
            @{ Name = 'graphics-duplicate'; Text = "$graphicsOk`n$graphicsOk`n"; Expected = $false },
            @{ Name = 'graphics-junk'; Text = "$graphicsOk junk`n"; Expected = $false },
            @{ Name = 'graphics-malformed'; Text = "$graphicsOk`nGRAPHICS_KEYS_bad`n"; Expected = $false },
            @{ Name = 'graphics-wrong-count'; Text = ($graphicsOk -replace 'pipelines=2', 'pipelines=0'); Expected = $false },
            @{ Name = 'graphics-bad-subpass'; Text = ($graphicsOk -replace 'subpasses=OK', 'subpasses=BAD'); Expected = $false },
            @{ Name = 'graphics-failed-driver'; Text = ($graphicsOk -replace 'driver=SUCCESS', 'driver=FAILED'); Expected = $false }
        )
        foreach ($fixture in $graphicsFixtures) {
            $pass = ((Test-GraphicsKeyEvidence $fixture.Text) -eq $fixture.Expected)
            Add-Result $fixture.Name 0 'parser' $pass
            Write-Host ("  selftest {0}: {1}" -f $fixture.Name, $(if ($pass) {'OK'} else {'FAIL'}))
        }
    }

    foreach ($f in $fixtures) {
        $name = $f.Name
        Write-Host "`n=== selftest fixture ${name} ===" -ForegroundColor Cyan
        $caseDir = Join-Path $runRoot $name
        $log = "$caseDir.log"
        cmd /c "$($f.Command) > ""$log"" 2>&1"
        $code = $LASTEXITCODE
        $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
        # The crash fixture has every required marker, so only the exit-code
        # guard can reject it. Both use the real case verdict helper.
        if ($name -eq 'fixture-ntstatus-crash') {
            $text = "fixture-rejection`nFAIL: Cocos Vulkan initialization"
        }
        $judgedPass = Test-NegativeRejection $code $text $f.Reason
        if ($judgedPass) {
            Write-Host "  SELFTEST FAIL: $name would have been judged PASS (logic broken)" -ForegroundColor Red
            Add-Result $name $code 'judged-FAIL' $false
        } else {
            Write-Host "  SELFTEST OK: $name correctly rejected by the real verdict helper (exit $code)" -ForegroundColor Green
            Add-Result $name $code 'judged-FAIL' $true
        }
    }
} catch {
    Write-Host "UNHANDLED FAILURE: $($_.Exception.Message)" -ForegroundColor Red
    $script:scriptFailedAny = $true
} finally {
    $env:PATH = $oldPath
    if ($null -ne $oldLayerPath) { $env:VK_LAYER_PATH = $oldLayerPath } else { Remove-Item Env:VK_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldImplicit) { $env:VK_IMPLICIT_LAYER_PATH = $oldImplicit } else { Remove-Item Env:VK_IMPLICIT_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldForce) { $env:CC_VULKAN_FORCE_API = $oldForce } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
    if ($null -ne $oldDisable) { $env:CC_VULKAN_DISABLE_FEATURES = $oldDisable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }
}

Write-Host "`n=== capability matrix summary ($runRoot) ===" -ForegroundColor Cyan
$script:results | Format-Table -AutoSize
$exit = if ($script:scriptFailedAny) { 1 } else { 0 }
Write-Host "overall exit: $exit"
exit $exit
