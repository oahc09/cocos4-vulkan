# M6-A5: verify the D32 depth-only SAMPLE_ZERO MSAA resolve harness.
# Builds the CURRENT target first; runs each case in its own ignored cwd/log;
# one strict shared judge for real runs and SelfTest fixtures.
param(
    [string[]]$Cases = @(),
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$buildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator'
$exe = Join-Path $buildDir 'vulkan-engine-smoke\Release\vulkan_depth_resolve_smoke.exe'
$outRoot = Join-Path $repo 'native\build\vulkan-modernization\dsr-matrix'
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$runRoot = Join-Path $outRoot ("run-{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $PID)
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

function Test-DepthResolveLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$CaseName = '2x'
    )
    $reasons = @()
    if ($CaseName -cnotin @('2x','4x')) { return [pscustomobject]@{ Pass=$false; Reasons=@('unknown case') } }
    $samples = if ($CaseName -ceq '2x') { 2 } else { 4 }
    if ($ExitCode -ne 0) { $reasons += "exit $ExitCode != 0 (3=UNSUPPORTED is not PASS)" }
    if (@($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count) { $reasons += 'VUID/hazard' }
    if (@($Lines | Where-Object { $_ -match 'FAIL:' }).Count) { $reasons += 'FAIL lines (any format incl. timestamped/logger)' }
    if (@($Lines | Where-Object { $_ -match 'SKIPPED|INCOMPLETE|UNSUPPORTED' }).Count) { $reasons += 'SKIPPED/INCOMPLETE/UNSUPPORTED' }
    $valAny = @($Lines | Where-Object { $_ -match 'validation \+ best practices \+ sync' })
    $valExact = @($Lines | Where-Object { $_ -ceq 'validation + best practices + sync: ENABLED' })
    if ($valAny.Count -ne 1 -or $valExact.Count -ne 1) { $reasons += "validation not uniquely ENABLED (any=$($valAny.Count) exact=$($valExact.Count))" }
    # footer: exact clean, count ALL footer-like lines
    $footer = "dsr $CaseName completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN"
    $anyFooters = @($Lines | Where-Object { $_ -match 'completed:' -or $_ -match '^dsr .* completed' })
    if ($anyFooters.Count -ne 1) { $reasons += "footer count=$($anyFooters.Count)" }
    elseif ($anyFooters[0] -cne $footer) { $reasons += "footer not exact: $($anyFooters[0])" }
    # singletons (reserved-prefix counts first, then exact)
    $singletons = @(
        @{ P='dsr-config'; T="dsr-config: samples=$samples depthFormat=126 resolveMode=SAMPLE_ZERO pixels=187 hostBytes=1496 runtimeDynamic=1" }
        @{ P='dsr-modes'; T='dsr-modes: depth=1 stencil=1 (Classic filtered values)' }
        @{ P='dsr-pipelines'; T="dsr-pipelines: classic=2 dynamic=2 signatures=OK samples=$samples" }
        @{ P='dsr-compare'; T='dsr-compare: colorBytes=748 colorDiff=0 depthBytes=748 depthDiff=0' }
        @{ P='dsr-retirement'; T='dsr-retirement: created=4 destroyed=4' }
    )
    foreach ($s in $singletons) {
        $any = @($Lines | Where-Object { $_ -match [regex]::Escape($s.P) })
        $exact = @($Lines | Where-Object { $_ -ceq $s.T })
        if ($any.Count -ne 1 -or $exact.Count -ne 1) { $reasons += "invalid $($s.P) (any=$($any.Count) exact=$($exact.Count))" }
    }
    # two phase records: classic then dynamic, chain=1
    $nz = '(?:0x)?0*[1-9A-Fa-f][0-9A-Fa-f]*'
    $phases = @($Lines | Where-Object { $_ -match 'dsr-phase' })
    if ($phases.Count -ne 2) { $reasons += "phase count=$($phases.Count)" }
    else {
        $pc = "^dsr-phase: mode=classic cb=$nz rp=1/1 dynamic=0/0 draws=2 copies=1/1 submits=1/1 chain=1`$"
        $pd = "^dsr-phase: mode=dynamic cb=$nz rp=0/0 dynamic=1/1 draws=2 copies=1/1 submits=1/1 chain=1`$"
        if ($phases[0] -cnotmatch $pc) { $reasons += "classic phase: $($phases[0])" }
        if ($phases[1] -cnotmatch $pd) { $reasons += "dynamic phase: $($phases[1])" }
    }
    # two readback records: both goldens zero
    $readbacks = @($Lines | Where-Object { $_ -match 'dsr-readback' })
    if ($readbacks.Count -ne 2) { $reasons += "readback count=$($readbacks.Count)" }
    else {
        foreach ($rb in $readbacks) {
            if ($rb -cnotmatch '^dsr-readback: mode=(classic|dynamic) colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0$') {
                $reasons += "readback not clean: $rb"
            }
        }
        if ($readbacks[0] -cnotmatch 'mode=classic' -or $readbacks[1] -cnotmatch 'mode=dynamic') { $reasons += 'readback mode order' }
    }
    # leak line
    $leaks = @($Lines | Where-Object { $_ -match 'Total device memory leaked' })
    if ($leaks.Count -ne 1 -or $leaks[0] -cnotmatch 'Total device memory leaked: 0 bytes\.$') { $reasons += 'leak line' }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons }
}

function Invoke-SelfTest {
    $base2x = @(
        'validation + best practices + sync: ENABLED'
        'dsr-config: samples=2 depthFormat=126 resolveMode=SAMPLE_ZERO pixels=187 hostBytes=1496 runtimeDynamic=1'
        'dsr-modes: depth=1 stencil=1 (Classic filtered values)'
        'dsr-pipelines: classic=2 dynamic=2 signatures=OK samples=2'
        'dsr-phase: mode=classic cb=000000800133E0D0 rp=1/1 dynamic=0/0 draws=2 copies=1/1 submits=1/1 chain=1'
        'dsr-readback: mode=classic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0'
        'dsr-phase: mode=dynamic cb=00000080013418D0 rp=0/0 dynamic=1/1 draws=2 copies=1/1 submits=1/1 chain=1'
        'dsr-readback: mode=dynamic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0'
        'dsr-compare: colorBytes=748 colorDiff=0 depthBytes=748 depthDiff=0'
        'dsr-retirement: created=4 destroyed=4'
        'Total device memory leaked: 0 bytes.'
        'dsr 2x completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $base4x = @(
        'validation + best practices + sync: ENABLED'
        'dsr-config: samples=4 depthFormat=126 resolveMode=SAMPLE_ZERO pixels=187 hostBytes=1496 runtimeDynamic=1'
        'dsr-modes: depth=1 stencil=1 (Classic filtered values)'
        'dsr-pipelines: classic=2 dynamic=2 signatures=OK samples=4'
        'dsr-phase: mode=classic cb=000000C48E7B18D0 rp=1/1 dynamic=0/0 draws=2 copies=1/1 submits=1/1 chain=1'
        'dsr-readback: mode=classic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0'
        'dsr-phase: mode=dynamic cb=000000C48E7AE0D0 rp=0/0 dynamic=1/1 draws=2 copies=1/1 submits=1/1 chain=1'
        'dsr-readback: mode=dynamic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0'
        'dsr-compare: colorBytes=748 colorDiff=0 depthBytes=748 depthDiff=0'
        'dsr-retirement: created=4 destroyed=4'
        'Total device memory leaked: 0 bytes.'
        'dsr 4x completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $fixtures = @(
        @{ Name='valid-2x'; Case='2x'; Lines=$base2x; Exit=0; Expect=$true }
        @{ Name='valid-4x'; Case='4x'; Lines=$base4x; Exit=0; Expect=$true }
        @{ Name='missing-footer'; Case='2x'; Lines=$base2x[0..10]; Exit=0; Expect=$false }
        @{ Name='duplicate-footer'; Case='2x'; Lines=($base2x + $base2x[11]); Exit=0; Expect=$false }
        @{ Name='foreign-footer'; Case='2x'; Lines=($base2x + 'dsr 123 completed: failures=0 outcome=CLEAN'); Exit=0; Expect=$false }
        @{ Name='footer-junk'; Case='2x'; Lines=($base2x[0..10] + ($base2x[11] + ' junk')); Exit=0; Expect=$false }
        @{ Name='crash'; Case='2x'; Lines=$base2x; Exit=-1073741819; Expect=$false }
        @{ Name='exit3'; Case='2x'; Lines=$base2x; Exit=3; Expect=$false }
        @{ Name='no-validation'; Case='2x'; Lines=$base2x[1..11]; Exit=0; Expect=$false }
        @{ Name='missing-phase'; Case='2x'; Lines=($base2x | Where-Object { $_ -notmatch '^dsr-phase: mode=classic' }); Exit=0; Expect=$false }
        @{ Name='duplicate-phase'; Case='2x'; Lines=($base2x + $base2x[4]); Exit=0; Expect=$false }
        @{ Name='malformed-phase'; Case='2x'; Lines=($base2x + 'dsr-phase:mode=classic'); Exit=0; Expect=$false }
        @{ Name='zero-cb'; Case='2x'; Lines=($base2x -replace 'cb=000000800133E0D0','cb=0000000000000000'); Exit=0; Expect=$false }
        @{ Name='draw-one-less'; Case='2x'; Lines=($base2x[0..3] + ($base2x[4] -replace 'draws=2','draws=1') + $base2x[5..11]); Exit=0; Expect=$false }
        @{ Name='submit-zero'; Case='2x'; Lines=($base2x[0..3] + ($base2x[4] -replace 'submits=1/1','submits=0/1') + $base2x[5..11]); Exit=0; Expect=$false }
        @{ Name='missing-copy'; Case='2x'; Lines=($base2x[0..3] + ($base2x[4] -replace 'copies=1/1','copies=1/0') + $base2x[5..11]); Exit=0; Expect=$false }
        @{ Name='golden-bad-mutual0'; Case='2x'; Lines=($base2x[0..4] + ($base2x[5] -replace 'colorMismatch=0 depthMismatch=0','colorMismatch=4 depthMismatch=0') + $base2x[6..11]); Exit=0; Expect=$false }
        @{ Name='golden-depth-bad'; Case='2x'; Lines=($base2x[0..4] + ($base2x[5] -replace 'depthMismatch=0','depthMismatch=2') + $base2x[6..11]); Exit=0; Expect=$false }
        @{ Name='missing-readback'; Case='2x'; Lines=($base2x | Where-Object { $_ -notmatch '^dsr-readback: mode=classic' }); Exit=0; Expect=$false }
        @{ Name='extra-readback'; Case='2x'; Lines=($base2x + 'dsr-readback: mode=classic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0'); Exit=0; Expect=$false }
        @{ Name='wrong-samples'; Case='2x'; Lines=($base2x -replace 'samples=2 depthFormat','samples=4 depthFormat'); Exit=0; Expect=$false }
        @{ Name='wrong-mode'; Case='2x'; Lines=($base2x -replace 'depth=1 stencil=1','depth=2 stencil=1'); Exit=0; Expect=$false }
        @{ Name='diff-nonzero'; Case='2x'; Lines=($base2x -replace 'colorDiff=0 depthBytes','colorDiff=3 depthBytes'); Exit=0; Expect=$false }
        @{ Name='missing-retirement'; Case='2x'; Lines=($base2x | Where-Object { $_ -notmatch '^dsr-retirement:' }); Exit=0; Expect=$false }
        @{ Name='retirement-mismatch'; Case='2x'; Lines=($base2x -replace 'created=4 destroyed=4','created=4 destroyed=3'); Exit=0; Expect=$false }
        @{ Name='missing-leak'; Case='2x'; Lines=($base2x | Where-Object { $_ -notmatch 'Total device memory' }); Exit=0; Expect=$false }
        @{ Name='vuid'; Case='2x'; Lines=($base2x + 'VUID-fixture'); Exit=0; Expect=$false }
        @{ Name='unknown-case-judge'; Case='8x'; Lines=$base2x; Exit=0; Expect=$false }
        @{ Name='skip-near-realshape'; Case='2x'; Lines=(($base2x -replace 'draws=2','draws=1') -replace 'colorMismatch=0 depthMismatch=0','colorMismatch=374 depthMismatch=187' -replace 'failures=0 liveErrors=0 postErrors=0 mismatches=0','failures=0 liveErrors=0 postErrors=0 mismatches=1122' -replace 'outcome=CLEAN','outcome=FAILED'); Exit=1; Expect=$false }  # multi-condition real shape (full 4 evidence lines kept)
    )
    $fixtures += @(
        @{ Name='lead-bare-footer-append';   Case='2x'; Lines=($base2x + 'dsr 2x completed:'); Exit=0; Expect=$false }
        @{ Name='lead-foreign-bare-footer';  Case='2x'; Lines=($base2x + 'foo completed:'); Exit=0; Expect=$false }
        @{ Name='lead-timestamped-fail';     Case='2x'; Lines=($base2x + '19:00:00 [INFO]: FAIL: synthetic failure'); Exit=0; Expect=$false }
        @{ Name='lead-validation-disabled';  Case='2x'; Lines=($base2x + 'validation + best practices + sync: DISABLED'); Exit=0; Expect=$false }
        @{ Name='depth-mismatch-single';     Case='2x'; Lines=($base2x[0..4] + ($base2x[5] -replace 'depthMismatch=0','depthMismatch=1') + $base2x[6..11]); Exit=0; Expect=$false }
        @{ Name='extra-bad-readback';        Case='2x'; Lines=($base2x + 'dsr-readback: mode=classic colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=9 depthMismatch=0'); Exit=0; Expect=$false }
        @{ Name='extra-bare-footer';         Case='2x'; Lines=($base2x + 'completed:'); Exit=0; Expect=$false }
        @{ Name='duplicate-validation';      Case='2x'; Lines=($base2x + $base2x[0]); Exit=0; Expect=$false }
        @{ Name='malformed-validation';      Case='2x'; Lines=($base2x + 'validation + best practices + sync:ENABLED'); Exit=0; Expect=$false }
    )
    $good = $true; $executed = 0
    foreach ($f in $fixtures) {
        ++$executed
        $r = Test-DepthResolveLog -Lines @($f.Lines) -ExitCode $f.Exit -CaseName $f.Case
        $ok = $r.Pass -eq $f.Expect
        Write-Host "SelfTest $($f.Name): $ok"
        if (-not $ok) { $good = $false; $r.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    Write-Host ("fixtures: {0}; executed: {1}" -f $fixtures.Count, $executed)
    return $good
}

if ($SelfTest) {
    $ok = Invoke-SelfTest
    exit $(if ($ok) {0} else {1})
}

$all = @('2x','4x')
if ($PSBoundParameters.ContainsKey('Cases')) {
    $raw = @($Cases | ForEach-Object { $_ -split ',' })
    if ($raw.Count -eq 0 -or @($raw | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count) {
        Write-Host 'FAIL: empty Cases entry' -ForegroundColor Red; exit 1
    }
    $Cases = @($raw | Select-Object -Unique)
    foreach ($c in $Cases) {
        if ($all -notcontains $c) { Write-Host "FAIL: unknown case '$c'" -ForegroundColor Red; exit 1 }
    }
} else { $Cases = $all }

$envNames = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$oldEnv = @{}
foreach ($n in $envNames) { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
$oldCwd = (Get-Location).Path
$script:anyFail = $false
try {
    Write-Host "building vulkan_depth_resolve_smoke..." -ForegroundColor Cyan
    Write-Host "run root: $runRoot"
    & cmake --build $buildDir --config Release --target vulkan_depth_resolve_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: build failed" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $exe)) { throw "exe not found" }
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    $env:CC_VULKAN_RENDERING = 'dynamic'
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Write-Host 'policy: FORCE=1.3 COMPLETION=auto SUBMIT=legacy BARRIER=legacy RENDERING=dynamic DISABLE=cleared' -ForegroundColor Cyan
    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $empty = Join-Path $repo 'native\build\vulkan-modernization\empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $empty
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    $results = @()
    foreach ($name in $Cases) {
        $caseDir = Join-Path $runRoot $name
        New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
        Push-Location $caseDir
        try {
            Write-Host "`n=== case ${name} ===" -ForegroundColor Cyan
            cmd /c "`"$exe`" --case $name > `"$caseDir\log.txt`" 2>&1"
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        $lines = if (Test-Path "$caseDir\log.txt") { Get-Content "$caseDir\log.txt" } else { @() }
        $r = Test-DepthResolveLog -Lines $lines -ExitCode $code -CaseName $name
        if ($r.Pass) { Write-Host "  [PASS] ${name}" -ForegroundColor Green }
        else { Write-Host "  [FAIL] ${name}: $($r.Reasons -join '; ')" -ForegroundColor Red; $script:anyFail = $true }
        $results += [pscustomobject]@{ Case = $name; Exit = $code; Pass = $r.Pass }
    }
    $results | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host ("`n=== depth-resolve overall: {0} ===" -f $(if ($script:anyFail) {'FAIL'} else {'PASS'}))
} catch {
    Write-Host "FAIL: $_" -ForegroundColor Red; $script:anyFail = $true
} finally {
    foreach ($n in $envNames) {
        if ($null -ne $oldEnv[$n]) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
        else { Remove-Item Env:$n -ErrorAction SilentlyContinue }
    }
    Set-Location $oldCwd
}
exit $(if ($script:anyFail) {1} else {0})
