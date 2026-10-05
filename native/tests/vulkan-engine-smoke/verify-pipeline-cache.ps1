# M7-A: verify the pipeline cache persistence harness (vulkan_pipeline_cache_smoke).
# Builds the CURRENT target first; runs in its own ignored cwd (the exe writes
# a pipeline-cache-suite.bin in the CWD; never run in the repo root or an old
# result dir). Test-PipelineCacheLog is the single shared judge.
#   powershell -ExecutionPolicy Bypass -File verify-pipeline-cache.ps1 [-SelfTest]
param([switch]$SelfTest)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$buildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator'
$exe = Join-Path $buildDir 'vulkan-engine-smoke\Release\vulkan_pipeline_cache_smoke.exe'
$outRoot = Join-Path $repo 'native\build\vulkan-modernization\cache-matrix'
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$runRoot = Join-Path $outRoot ("run-{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $PID)
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

function Test-PipelineCacheLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0
    )
    $reasons = [System.Collections.Generic.List[string]]::new()
    if ($ExitCode -ne 0) { $reasons.Add("exit $ExitCode != 0") }
    # validation triad: unique reserved prefix, exact ENABLED
    $valAny = @($Lines | Where-Object { $_ -match 'cache validation' })
    $valExact = @($Lines | Where-Object { $_ -ceq 'cache validation: ENABLED' })
    if ($valAny.Count -ne 1 -or $valExact.Count -ne 1) { $reasons.Add("cache validation count=$($valAny.Count) (expected 1 ENABLED)") }
    # three stage lines: reserved-prefix wide count, then exact full-line match
    $stages = @(
        @{ P = 'cache-stage restored';     T = 'cache-stage restored: realPipeline=1 cachedCreates=1 preserved=1' }
        @{ P = 'cache-stage cold-recovery'; T = 'cache-stage cold-recovery: cases=4 coldCreates=5' }
        @{ P = 'cache-stage rejected';      T = 'cache-stage rejected: rejected=1 coldCreates=6' }
    )
    foreach ($s in $stages) {
        $any = @($Lines | Where-Object { $_ -match [regex]::Escape($s.P) })
        $exact = @($Lines | Where-Object { $_ -ceq $s.T })
        if ($any.Count -ne 1 -or $exact.Count -ne 1) { $reasons.Add("stage $($s.P) any=$($any.Count) exact=$($exact.Count)") }
    }
    # any malformed/unknown/malformed stage line with 'cache-stage' prefix counts
    $stageAny = @($Lines | Where-Object { $_ -match 'cache-stage' })
    if ($stageAny.Count -ne 3) { $reasons.Add("cache-stage total=$($stageAny.Count) (expected 3)") }
    # footer: any completed: / cache-suite line counts, then exact
    $anyFooters = @($Lines | Where-Object { $_ -match 'completed:' -or $_ -match '^cache-suite' })
    $footerExact = 'cache-suite completed: failures=0 liveErrors=0 postErrors=0'
    $footers = @($Lines | Where-Object { $_ -ceq $footerExact })
    if ($anyFooters.Count -ne 1) { $reasons.Add("footer count=$($anyFooters.Count)") }
    elseif ($footers.Count -ne 1) { $reasons.Add("footer not exact: $($anyFooters[0])") }
    # error/incomplete patterns reject
    if (@($Lines | Where-Object { $_ -match 'VUID-' }).Count) { $reasons.Add('VUID lines') }
    if (@($Lines | Where-Object { $_ -match 'HAZARD' }).Count) { $reasons.Add('HAZARD lines (incl. non-SYNC)') }
    if (@($Lines | Where-Object { $_ -match 'FAIL:' }).Count) { $reasons.Add('FAIL lines') }
    if (@($Lines | Where-Object { $_ -match '\bSKIPPED\b|\bINCOMPLETE\b|\bUNSUPPORTED\b' }).Count) { $reasons.Add('SKIPPED/INCOMPLETE/UNSUPPORTED') }
    # leak line unique exact
    $leaks = @($Lines | Where-Object { $_ -cmatch '^(?:[0-9]{2}:[0-9]{2}:[0-9]{2} \[INFO\]: )?Total device memory leaked: 0 bytes\.$' })
    if ($leaks.Count -ne 1 -or @($Lines | Where-Object { $_ -match 'Total device memory leaked' }).Count -ne 1) { $reasons.Add('leak line invalid') }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons.ToArray() }
}

function Invoke-SelfTest {
    # canonical literal protocol values taken from the Lead-verified real run log
    $base = [string[]]@(
        'cache validation: ENABLED'
        'cache-stage restored: realPipeline=1 cachedCreates=1 preserved=1'
        'cache-stage cold-recovery: cases=4 coldCreates=5'
        'cache-stage rejected: rejected=1 coldCreates=6'
        'Total device memory leaked: 0 bytes.'
        'cache-suite completed: failures=0 liveErrors=0 postErrors=0'
    )
    $fixtures = [System.Collections.Generic.List[hashtable]]::new()
    $fixtures.Add(@{ Name='valid';            Lines=$base; Exit=0; Expect=$true })
    $fixtures.Add(@{ Name='missing-footer';   Lines=$base[0..4]; Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='duplicate-footer'; Lines=($base + $base[5]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='footer-junk';      Lines=($base[0..4] + ($base[5] + ' junk')); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='foreign-footer';   Lines=($base + 'foo completed:'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='footer-truncated'; Lines=($base + 'cache-suite completed: failures='); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='footer-01';        Lines=($base -replace 'failures=0','failures=01'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='live-nonzero';     Lines=($base -replace 'liveErrors=0','liveErrors=1'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='post-nonzero';     Lines=($base -replace 'postErrors=0','postErrors=2'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='failures-nonzero'; Lines=($base -replace 'failures=0','failures=1'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='ntstatus';         Lines=$base; Exit=-1073741819; Expect=$false })
    $fixtures.Add(@{ Name='missing-validation'; Lines=($base[1..5]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='validation-disabled'; Lines=($base + 'cache validation: DISABLED'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='missing-restored';  Lines=($base | Where-Object { $_ -notmatch 'restored:' }); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='duplicate-restored'; Lines=($base + $base[1]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='stage-count-wrong'; Lines=($base -replace 'coldCreates=5','coldCreates=6'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='stage-trailing-junk'; Lines=($base[0..2] + ($base[3] + ' junk') + $base[4..5]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='missing-stage';     Lines=($base | Where-Object { $_ -notmatch 'cold-recovery' }); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='leak-nonzero';      Lines=($base -replace 'leaked: 0','leaked: 4096'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='duplicate-leak';    Lines=($base + $base[4]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='timestamped-fail';  Lines=($base + '19:00:00 [INFO]: FAIL: fixture'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='vuid';              Lines=($base + 'VUID-fixture'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='skipped';           Lines=($base + 'SKIPPED'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='incomplete';        Lines=($base + 'INCOMPLETE'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='lead-cache-suite-bad';   Lines=($base + 'cache-suite_bad failures=1'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='lead-garbage-leak';      Lines=($base[0..3] + ('garbage Total device memory leaked: 0 bytes.') + $base[5..5]); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='hazard-non-sync';        Lines=($base + 'WRITE-AFTER-WRITE HAZARD fixture'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='unknown-malformed-stage'; Lines=($base + 'cache-stage:malformed'); Exit=0; Expect=$false })
    $fixtures.Add(@{ Name='unsupported'; Lines=($base + 'UNSUPPORTED'); Exit=0; Expect=$false })
    $good = $true; $executed = 0
    foreach ($f in $fixtures) {
        ++$executed
        $r = Test-PipelineCacheLog -Lines @($f.Lines) -ExitCode $f.Exit
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

$envNames = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$oldEnv = @{}
foreach ($n in $envNames) { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
$oldCwd = (Get-Location).Path
$script:anyFail = $false
try {
    Write-Host "building vulkan_pipeline_cache_smoke..." -ForegroundColor Cyan
    Write-Host "run root: $runRoot"
    & cmake --build $buildDir --config Release --target vulkan_pipeline_cache_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: build failed" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    Remove-Item Env:CC_VULKAN_RENDERING -ErrorAction SilentlyContinue
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Write-Host 'policy: FORCE=1.3 COMPLETION=auto SUBMIT=legacy BARRIER=legacy RENDERING=(cleared) DISABLE=cleared' -ForegroundColor Cyan
    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $empty = Join-Path $repo 'native\build\vulkan-modernization\empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $empty
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH

    # the exe writes a pipeline-cache-suite.bin in its CWD; use an isolated dir
    $caseDir = Join-Path $runRoot 'suite'
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    Push-Location $caseDir
    try {
        Write-Host "`n=== pipeline-cache suite ===" -ForegroundColor Cyan
        cmd /c "`"$exe`" > `"$caseDir\log.txt`" 2>&1"
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $lines = if (Test-Path "$caseDir\log.txt") { Get-Content "$caseDir\log.txt" } else { @() }
    $r = Test-PipelineCacheLog -Lines $lines -ExitCode $code
    if ($r.Pass) { Write-Host "  [PASS] pipeline-cache" -ForegroundColor Green }
    else { Write-Host "  [FAIL] pipeline-cache: $($r.Reasons -join '; ')" -ForegroundColor Red; $script:anyFail = $true }
    $results = [pscustomobject]@{ Case = 'pipeline-cache'; Exit = $code; Pass = $r.Pass }
    $results | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host ("`n=== pipeline-cache overall: {0} ===" -f $(if ($script:anyFail) {'FAIL'} else {'PASS'}))
} catch {
    Write-Host "FAIL: $_" -ForegroundColor Red
    $script:anyFail = $true
} finally {
    foreach ($n in $envNames) {
        if ($null -ne $oldEnv[$n]) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
        else { Remove-Item Env:$n -ErrorAction SilentlyContinue }
    }
    Set-Location $oldCwd
}
exit $(if ($script:anyFail) {1} else {0})
