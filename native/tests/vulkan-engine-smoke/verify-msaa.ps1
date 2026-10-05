# M6-A4: build current sources, run 2x/4x independently, and validate the
# four real mode/pattern chains and two owned readback comparisons.
param([string[]]$Cases = @(), [switch]$SelfTest)
$ErrorActionPreference = 'Stop'

function Test-MsaaLog {
    param([AllowEmptyCollection()][string[]]$Lines = @(), [int]$ExitCode = 0, [string]$CaseName = '2x')
    $reasons = [System.Collections.Generic.List[string]]::new()
    if ($CaseName -cnotin @('2x', '4x')) {
        return [pscustomobject]@{ Pass = $false; Reasons = @('unknown case') }
    }
    $samples = if ($CaseName -ceq '2x') { 2 } else { 4 }
    if ($ExitCode -ne 0) { $reasons.Add("exit $ExitCode (UNSUPPORTED is not PASS)") }
    if (@($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD|\bFAIL\b|\b(SKIPPED|INCOMPLETE|UNSUPPORTED)\b' }).Count) {
        $reasons.Add('error/incomplete evidence')
    }
    $validation = @($Lines | Where-Object { $_ -match 'validation \+ best practices \+ sync' })
    if ($validation.Count -ne 1 -or $validation[0] -cne 'validation + best practices + sync: ENABLED') {
        $reasons.Add('validation triad not uniquely ENABLED')
    }
    $footers = @($Lines | Where-Object { $_ -match '^msaa(?:\s|$)|completed:' })
    $clean = "msaa $CaseName completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN"
    if ($footers.Count -ne 1 -or $footers[0] -cne $clean) { $reasons.Add('missing/extra/non-clean footer') }
    $singletons = @(
        @{ Prefix = 'msaa-config'; Text = "msaa-config: samples=$samples resolve=1 pixels=187 hostBytes=748 runtimeDynamic=1" }
        @{ Prefix = 'msaa-pipelines'; Text = "msaa-pipelines: classic=2 dynamic=2 signatures=OK samples=$samples" }
        @{ Prefix = 'msaa-retirement'; Text = 'msaa-retirement: created=4 destroyed=4' }
    )
    foreach ($item in $singletons) {
        $records = @($Lines | Where-Object { $_ -match [regex]::Escape($item.Prefix) })
        if ($records.Count -ne 1 -or $records[0] -cne $item.Text) { $reasons.Add("invalid $($item.Prefix)") }
    }
    $keys = @(
        @{ Mode = 'classic'; Pattern = 'uniform'; RP = '1/1'; Dynamic = '0/0' }
        @{ Mode = 'classic'; Pattern = 'edge'; RP = '1/1'; Dynamic = '0/0' }
        @{ Mode = 'dynamic'; Pattern = 'uniform'; RP = '0/0'; Dynamic = '1/1' }
        @{ Mode = 'dynamic'; Pattern = 'edge'; RP = '0/0'; Dynamic = '1/1' }
    )
    $phases = @($Lines | Where-Object { $_ -match 'msaa-phase' })
    $readbacks = @($Lines | Where-Object { $_ -match 'msaa-readback' })
    if ($phases.Count -ne 4) { $reasons.Add('expected exactly four phase records') }
    if ($readbacks.Count -ne 4) { $reasons.Add('expected exactly four readback records') }
    $nonZeroHandle = '(?:0x)?0*[1-9A-Fa-f][0-9A-Fa-f]*'
    for ($i = 0; $i -lt 4; ++$i) {
        $key = $keys[$i]
        if ($phases.Count -eq 4) {
            $pattern = '^msaa-phase: mode=' + $key.Mode + ' pattern=' + $key.Pattern +
                ' cb=' + $nonZeroHandle + ' rp=' + $key.RP + ' dynamic=' + $key.Dynamic +
                ' draws=1 copies=1 submits=1/1 chain=1$'
            if ($phases[$i] -cnotmatch $pattern) { $reasons.Add("invalid phase $i") }
        }
        if ($readbacks.Count -eq 4) {
            $prefix = 'msaa-readback: mode=' + $key.Mode + ' pattern=' + $key.Pattern + ' bytes=748 checkedBytes=748 '
            if ($key.Pattern -ceq 'uniform') {
                if ($readbacks[$i] -cne ($prefix + 'red=187 blue=0 mixed=0 mismatch=0')) { $reasons.Add("invalid uniform golden $i") }
            } else {
                $match = [regex]::Match($readbacks[$i], '^' + [regex]::Escape($prefix) +
                    'red=([1-9][0-9]{0,2}) blue=([1-9][0-9]{0,2}) mixed=([1-9][0-9]{0,2}) mismatch=0$')
                if (-not $match.Success) { $reasons.Add("invalid edge content $i") }
                elseif (([int]$match.Groups[1].Value + [int]$match.Groups[2].Value + [int]$match.Groups[3].Value) -ne 187) {
                    $reasons.Add("edge pixel counts do not total 187 at $i")
                }
            }
        }
    }
    # Every pattern has its own owned comparison. Extra malformed records count.
    $compares = @($Lines | Where-Object { $_ -match 'msaa-compare' })
    if ($compares.Count -ne 2 -or
        $compares[0] -cne 'msaa-compare: pattern=uniform bytes=748 differences=0' -or
        $compares[1] -cne 'msaa-compare: pattern=edge bytes=748 differences=0') {
        $reasons.Add('missing/extra/nonzero pattern comparisons')
    }
    $leaks = @($Lines | Where-Object { $_ -match 'Total device memory leaked' })
    if ($leaks.Count -ne 1 -or $leaks[0] -cnotmatch 'Total device memory leaked: 0 bytes\.$') {
        $reasons.Add('missing/extra/nonzero memory leak record')
    }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons.ToArray() }
}

function Invoke-MsaaSelfTest {
    # Canonical protocol values from the rev2 RTX raw log, not loaded from an
    # optional local artifact at test time. Fixtures do not require GPU/SDK.
    $base = [string[]]@(
        'validation + best practices + sync: ENABLED'
        'msaa-config: samples=2 resolve=1 pixels=187 hostBytes=748 runtimeDynamic=1'
        'msaa-pipelines: classic=2 dynamic=2 signatures=OK samples=2'
        'msaa-phase: mode=classic pattern=uniform cb=00000080013618D0 rp=1/1 dynamic=0/0 draws=1 copies=1 submits=1/1 chain=1'
        'msaa-readback: mode=classic pattern=uniform bytes=748 checkedBytes=748 red=187 blue=0 mixed=0 mismatch=0'
        'msaa-phase: mode=classic pattern=edge cb=000000800135E0D0 rp=1/1 dynamic=0/0 draws=1 copies=1 submits=1/1 chain=1'
        'msaa-readback: mode=classic pattern=edge bytes=748 checkedBytes=748 red=86 blue=87 mixed=14 mismatch=0'
        'msaa-phase: mode=dynamic pattern=uniform cb=00000080013650D0 rp=0/0 dynamic=1/1 draws=1 copies=1 submits=1/1 chain=1'
        'msaa-readback: mode=dynamic pattern=uniform bytes=748 checkedBytes=748 red=187 blue=0 mixed=0 mismatch=0'
        'msaa-phase: mode=dynamic pattern=edge cb=00000080013688D0 rp=0/0 dynamic=1/1 draws=1 copies=1 submits=1/1 chain=1'
        'msaa-readback: mode=dynamic pattern=edge bytes=748 checkedBytes=748 red=86 blue=87 mixed=14 mismatch=0'
        'msaa-compare: pattern=uniform bytes=748 differences=0'
        'msaa-compare: pattern=edge bytes=748 differences=0'
        '18:20:35 [INFO]: Total device memory leaked: 0 bytes.'
        'msaa-retirement: created=4 destroyed=4'
        'msaa 2x completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $fixtures = [System.Collections.Generic.List[object]]::new()
    function Add-Fixture($Name, [string[]]$Lines, [bool]$Expected = $false, [int]$Code = 0, [string]$Case = '2x') {
        $fixtures.Add([pscustomobject]@{ Name = $Name; Lines = $Lines; Expected = $Expected; Code = $Code; Case = $Case })
    }
    function Changed([string[]]$InputLines, [int]$Index, [string]$Value) {
        $copy = [string[]]$InputLines.Clone()
        $copy[$Index] = $Value
        return ,$copy
    }
    Add-Fixture 'valid-2x' $base $true
    $four = [string[]]($base -replace 'samples=2', 'samples=4' -replace 'msaa 2x completed:', 'msaa 4x completed:' -replace 'blue=87 mixed=14', 'blue=86 mixed=15')
    Add-Fixture 'valid-4x' $four $true 0 '4x'
    Add-Fixture 'legal-handle-reuse' ($base -replace 'cb=[0-9A-F]+', 'cb=ABC') $true
    Add-Fixture 'crash-with-clean-log' $base $false -1073741819
    Add-Fixture 'unsupported-with-clean-log' $base $false 3
    Add-Fixture 'unknown-case' $base $false 0 '8x'
    foreach ($index in @(0, 1, 2, 3, 4, 11, 12, 13, 14, 15)) {
        Add-Fixture "missing-record-$index" (Changed $base $index '')
    }
    foreach ($index in @(0, 1, 2, 3, 4, 11, 13, 14, 15)) {
        Add-Fixture "duplicate-record-$index" ($base + $base[$index])
    }
    foreach ($prefix in @('msaa-config', 'msaa-pipelines', 'msaa-phase', 'msaa-readback', 'msaa-compare', 'msaa-retirement')) {
        Add-Fixture "malformed-extra-$prefix" ($base + ($prefix + ':garbage'))
    }
    Add-Fixture 'bare-extra-footer' ($base + 'msaa 2x completed:')
    Add-Fixture 'foreign-extra-footer' ($base + 'other_name 123 completed: failures=0')
    Add-Fixture 'footer-trailing-junk' (Changed $base 15 ($base[15] + ' junk'))
    foreach ($field in @('failures', 'liveErrors', 'postErrors', 'mismatches')) {
        Add-Fixture "footer-$field-nonzero" (Changed $base 15 ($base[15] -replace ($field + '=0'), ($field + '=1')))
    }
    Add-Fixture 'footer-leading-zero' (Changed $base 15 ($base[15] -replace 'failures=0', 'failures=01'))
    Add-Fixture 'wrong-config-samples' (Changed $base 1 ($base[1] -replace 'samples=2', 'samples=4'))
    Add-Fixture 'runtime-off' (Changed $base 1 ($base[1] -replace 'runtimeDynamic=1', 'runtimeDynamic=0'))
    Add-Fixture 'wrong-pipeline-samples' (Changed $base 2 ($base[2] -replace 'samples=2', 'samples=4'))
    foreach ($field in @('draws', 'copies', 'chain')) {
        Add-Fixture "phase-$field-zero" (Changed $base 3 ($base[3] -replace ($field + '=1'), ($field + '=0')))
    }
    Add-Fixture 'phase-zero-submit' (Changed $base 3 ($base[3] -replace 'submits=1/1', 'submits=0/1'))
    Add-Fixture 'phase-zero-handle' (Changed $base 3 ($base[3] -replace 'cb=[0-9A-F]+', 'cb=0000'))
    Add-Fixture 'phase-extra-end' (Changed $base 3 ($base[3] -replace 'rp=1/1', 'rp=1/2'))
    Add-Fixture 'phase-trailing-junk' (Changed $base 3 ($base[3] + ' junk'))
    foreach ($index in @(4, 6, 8, 10)) {
        Add-Fixture "independent-content-error-$index" (Changed $base $index ($base[$index] -replace 'mismatch=0', 'mismatch=1'))
    }
    Add-Fixture 'uniform-byte-count-prefix-trick' (Changed $base 4 ($base[4] -replace 'bytes=748 ', 'bytes=7489 '))
    Add-Fixture 'uniform-incomplete-check' (Changed $base 4 ($base[4] -replace 'checkedBytes=748', 'checkedBytes=744'))
    Add-Fixture 'uniform-not-all-red' (Changed $base 4 ($base[4] -replace 'red=187', 'red=186'))
    Add-Fixture 'edge-count-total-wrong' (Changed $base 6 ($base[6] -replace 'red=86', 'red=85'))
    # A deliberately binary but complete total isolates the mixed-pixel rule.
    Add-Fixture 'edge-binary-total-187' (Changed $base 6 ($base[6] -replace 'blue=87 mixed=14', 'blue=101 mixed=0'))
    Add-Fixture 'edge-trailing-junk' (Changed $base 6 ($base[6] + ' junk'))
    foreach ($index in @(11, 12)) {
        Add-Fixture "independent-compare-error-$index" (Changed $base $index ($base[$index] -replace 'differences=0', 'differences=1'))
    }
    Add-Fixture 'nonzero-leak' (Changed $base 13 ($base[13] -replace '0 bytes', '1 bytes'))
    Add-Fixture 'incomplete-retirement' (Changed $base 14 ($base[14] -replace 'destroyed=4', 'destroyed=3'))
    foreach ($line in @('VUID-fixture', 'SYNC-HAZARD-fixture', '[18:20:35] FAIL: fixture', 'INCOMPLETE: fixture', 'SKIPPED: fixture')) {
        Add-Fixture ("bad-line-" + $line) ($base + $line)
    }
    $good = $true
    $executed = 0
    foreach ($fixture in $fixtures) {
        ++$executed
        $result = Test-MsaaLog -Lines $fixture.Lines -ExitCode $fixture.Code -CaseName $fixture.Case
        $correct = $result.Pass -eq $fixture.Expected
        Write-Host ("SelfTest {0}: {1}" -f $fixture.Name, $correct)
        if (-not $correct) { $good = $false; Write-Host ($result.Reasons -join '; ') }
    }
    Write-Host ("fixtures: {0}; executed: {1}" -f $fixtures.Count, $executed)
    return $good
}

if ($SelfTest) {
    $ok = Invoke-MsaaSelfTest
    exit $(if ($ok) { 0 } else { 1 })
}
$allowed = @('2x', '4x')
if ($PSBoundParameters.ContainsKey('Cases')) {
    $raw = @($Cases | ForEach-Object { $_ -split ',' })
    if (-not $raw.Count -or @($raw | Where-Object { [string]::IsNullOrWhiteSpace($_) -or $_ -cnotin $allowed }).Count) {
        Write-Host 'FAIL: Cases must contain only non-empty 2x/4x entries'
        exit 1
    }
    $Cases = @($raw | Select-Object -Unique)
} else { $Cases = $allowed }
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$buildDir = Join-Path $repo 'native/build/vulkan-modernization/win64-simulator'
$exe = Join-Path $buildDir 'vulkan-engine-smoke/Release/vulkan_msaa_resolve_smoke.exe'
$runRoot = Join-Path $repo ('native/build/vulkan-modernization/msaa-matrix/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $PID)
$envNames = @('PATH', 'VK_LAYER_PATH', 'VK_IMPLICIT_LAYER_PATH', 'CC_VULKAN_FORCE_API', 'CC_VULKAN_COMPLETION',
    'CC_VULKAN_SUBMIT', 'CC_VULKAN_BARRIER', 'CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES')
$old = @{}
foreach ($name in $envNames) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$cwd = (Get-Location).Path
$failed = $false
try {
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    Write-Host "building current vulkan_msaa_resolve_smoke; run root: $runRoot"
    & cmake --build $buildDir --config Release --target vulkan_msaa_resolve_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'current target build failed' }
    if (-not (Test-Path -LiteralPath $exe)) { throw "missing executable: $exe" }
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    $env:CC_VULKAN_RENDERING = 'dynamic'
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    $env:VK_LAYER_PATH = 'D:/Software/VulkanSDK/1.4.341.1/Bin'
    $empty = Join-Path $repo 'native/build/vulkan-modernization/empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $empty
    $env:PATH = (Join-Path $repo 'native/external/win64/libs') + ';' +
        (Join-Path $repo 'native/external/win64/libs/v8/Release') + ';' + $env:PATH
    Write-Host 'policy: FORCE=1.3 COMPLETION=auto SUBMIT=legacy BARRIER=legacy RENDERING=dynamic DISABLE=cleared'
    foreach ($caseName in $Cases) {
        $dir = Join-Path $runRoot $caseName
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $log = Join-Path $dir 'log.txt'
        Push-Location $dir
        try {
            # Merge native streams inside cmd for Windows PowerShell 5.1 too.
            $command = '"' + $exe + '" --case ' + $caseName + ' > "' + $log + '" 2>&1'
            & cmd /c $command
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        $lines = Get-Content -LiteralPath $log
        $result = Test-MsaaLog -Lines $lines -ExitCode $code -CaseName $caseName
        Write-Host ("[{0}] {1}: exit={2}; {3}" -f $(if ($result.Pass) { 'PASS' } else { 'FAIL' }),
            $caseName, $code, ($result.Reasons -join '; '))
        if (-not $result.Pass) { $failed = $true }
    }
} catch {
    Write-Host "FAIL: MSAA verification error: $_"
    $failed = $true
} finally {
    foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
    Set-Location -LiteralPath $cwd
}
Write-Host ("MSAA overall: {0}" -f $(if ($failed) { 'FAIL' } else { 'PASS' }))
exit $(if ($failed) { 1 } else { 0 })
