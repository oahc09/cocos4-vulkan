# Real Win32 WSI stress harness verification (M2-E, strict criteria extracted)
#
# Builds the vulkan_wsi_smoke target (no pre-existing exe required), runs it
# with SDK validation layers + third-party implicit-layer isolation + engine
# DLLs on PATH. All environment/cwd changes are restored in finally.
#
# STRICT PASS (Test-WsiLog, shared by real runs AND fixtures):
#   - exit code 0
#   - validation REQUIRED ("validation + best practices + sync: ENABLED")
#   - EXACTLY ONE completion marker of each scenario S1..S8 (duplicates fail)
#   - EXACTLY ONE final footer "WSI smoke: validation errors=0 ... failures=0"
#     (missing footer or footer with errors/failures != 0 fails)
#   - no "SCENARIOS SKIPPED" / "[INCOMPLETE]"
#   - no VUID- / SYNC-HAZARD- lines, no harness "FAIL:" lines
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify-wsi.ps1            # single default run
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify-wsi.ps1 -SelfTest  # fixture self-check (must exit 0)
#   powershell -NoProfile -ExecutionPolicy Bypass -File verify-wsi.ps1 -Matrix    # default + 1.1/1.2/1.3 + maintenance1-off
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Log = "",
    [switch]$SelfTest,
    [switch]$Matrix,
    [ValidateSet('legacy','submit2')][string]$Submit='legacy',
    [ValidateSet('legacy','sync2')][string]$Barrier='legacy',
    [ValidateSet('auto','fence','timeline')][string]$Completion='auto'
)

$ErrorActionPreference = 'Continue'
if ($Matrix -and $Log) { Write-Error "-Log cannot be combined with -Matrix (each matrix run writes its own unique log)"; exit 1 }
if ($Matrix -and ($Submit -eq 'submit2' -or $Barrier -eq 'sync2')) {Write-Error '现代编码器不能用于当前包含未启用 Sync2 的 1.1/1.2 矩阵；请使用单跑进行 A/B。';exit 1}
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator' }
$vmRoot = Join-Path $repo 'native\build\vulkan-modernization'   # ignored artifacts area
New-Item -ItemType Directory -Force -Path $vmRoot | Out-Null

# ---------------------------------------------------------------------------
# Strict log verdict - the ONLY judge, used by real runs and fixtures alike.
# ---------------------------------------------------------------------------
function Test-WsiLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$ExpectedEffective = "",
        [string]$ExpectedMaintenance = "",   # "on" / "off"
        [string]$ExpectedSubmit='legacy', [string]$ExpectedBarrier='legacy', [string]$ExpectedCompletion='auto' 
    )
    $reasons = @()

    if ($ExitCode -ne 0) { $reasons += "exit code $ExitCode != 0" }
    # actual effective tier from the engine's version log line
    if ($ExpectedEffective) {
        $m = $Lines | Where-Object { $_ -match 'effective=([0-9]+\.[0-9]+)' } | Select-Object -First 1
        if (-not $m) { $reasons += "no 'Vulkan API: ... effective=' line found" }
        elseif ($m -notmatch "effective=$ExpectedEffective(\s|$)") { $reasons += "actual effective line '$($m.Trim())' does not match expected $ExpectedEffective" }
    }
    # actual swapchainMaintenance1 tri-state
    if ($ExpectedMaintenance) {
        $c = $Lines | Where-Object { $_ -match 'swapchainMaintenance1: supported=' } | Select-Object -First 1
        if (-not $c) { $reasons += "no swapchainMaintenance1 capability line found" }
        else {
            $wantReq = $(if ($ExpectedMaintenance -eq 'on') {'1'} else {'0'})
            $wantEn  = $wantReq
            if ($c -notmatch "requested=$wantReq") { $reasons += "maintenance1 requested mismatch in '$($c.Trim())' (want $wantReq)" }
            if ($c -notmatch "enabled=$wantEn")  { $reasons += "maintenance1 enabled mismatch in '$($c.Trim())' (want $wantEn)" }
        }
    }
    if (-not ($Lines | Where-Object { $_ -match 'validation \+ best practices \+ sync: ENABLED' })) {
        $reasons += "validation not ENABLED"
    }
    # exactly one completion marker per scenario (S7 has a dedicated completion line)
    $markers = @{
        'S1' = '^S1 stable frames:'
        'S2' = '^S2 resizes:'
        'S3' = '^S3 zero-size'
        'S4' = '^S4 surface destroy/recreate done'
        'S5' = '^S5 two-window:'
        'S6' = '^S6 all-failed'
        'S7' = '^S7 result injection: completed'
        'S8' = '^S8 offscreen-between-frames: completed'
    }
    foreach ($k in $markers.Keys) {
        $n = ($Lines | Where-Object { $_ -match $markers[$k] }).Count
        if ($n -ne 1) { $reasons += "scenario $k marker count = $n (expected exactly 1)" }
    }
    # exactly one final footer, and it must be clean
    $footers = @($Lines | Where-Object { $_ -match '^WSI smoke: validation errors=' })
    if ($footers.Count -ne 1) {
        $reasons += "final footer count = $($footers.Count) (expected exactly 1)"
    } else {
        if ($footers[0] -cne 'WSI smoke: validation errors=0 (pre-teardown 0), failures=0') {$reasons+='unclean/malformed footer'}
    }
    $submitLines=@($Lines | Where-Object {$_ -match '^submit encoder='})
    $submitPattern=if ($ExpectedSubmit -eq 'submit2') {'^submit encoder=submit2 observed-legacy=0 observed-submit2=[1-9][0-9]*$'} else {'^submit encoder=legacy observed-legacy=[1-9][0-9]* observed-submit2=0$'}
    if ($submitLines.Count -ne 1 -or $submitLines[0] -notmatch $submitPattern) {$reasons+='actual submit routing mismatch'}
    $barrierLines=@($Lines | Where-Object {$_ -match '^barrier encoder='})
    $barrierPattern=if ($ExpectedBarrier -eq 'sync2') {'^barrier encoder=sync2 observed-legacy=0 observed-sync2=[1-9][0-9]*$'} else {'^barrier encoder=legacy observed-legacy=[1-9][0-9]* observed-sync2=0$'}
    if ($barrierLines.Count -ne 1 -or $barrierLines[0] -notmatch $barrierPattern) {$reasons+='actual barrier routing mismatch'}
    $leakLines=@($Lines | Where-Object {$_ -match 'Total device memory leaked:'})
    if ($leakLines.Count -ne 1 -or $leakLines[0] -notmatch 'Total device memory leaked: 0 bytes\.$') {$reasons+='missing/duplicate/nonzero leak'}
    $completionLines=@($Lines | Where-Object {$_ -match '^completion backend='})
    if ($completionLines.Count -ne 1 -or $completionLines[0] -notmatch '^completion backend=(fence|timeline) submits=([1-9][0-9]*) submission-fences=([0-9]+)$') {$reasons+='missing/duplicate completion counts'}
    else {
        $actualCompletion=$Matches[1];$submits=[UInt64]$Matches[2];$fences=[UInt64]$Matches[3]
        if (($actualCompletion -eq 'fence' -and $fences -ne $submits) -or ($actualCompletion -eq 'timeline' -and $fences -ne 0)) {$reasons+='invalid completion fence counts'}
        if ($ExpectedCompletion -ne 'auto' -and $actualCompletion -ne $ExpectedCompletion) {$reasons+='completion backend mismatch'}
    }
    if ($Lines | Where-Object { $_ -match 'SCENARIOS SKIPPED|\[INCOMPLETE\]' }) { $reasons += "run incomplete (SKIPPED/INCOMPLETE)" }
    $vuids = ($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count
    if ($vuids -gt 0) { $reasons += "$vuids VUID/hazard lines" }
    $fails = ($Lines | Where-Object { $_ -match '^FAIL:' }).Count
    if ($fails -gt 0) { $reasons += "$fails harness FAIL lines" }

    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons }
}

function Read-WsiLog([string]$path) {
    $text = [System.IO.File]::ReadAllText($path)
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    return @($text -split "`r?`n")
}

# ---------------------------------------------------------------------------
# SelfTest: fixtures exercise Test-WsiLog itself (a broken judge cannot pass).
# ---------------------------------------------------------------------------
function Invoke-SelfTest {
    # One valid base; negatives each change one condition of the real judge.
    $base=@(
        'Vulkan API: loader=1.4 requested=1.3 XR=1.3 instance=1.3 physical=1.4 effective=1.3',
        'validation + best practices + sync: ENABLED',
        'S1 stable frames: presented=120/120','S2 resizes: presented=30/30, final generation=31',
        'S3 zero-size suspend/restore done','S4 surface destroy/recreate done, generation=33',
        'S5 two-window: B presented=10/10 while A suspended','S6 all-failed acquire: empty submit + empty present done',
        'S7 result injection: completed','S8 offscreen-between-frames: completed (delta=0)',
        'submit encoder=legacy observed-legacy=231 observed-submit2=0',
        'barrier encoder=legacy observed-legacy=100 observed-sync2=0',
        'completion backend=timeline submits=232 submission-fences=0',
        'Total device memory leaked: 0 bytes.',
        'WSI smoke: validation errors=0 (pre-teardown 0), failures=0')
    $modern=@($base -replace 'submit encoder=legacy observed-legacy=231 observed-submit2=0','submit encoder=submit2 observed-legacy=0 observed-submit2=231' -replace 'barrier encoder=legacy observed-legacy=100 observed-sync2=0','barrier encoder=sync2 observed-legacy=0 observed-sync2=100')
    $fixtures=@(
        @{Name='valid-complete';Expect=$true;Lines=$base},
        @{Name='missing-footer';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch '^WSI smoke:'})},
        @{Name='footer-failures-1';Expect=$false;Lines=($base -replace 'failures=0','failures=1')},
        @{Name='footer-zeros-junk';Expect=$false;Lines=($base -replace 'failures=0','failures=01')},
        @{Name='duplicate-S1';Expect=$false;Lines=($base+$base[2])},
        @{Name='missing-S8';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch '^S8 '})},
        @{Name='ntstatus';Expect=$false;Lines=$base;ExitCode=-1073741819},
        @{Name='skipped';Expect=$false;Lines=($base+'SCENARIOS SKIPPED after error budget [INCOMPLETE]')},
        @{Name='duplicate-footer';Expect=$false;Lines=($base+$base[-1])},
        @{Name='wrong-effective';Expect=$false;Lines=($base -replace 'effective=1.3','effective=1.1');ExpectedEffective='1.3'},
        @{Name='maintenance-off-mismatch';Expect=$false;Lines=$base;ExpectedMaintenance='off'},
        @{Name='modern-valid';Expect=$true;Lines=$modern;ExpectedSubmit='submit2';ExpectedBarrier='sync2'},
        @{Name='wrong-submit';Expect=$false;Lines=$base;ExpectedSubmit='submit2'},
        @{Name='wrong-barrier';Expect=$false;Lines=$base;ExpectedBarrier='sync2'},
        @{Name='missing-submit';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch '^submit encoder='})},
        @{Name='missing-barrier';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch '^barrier encoder='})},
        @{Name='duplicate-submit';Expect=$false;Lines=($base+$base[10])},
        @{Name='duplicate-barrier';Expect=$false;Lines=($base+$base[11])},
        @{Name='zero-submit';Expect=$false;Lines=($base -replace 'observed-legacy=231','observed-legacy=0')},
        @{Name='zero-barrier';Expect=$false;Lines=($base -replace 'observed-legacy=100','observed-legacy=0')},
        @{Name='mixed-submit';Expect=$false;Lines=($base -replace 'observed-submit2=0','observed-submit2=1')},
        @{Name='mixed-barrier';Expect=$false;Lines=($base -replace 'observed-sync2=0','observed-sync2=1')},
        @{Name='missing-leak';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch 'Total device memory leaked:'})},
        @{Name='duplicate-leak';Expect=$false;Lines=($base+$base[13])},
        @{Name='nonzero-leak';Expect=$false;Lines=($base -replace 'leaked: 0','leaked: 64')},
        @{Name='missing-completion';Expect=$false;Lines=@($base | Where-Object {$_ -notmatch '^completion backend='})},
        @{Name='completion-mismatch';Expect=$false;Lines=$base;ExpectedCompletion='fence'},
        @{Name='wrong-fence-count';Expect=$false;Lines=($base -replace 'submission-fences=0','submission-fences=1')})

    $all = $true
    foreach ($f in $fixtures) {
        $args = @{ Lines = @($f.Lines); ExitCode = $f.ExitCode }
        if ($f.ExpectedEffective) { $args.ExpectedEffective = $f.ExpectedEffective }
        if ($f.ExpectedMaintenance) { $args.ExpectedMaintenance = $f.ExpectedMaintenance }
        if ($f.ExpectedSubmit) {$args.ExpectedSubmit=$f.ExpectedSubmit}
        if ($f.ExpectedBarrier) {$args.ExpectedBarrier=$f.ExpectedBarrier}
        if ($f.ExpectedCompletion) {$args.ExpectedCompletion=$f.ExpectedCompletion}
        if ($f.Name -eq 'wrong-effective') { $args.ExpectedEffective = '1.3' }
        $r = Test-WsiLog @args
        $ok = ($r.Pass -eq $f.Expect)
        Write-Host ("  selftest {0}: judge={1} expect={2} -> {3}" -f $f.Name, $r.Pass, $f.Expect, $(if ($ok) {'OK'} else {'BROKEN'}))
        if (-not $ok) { $all = $false; $r.Reasons | ForEach-Object { Write-Host "    reason: $_" } }
    }
    return $all
}

# ---------------------------------------------------------------------------
# One harness run (build once outside; env set/restored around the run).
# ---------------------------------------------------------------------------
function Invoke-WsiRun {
    param([string]$Name, [string]$ForceApi, [string]$Disable, [string]$Exe,
          [string]$ExpectedEffective = "", [string]$ExpectedMaintenance = "")

    if ($ForceApi) { $env:CC_VULKAN_FORCE_API = $ForceApi } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
    if ($Disable) { $env:CC_VULKAN_DISABLE_FEATURES = $Disable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }

    $log = Join-Path $vmRoot ("wsi-{0}-{1:yyyyMMdd-HHmmss-fff}-{2}.log" -f $Name, (Get-Date), $PID)
    cmd /c """$Exe"" > ""$log"" 2>&1"
    $code = $LASTEXITCODE

    $lines = if (Test-Path $log) { Read-WsiLog $log } else { @() }
    $verdict = Test-WsiLog -Lines @($lines) -ExitCode $code -ExpectedEffective $ExpectedEffective -ExpectedMaintenance $ExpectedMaintenance -ExpectedSubmit $Submit -ExpectedBarrier $Barrier -ExpectedCompletion $Completion

    # surface the run's own tier/feature evidence (never a substitute for the verdict)
    $lines | Where-Object { $_ -match 'harness config:|effective=|swapchainMaintenance1: supported=' } |
        Select-Object -First 3 | ForEach-Object { Write-Host "    $_" }

    $tag = if ($verdict.Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}: exit={2} log={3}" -f $tag, $Name, $code, (Split-Path $Log -Leaf))
    if (-not $verdict.Pass) { $verdict.Reasons | ForEach-Object { Write-Host "    reason: $_" -ForegroundColor Red }
        # first VUID classes for the report
        $lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' } | ForEach-Object { if ($_ -match '(VUID-[A-Za-z0-9_-]+|SYNC-HAZARD-[A-Z-]+)') { $Matches[1] } } |
            Group-Object | Sort-Object Count -Descending | Select-Object -First 6 | ForEach-Object { Write-Host ("    {0,3}x {1}" -f $_.Count, $_.Name) }
    }
    return $verdict.Pass
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
if ($SelfTest) {
    Write-Host "=== verify-wsi SelfTest ===" -ForegroundColor Cyan
    $ok = Invoke-SelfTest
    Write-Host ("SelfTest exit: {0}" -f $(if ($ok) {0} else {1}))
    exit $(if ($ok) { 0 } else { 1 })
}

$oldPath = $env:PATH; $oldLayer = $env:VK_LAYER_PATH; $oldImplicit = $env:VK_IMPLICIT_LAYER_PATH
$oldForce = $env:CC_VULKAN_FORCE_API; $oldDisable = $env:CC_VULKAN_DISABLE_FEATURES
$oldSubmit=$env:CC_VULKAN_SUBMIT;$oldBarrier=$env:CC_VULKAN_BARRIER;$oldCompletion=$env:CC_VULKAN_COMPLETION
$oldLoc = Get-Location
try {
    & cmake --build $BuildDir --config Release --target vulkan_wsi_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_wsi_smoke.exe'
    if (-not (Test-Path $exe)) { throw "harness exe not produced: $exe" }

    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $emptyImplicit = Join-Path $vmRoot 'empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $emptyImplicit | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $emptyImplicit
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    $env:CC_VULKAN_SUBMIT=$Submit; $env:CC_VULKAN_BARRIER=$Barrier
    $env:CC_VULKAN_COMPLETION=$Completion
    Push-Location $vmRoot   # run inside the ignored artifacts area

    $runs = @()
    if ($Matrix) {
        $runs = @(
            @{ Name = 'default';     ForceApi = '';    Disable = '';                   ExpectedEffective = '';  ExpectedMaintenance = '' },
            @{ Name = 'force-1.1';   ForceApi = '1.1'; Disable = '';                   ExpectedEffective = '1.1'; ExpectedMaintenance = 'on' },
            @{ Name = 'force-1.2';   ForceApi = '1.2'; Disable = '';                   ExpectedEffective = '1.2'; ExpectedMaintenance = 'on' },
            @{ Name = 'force-1.3';   ForceApi = '1.3'; Disable = '';                   ExpectedEffective = '1.3'; ExpectedMaintenance = 'on' },
            @{ Name = 'maint1-off';  ForceApi = '';    Disable = 'swapchainMaintenance1'; ExpectedEffective = ''; ExpectedMaintenance = 'off' }
        )
    } else {
        $runs = @( @{ Name = 'default'; ForceApi = ''; Disable = '' } )
    }

    $all = $true
    foreach ($r in $runs) {
        Write-Host "`n=== WSI run: $($r.Name) (FORCE='$($r.ForceApi)' DISABLE='$($r.Disable)') ===" -ForegroundColor Cyan
        if (-not (Invoke-WsiRun -Name $r.Name -ForceApi $r.ForceApi -Disable $r.Disable -Exe $exe -ExpectedEffective $r.ExpectedEffective -ExpectedMaintenance $r.ExpectedMaintenance)) { $all = $false }
    }
    Write-Host "`n=== verify-wsi overall: $(if ($all) {'PASS'} else {'FAIL'}) ===" -ForegroundColor $(if ($all) {'Green'} else {'Red'})
    exit $(if ($all) { 0 } else { 1 })
} catch {
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    $env:CC_VULKAN_SUBMIT=$oldSubmit;$env:CC_VULKAN_BARRIER=$oldBarrier;$env:CC_VULKAN_COMPLETION=$oldCompletion
    $env:PATH = $oldPath
    if ($null -ne $oldLayer) { $env:VK_LAYER_PATH = $oldLayer } else { Remove-Item Env:VK_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldImplicit) { $env:VK_IMPLICIT_LAYER_PATH = $oldImplicit } else { Remove-Item Env:VK_IMPLICIT_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldForce) { $env:CC_VULKAN_FORCE_API = $oldForce } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
    if ($null -ne $oldDisable) { $env:CC_VULKAN_DISABLE_FEATURES = $oldDisable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }
    if ((Get-Location).Path -ne $oldLoc.Path) { Pop-Location }
}
