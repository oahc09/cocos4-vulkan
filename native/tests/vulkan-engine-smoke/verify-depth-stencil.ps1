# M6-A3: verify the depth/stencil hybrid harness (single-sample D32/D32S8).
# Builds the CURRENT target first (red build fails fast), runs each case in
# its own ignored cwd/log, and judges with one strict shared footer function;
# SelfTest exercises the judge with single-condition fixtures.
#   powershell -ExecutionPolicy Bypass -File verify-depth-stencil.ps1 [-Cases depth,depth-stencil] [-SelfTest]
param(
    [string[]]$Cases = @(),
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$buildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator'
$exe = Join-Path $buildDir 'vulkan-engine-smoke\Release\vulkan_dynamic_depth_stencil_smoke.exe'
$outRoot = Join-Path $repo 'native\build\vulkan-modernization\ds-matrix'
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$runRoot = Join-Path $outRoot "run-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

function Test-DepthStencilLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$CaseName = 'depth'
    )
    $reasons = @()
    if ($CaseName -notin @('depth','depth-stencil')) {
        return [pscustomobject]@{ Pass = $false; Reasons = @("unknown case $CaseName") }
    }
    if ($ExitCode -ne 0) { $reasons += "exit $ExitCode != 0 (3 = UNSUPPORTED, not PASS)" }
    if (@($Lines | Where-Object { $_ -match '^validation \+ best practices \+ sync: ENABLED$' }).Count -ne 1 -or
        @($Lines | Where-Object { $_ -match '^validation \+ best practices \+ sync:' }).Count -ne 1) {
        $reasons += 'validation not ENABLED (unique)'
    }
    # unique exact footer; count ALL footers regardless of case prefix
    $footer = "depth-stencil $CaseName completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN"
    $footers = @($Lines | Where-Object { $_ -eq $footer })
    $anyFooters = @($Lines | Where-Object { $_ -match '^depth-stencil' -or $_ -match 'completed:' })
    if ($anyFooters.Count -ne 1) { $reasons += "footer count=$($anyFooters.Count) (expected 1 total)" }
    elseif ($footers.Count -ne 1) { $reasons += "footer not exact clean: $($anyFooters[0])" }
    if (@($Lines | Where-Object { $_ -match 'SKIPPED|INCOMPLETE|UNSUPPORTED' }).Count -ne 0) { $reasons += 'SKIPPED/INCOMPLETE/UNSUPPORTED' }
    if (@($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count -ne 0) { $reasons += 'VUID/hazard lines' }
    if (@($Lines | Where-Object { $_ -match '(^|\]) ?FAIL:' -or $_ -match '\] FAIL:' -or $_ -match '^FAIL:' }).Count -ne 0) { $reasons += 'FAIL lines (incl. timestamped)' }
    # ds-config: unique, case-exact (0/01-style numbers rejected by exact line)
    $wantFmt = if ($CaseName -eq 'depth') { '126' } else { '130' }
    $wantStencil = if ($CaseName -eq 'depth') { '0' } else { '1' }
    $wantBytes = if ($CaseName -eq 'depth') { '1496' } else { '1683' }
    $wantPipes = if ($CaseName -eq 'depth') { '2' } else { '3' }
    $wantCopies = if ($CaseName -eq 'depth') { '1/1/0' } else { '1/1/1' }
    $wantStencilPixels = if ($CaseName -eq 'depth') { '0' } else { '187' }
    $cfg = "ds-config: format=$wantFmt stencil=$wantStencil samples=1 pixels=187 hostBytes=$wantBytes runtimeDynamic=1"
    if (@($Lines | Where-Object { $_ -eq $cfg }).Count -ne 1 -or         @($Lines | Where-Object { $_ -match 'ds-config:' }).Count -ne 1 -or @($Lines | Where-Object { $_ -match 'ds-config' }).Count -ne 1) { $reasons += 'invalid/missing/extra ds-config' }
    $pip = "ds-pipelines: classic=$wantPipes dynamic=$wantPipes signatures=OK depthTest=1 depthWrite=1 stencilTest=$wantStencil"
    if (@($Lines | Where-Object { $_ -eq $pip }).Count -ne 1 -or         @($Lines | Where-Object { $_ -match 'ds-pipelines:' }).Count -ne 1 -or @($Lines | Where-Object { $_ -match 'ds-pipelines' }).Count -ne 1) { $reasons += 'invalid/missing/extra ds-pipelines' }
    # exactly two ds-phase lines: classic then dynamic, non-zero CB handles
    $nz = '(?:0x)?0*[1-9A-Fa-f][0-9A-Fa-f]*'
    $phAny = @($Lines | Where-Object { $_ -match 'ds-phase' })
    if ($phAny.Count -ne 2) { $reasons += "ds-phase prefix count=$($phAny.Count) (expected 2 total, incl. malformed)" }
    $ph = @($Lines | Where-Object { $_ -match '^ds-phase: ' })
    if ($ph.Count -ne 2) { $reasons += "ds-phase count=$($ph.Count) (expected 2)" }
    else {
        $pc = "^ds-phase: mode=classic cb=$nz rp=1/1 dynamic=0/0 draws=$wantPipes copies=$wantCopies submits=1/1 chain=1`$"
        $pd = "^ds-phase: mode=dynamic cb=$nz rp=0/0 dynamic=1/1 draws=$wantPipes copies=$wantCopies submits=1/1 chain=1`$"
        if ($ph[0] -notmatch $pc) { $reasons += "classic phase mismatch: $($ph[0])" }
        if ($ph[1] -notmatch $pd) { $reasons += "dynamic phase mismatch: $($ph[1])" }
    }
    # exactly two ds-readback lines, all three mismatch fields exactly 0
    $rbAny = @($Lines | Where-Object { $_ -match 'ds-readback' })
    if ($rbAny.Count -ne 2) { $reasons += "ds-readback prefix count=$($rbAny.Count) (expected 2 total, incl. malformed)" }
    $rb = @($Lines | Where-Object { $_ -match '^ds-readback: ' })
    if ($rb.Count -ne 2) { $reasons += "ds-readback count=$($rb.Count) (expected 2)" }
    else {
        $pat = "^ds-readback: mode=(classic|dynamic) colorBytes=748 depthPixels=187 stencilPixels=$wantStencilPixels colorMismatch=0 depthMismatch=0 stencilMismatch=0`$"
        if ($rb[0] -notmatch $pat -or $rb[0] -notmatch 'mode=classic') { $reasons += "classic readback mismatch: $($rb[0])" }
        if ($rb[1] -notmatch $pat -or $rb[1] -notmatch 'mode=dynamic') { $reasons += "dynamic readback mismatch: $($rb[1])" }
    }
    # unique ds-compare with exact byte count and zero differences
    $cmp = "ds-compare: bytes=$wantBytes differences=0"
    if (@($Lines | Where-Object { $_ -eq $cmp }).Count -ne 1 -or         @($Lines | Where-Object { $_ -match 'ds-compare:' }).Count -ne 1 -or @($Lines | Where-Object { $_ -match 'ds-compare' }).Count -ne 1) { $reasons += 'invalid/missing/extra ds-compare' }
    # unique retirement with created == destroyed
    $wantCreated = if ($CaseName -eq 'depth') { '4' } else { '6' }
    $ret = "ds-retirement: created=$wantCreated destroyed=$wantCreated"
    if (@($Lines | Where-Object { $_ -eq $ret }).Count -ne 1 -or         @($Lines | Where-Object { $_ -match 'ds-retirement:' }).Count -ne 1 -or @($Lines | Where-Object { $_ -match 'ds-retirement' }).Count -ne 1) { $reasons += 'invalid/missing/extra ds-retirement' }
    # unique zero leak line
    $leaks = @($Lines | Where-Object { $_ -match 'Total device memory leaked:' })
    if ($leaks.Count -ne 1) { $reasons += "memory leak line count=$($leaks.Count)" }
    elseif ($leaks[0] -notmatch 'Total device memory leaked: 0 bytes\.$') { $reasons += "non-zero leak: $($leaks[0])" }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons }
}

function Invoke-SelfTest {
    # literal valid bases taken from the Lead's real verified logs
    $depthBase = @(
        'validation + best practices + sync: ENABLED'
        'ds-config: format=126 stencil=0 samples=1 pixels=187 hostBytes=1496 runtimeDynamic=1'
        'ds-pipelines: classic=2 dynamic=2 signatures=OK depthTest=1 depthWrite=1 stencilTest=0'
        'ds-phase: mode=classic cb=00000080014C18D0 rp=1/1 dynamic=0/0 draws=2 copies=1/1/0 submits=1/1 chain=1'
        'ds-readback: mode=classic colorBytes=748 depthPixels=187 stencilPixels=0 colorMismatch=0 depthMismatch=0 stencilMismatch=0'
        'ds-phase: mode=dynamic cb=00000080014BE0D0 rp=0/0 dynamic=1/1 draws=2 copies=1/1/0 submits=1/1 chain=1'
        'ds-readback: mode=dynamic colorBytes=748 depthPixels=187 stencilPixels=0 colorMismatch=0 depthMismatch=0 stencilMismatch=0'
        'ds-compare: bytes=1496 differences=0'
        'Total device memory leaked: 0 bytes.'
        'ds-retirement: created=4 destroyed=4'
        'depth-stencil depth completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $dsBase = @(
        'validation + best practices + sync: ENABLED'
        'ds-config: format=130 stencil=1 samples=1 pixels=187 hostBytes=1683 runtimeDynamic=1'
        'ds-pipelines: classic=3 dynamic=3 signatures=OK depthTest=1 depthWrite=1 stencilTest=1'
        'ds-phase: mode=classic cb=00000080013E18D0 rp=1/1 dynamic=0/0 draws=3 copies=1/1/1 submits=1/1 chain=1'
        'ds-readback: mode=classic colorBytes=748 depthPixels=187 stencilPixels=187 colorMismatch=0 depthMismatch=0 stencilMismatch=0'
        'ds-phase: mode=dynamic cb=00000080013DE0D0 rp=0/0 dynamic=1/1 draws=3 copies=1/1/1 submits=1/1 chain=1'
        'ds-readback: mode=dynamic colorBytes=748 depthPixels=187 stencilPixels=187 colorMismatch=0 depthMismatch=0 stencilMismatch=0'
        'ds-compare: bytes=1683 differences=0'
        'Total device memory leaked: 0 bytes.'
        'ds-retirement: created=6 destroyed=6'
        'depth-stencil depth-stencil completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $fixtures = @(
        @{ Name='valid-depth';           Case='depth';           Lines=$depthBase; Exit=0; Expect=$true }
        @{ Name='valid-depth-stencil';   Case='depth-stencil';   Lines=$dsBase;    Exit=0; Expect=$true }
        @{ Name='missing-footer';        Case='depth';           Lines=$depthBase[0..9]; Exit=0; Expect=$false }
        @{ Name='duplicate-footer';      Case='depth';           Lines=($depthBase + $depthBase[10]); Exit=0; Expect=$false }
        @{ Name='footer-junk';           Case='depth';           Lines=($depthBase[0..9] + ($depthBase[10] + ' junk')); Exit=0; Expect=$false }
        @{ Name='footer-01';             Case='depth';           Lines=($depthBase -replace 'failures=0','failures=01'); Exit=0; Expect=$false }
        @{ Name='live-errors';           Case='depth';           Lines=($depthBase -replace 'liveErrors=0','liveErrors=1'); Exit=0; Expect=$false }
        @{ Name='post-errors';           Case='depth';           Lines=($depthBase -replace 'postErrors=0','postErrors=2'); Exit=0; Expect=$false }
        @{ Name='footer-mismatch';       Case='depth';           Lines=($depthBase -replace 'mismatches=0 outcome','mismatches=1 outcome'); Exit=0; Expect=$false }
        @{ Name='crash';                 Case='depth';           Lines=$depthBase; Exit=-1073741819; Expect=$false }
        @{ Name='exit3-unsupported';     Case='depth';           Lines=$depthBase; Exit=3; Expect=$false }
        @{ Name='no-validation';         Case='depth';           Lines=$depthBase[1..10]; Exit=0; Expect=$false }
        @{ Name='missing-phase';         Case='depth';           Lines=($depthBase | Where-Object { $_ -notmatch '^ds-phase: mode=classic' }); Exit=0; Expect=$false }
        @{ Name='duplicate-phase';       Case='depth';           Lines=($depthBase + $depthBase[3]); Exit=0; Expect=$false }
        @{ Name='extra-malformed-phase'; Case='depth';           Lines=($depthBase + 'ds-phase: mode=classic cb=0 rp=1/1 dynamic=0/0'); Exit=0; Expect=$false }
        @{ Name='zero-cb';               Case='depth';           Lines=($depthBase -replace 'mode=classic cb=00000080014C18D0','mode=classic cb=0000000000000000'); Exit=0; Expect=$false }
        @{ Name='draw-one-less';         Case='depth';           Lines=($depthBase[0..2] + ($depthBase[3] -replace 'draws=2','draws=1') + $depthBase[4..10]); Exit=0; Expect=$false }
        @{ Name='submit-zero';           Case='depth';           Lines=($depthBase[0..2] + ($depthBase[3] -replace 'submits=1/1','submits=0/1') + $depthBase[4..10]); Exit=0; Expect=$false }
        @{ Name='wrong-format';          Case='depth';           Lines=($depthBase -replace 'format=126','format=124'); Exit=0; Expect=$false }
        @{ Name='wrong-stencil';         Case='depth';           Lines=($depthBase -replace 'stencil=0 samples','stencil=1 samples'); Exit=0; Expect=$false }
        @{ Name='wrong-bytes';           Case='depth';           Lines=($depthBase -replace 'hostBytes=1496','hostBytes=1497'); Exit=0; Expect=$false }
        @{ Name='golden-color-mismatch'; Case='depth';           Lines=($depthBase[0..3] + ($depthBase[4] -replace 'colorMismatch=0 depthMismatch','colorMismatch=1 depthMismatch') + $depthBase[5..10]); Exit=0; Expect=$false }
        @{ Name='golden-depth-mismatch';  Case='depth';           Lines=($depthBase[0..3] + ($depthBase[4] -replace 'depthMismatch=0 stencilMismatch','depthMismatch=1 stencilMismatch') + $depthBase[5..10]); Exit=0; Expect=$false }
        @{ Name='golden-stencil-mismatch'; Case='depth-stencil';   Lines=($dsBase[0..3] + ($dsBase[4] -replace 'stencilMismatch=0','stencilMismatch=1') + $dsBase[5..10]); Exit=0; Expect=$false }
        @{ Name='missing-retirement';    Case='depth';           Lines=($depthBase | Where-Object { $_ -notmatch '^ds-retirement:' }); Exit=0; Expect=$false }
        @{ Name='retirement-nonzero';    Case='depth';           Lines=($depthBase -replace 'created=4 destroyed=4','created=4 destroyed=3'); Exit=0; Expect=$false }
        @{ Name='missing-leak';          Case='depth';           Lines=($depthBase | Where-Object { $_ -notmatch 'Total device memory' }); Exit=0; Expect=$false }
        @{ Name='duplicate-leak';        Case='depth';           Lines=($depthBase + $depthBase[8]); Exit=0; Expect=$false }
        @{ Name='vuid';                  Case='depth';           Lines=($depthBase + 'VUID-fixture'); Exit=0; Expect=$false }
        @{ Name='fail-line';             Case='depth';           Lines=($depthBase + 'FAIL: fixture'); Exit=0; Expect=$false }
        @{ Name='unknown-case-judge';    Case='depth-only';      Lines=$depthBase; Exit=0; Expect=$false }
        @{ Name='skip-near-realshape-multi'; Case='depth-stencil'; Lines=(($dsBase | Where-Object { $_ -notmatch '^ds-phase: mode=classic' } | Where-Object { $_ -notmatch '^ds-readback: mode=classic' }) -replace 'colorMismatch=0 depthMismatch=0 stencilMismatch=0','colorMismatch=374 depthMismatch=187 stencilMismatch=187' -replace 'failures=0 liveErrors=0 postErrors=0 mismatches=0','failures=2 liveErrors=0 postErrors=0 mismatches=1496' -replace 'outcome=CLEAN','outcome=FAILED'); Exit=1; Expect=$false }
    )
    $fixtures += @(
        @{ Name='lead-phase-garbage-append';  Case='depth'; Lines=($depthBase + 'ds-phase:garbage'); Exit=0; Expect=$false }
        @{ Name='lead-readback-garbage-append'; Case='depth'; Lines=($depthBase + 'ds-readback:garbage'); Exit=0; Expect=$false }
        @{ Name='lead-foreign-bad-footer';    Case='depth'; Lines=($depthBase + 'depth-stencil depth_2 completed: failures=1'); Exit=0; Expect=$false }
        @{ Name='lead-timestamped-fail';      Case='depth'; Lines=($depthBase + '[17:00:00] FAIL: fixture'); Exit=0; Expect=$false }
        @{ Name='lead-same-cb-reuse';         Case='depth'; Lines=($depthBase -replace 'cb=00000080014BE0D0','cb=00000080014C18D0'); Exit=0; Expect=$true }
        @{ Name='nospace-malformed-phase';    Case='depth'; Lines=($depthBase + 'ds-phase:mode=classic'); Exit=0; Expect=$false }
        @{ Name='nospace-malformed-readback'; Case='depth'; Lines=($depthBase + 'ds-readback:mode=dynamic'); Exit=0; Expect=$false }
        @{ Name='numeric-footer-prefix';      Case='depth'; Lines=($depthBase + 'depth-stencil 123 completed: failures=0 outcome=CLEAN'); Exit=0; Expect=$false }
        @{ Name='underscore-footer-prefix';   Case='depth'; Lines=($depthBase + 'depth_stencil depth completed: failures=0 outcome=CLEAN'); Exit=0; Expect=$false }
        @{ Name='nospace-footer-prefix';      Case='depth'; Lines=($depthBase + 'depth-stencildepth completed: failures=0 outcome=CLEAN'); Exit=0; Expect=$false }
        @{ Name='truncated-footer';           Case='depth'; Lines=($depthBase + 'depth-stencil depth completed: failures='); Exit=0; Expect=$false }
        @{ Name='bare-extra-footer';          Case='depth'; Lines=($depthBase + 'depth-stencil depth completed:'); Exit=0; Expect=$false }
        @{ Name='malformed-config-append';    Case='depth'; Lines=($depthBase + 'ds-config:format=126'); Exit=0; Expect=$false }
        @{ Name='malformed-pipelines-append'; Case='depth'; Lines=($depthBase + 'ds-pipelines:classic=2'); Exit=0; Expect=$false }
    )
    $good = $true
    $executed = 0
    foreach ($f in $fixtures) {
        ++$executed
        $case = if ($f.Case) { $f.Case } else { 'depth' }
        $r = Test-DepthStencilLog -Lines @($f.Lines) -ExitCode $f.Exit -CaseName $case
        $ok = $r.Pass -eq $f.Expect
        Write-Host "SelfTest $($f.Name): $ok"
        if (-not $ok) { $good = $false; $r.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    Write-Host ("fixtures: {0}; executed: {1}" -f $fixtures.Count, $executed)
    return $good
}

if ($SelfTest) {
    $ok = Invoke-SelfTest
    Write-Host ("SelfTest exit: {0}" -f $(if ($ok) {0} else {1}))
    exit $(if ($ok) {0} else {1})
}

$all = @('depth', 'depth-stencil')
if ($PSBoundParameters.ContainsKey('Cases')) {
    $raw = @($Cases | ForEach-Object { $_ -split ',' })
    if (@($raw | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
        Write-Host 'FAIL: empty Cases entry (incl. partial empties)' -ForegroundColor Red; exit 1
    }
    $Cases = @($raw | Select-Object -Unique)
    if ($Cases.Count -eq 0) { Write-Host 'FAIL: empty Cases entry'; exit 1 }
    foreach ($c in $Cases) {
        if ($all -notcontains $c) { Write-Host "FAIL: unknown case '$c'. valid: $($all -join ', ')" -ForegroundColor Red; exit 1 }
    }
} else { $Cases = $all }

$envNames = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$oldEnv = @{}
foreach ($n in $envNames) { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
$oldCwd = (Get-Location).Path
$script:anyFail = $false
try {
    Write-Host "building vulkan_dynamic_depth_stencil_smoke..." -ForegroundColor Cyan
    Write-Host "run root: $runRoot"
    & cmake --build $buildDir --config Release --target vulkan_dynamic_depth_stencil_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: build failed" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

    # policy isolation: auto completion + legacy submit/barrier, dynamic path ON
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    $env:CC_VULKAN_RENDERING = 'dynamic'
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Write-Host "policy: FORCE=1.3 COMPLETION=auto SUBMIT=legacy BARRIER=legacy RENDERING=dynamic DISABLE=(cleared)" -ForegroundColor Cyan

    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $emptyImplicit = Join-Path $repo 'native\build\vulkan-modernization\empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $emptyImplicit | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $emptyImplicit
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
        $r = Test-DepthStencilLog -Lines $lines -ExitCode $code -CaseName $name
        if ($r.Pass) { Write-Host "  [PASS] ${name}" -ForegroundColor Green }
        else { Write-Host "  [FAIL] ${name}: $($r.Reasons -join '; ')" -ForegroundColor Red; $script:anyFail = $true }
        $results += [pscustomobject]@{ Case = $name; Exit = $code; Pass = $r.Pass }
    }
    $results | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host ("`n=== depth-stencil overall: {0} ===" -f $(if ($script:anyFail) {'FAIL'} else {'PASS'}))
} catch {
    Write-Host "FAIL: unexpected script error: $_" -ForegroundColor Red
    $script:anyFail = $true
} finally {
    foreach ($n in $envNames) {
        if ($null -ne $oldEnv[$n]) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
        else { Remove-Item Env:$n -ErrorAction SilentlyContinue }
    }
    Set-Location $oldCwd
}
exit $(if ($script:anyFail) {1} else {0})
