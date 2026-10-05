# M7-G3 formal prewarm verification (Windows, current source only).
#
# Usage (direct invocation; do NOT wrap in an outer powershell):
#   & .\verify-prewarm.ps1            # formal 3-process matrix
#   & .\verify-prewarm.ps1 -SelfTest  # judge fixtures only
#
# No -Cases parameter: the formal matrix is fixed (cold -> restored ->
# cold --wrong-key). The script builds the CURRENT target first and refuses to
# run a stale exe.

param([switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$EngineExe = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator\vulkan-engine-smoke\Release\vulkan_pipeline_prewarm_smoke.exe'
$runRootBase = Join-Path $repo 'native\build\vulkan-modernization\prewarm-matrix'

$script:scriptFailedAny = $false

# ---------------------------------------------------------------------------
# Shared judge. Returns $true/$false; sets $script:SavedBytes on success.
# All reserved markers use wide counts + case-sensitive exact whole-line matches.
# ---------------------------------------------------------------------------
function Test-PrewarmLog {
    param(
        [string[]]$Lines,
        [int]$ExitCode,
        [string]$Phase,
        [switch]$WrongKey,
        [object]$ExpectedInitialBytes
    )
    $script:judgeOk = $true
    $script:SavedBytes = 0
    if ($Phase -cnotin @('cold','restored')) { return $false }
    if ($null -eq $Lines -or $Lines.Count -eq 0) {
        $script:judgeOk = $false
        return $false
    }
    $text = ($Lines -join "`n")

    function Fail([string]$why) {
        Write-Host "    judge-FAIL: $why" -ForegroundColor Red
        $script:judgeOk = $false
    }

    # ---- exit code: exact; NTSTATUS / any non-expected value rejected ----
    if ($WrongKey) {
        if ($ExitCode -ne 1) { Fail "exit $ExitCode != exact 1 (negative)" }
    } else {
        if ($ExitCode -ne 0) { Fail "exit $ExitCode != exact 0 (positive)" }
    }

    # ---- reserved markers: wide count each class unique (case-sensitive) ----
    $markers = @(
        'prewarm validation:', 'prewarm mode:', 'prewarm cache-input:', 'prewarm manifest:',
        'prewarm queue:', 'prewarm factory:', 'prewarm saved:', 'prewarm ownership:',
        'prewarm retirement:', 'prewarm timing ns:'
    )
    foreach ($p in $markers) {
        $n = @($Lines | Where-Object { $_ -cne $null -and $_.StartsWith($p) }).Count
        if ($n -ne 1) { Fail "marker '$p' count $n != 1 (missing/duplicate)" }
    }
    # every 'prewarm'-prefixed line must carry a known class token (or be the
    # footer); anything else (no-space/malformed/unknown) rejected.
    $prewarmTotal = 0
    foreach ($line in $Lines) {
        if ($null -ne $line -and $line -match '^prewarm') { $prewarmTotal++ }
        if ($null -eq $line) { continue }
        if ($line -match '^prewarm') {
            $isClass = $line -cmatch '^prewarm (validation|mode|cache-input|manifest|queue|factory|saved|ownership|retirement|timing ns): .+'
            $isFooter = $line -cmatch '^prewarm (cold|restored) completed: failures=\d+ liveErrors=\d+ postErrors=\d+ outcome=(CLEAN|FAILED)$'
            if (-not $isClass -and -not $isFooter) {
                Fail "unknown/no-space prewarm line: '$line'"
            }
        }
    }
    # exactly 11 prewarm-prefixed lines (10 markers + 1 footer)
    if ($prewarmTotal -ne 11) { Fail "prewarm-prefixed line total $prewarmTotal != 11" }

    # ---- validation: exact whole line, case-sensitive ----
    if (-not (($Lines | Where-Object { $_ -ceq 'prewarm validation: ENABLED' }).Count -eq 1)) { Fail 'validation ENABLED exact line' }
    foreach ($line in $Lines) {
        if ($null -ne $line -and $line -cmatch 'prewarm validation' -and $line -cne 'prewarm validation: ENABLED') {
            Fail "malformed validation line: '$line'"
        }
    }

    # ---- API line: reserved unique, boundary-anchored effective=1.3 ----
    $apiLines = @($Lines | Where-Object { $null -ne $_ -and $_ -cmatch 'Vulkan API' })
    if ($apiLines.Count -ne 1) { Fail "API line count $($apiLines.Count) != 1" }
    else {
        # loader/physical may be other valid digits; effective must be exactly 1.3
        if ($apiLines[0] -cnotmatch '^(?:[0-9]{2}:[0-9]{2}:[0-9]{2} \[INFO\]: )?Vulkan API: loader=\d+\.\d+ requested=\d+\.\d+ XR=\d+\.\d+ instance=\d+\.\d+ physical=\d+\.\d+ effective=1\.3$') {
            Fail "API line malformed / effective!=1.3: '$($apiLines[0])'"
        }
    }

    # ---- paths line: reserved unique, full schema, boundary-anchored ----
    $pathLines = @($Lines | Where-Object { $null -ne $_ -and $_ -cmatch 'Vulkan paths' })
    if ($pathLines.Count -ne 1) { Fail "paths line count $($pathLines.Count) != 1" }
    else {
        if ($pathLines[0] -cnotmatch '^(?:[0-9]{2}:[0-9]{2}:[0-9]{2} \[INFO\]: )?Vulkan paths: timeline=[01] sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0$') {
            Fail "paths line malformed: '$($pathLines[0])'"
        }
    }

    # ---- mode ----
    $wantWrong = [int][bool]$WrongKey
    if (-not (($Lines | Where-Object { $_ -ceq "prewarm mode: phase=$Phase wrongKey=$wantWrong" }).Count -eq 1)) {
        Fail "mode line phase=$Phase wrongKey=$wantWrong"
    }

    # ---- manifest / factory / ownership / retirement (exact) ----
    $exact = @(
        'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=1 beforeCreates=0 keys=DISTINCT',
        'prewarm factory: calls=2 driverSuccesses=2 cache=EXACT keys=EXACT',
        'prewarm ownership: queued=1 released=1',
        'prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1'
    )
    foreach ($e in $exact) {
        if (-not (($Lines | Where-Object { $_ -ceq $e }).Count -eq 1)) { Fail "exact line: $e" }
    }

    # ---- saved: positive Int64 canonical digits, validated=1 dirty=0 ----
    $savedLine = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('prewarm saved:') } | Select-Object -First 1
    $savedOk = $false
    if ($savedLine -cmatch '^prewarm saved: bytes=(\d{1,19}) validated=1 dirty=0$') {
        $digits = $Matches[1]
        $parsed = 0L
        if ([int64]::TryParse($digits, [ref]$parsed) -and $parsed -gt 0 -and -not $digits.StartsWith('0')) {
            $savedOk = $true
            $script:SavedBytes = $parsed
        }
    }
    if (-not $savedOk) { Fail "saved line malformed/non-positive: '$savedLine'" }

    # ---- timing: five complete non-negative Int64 fields, no junk ----
    $timingLine = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('prewarm timing ns:') } | Select-Object -First 1
    if ($timingLine -cmatch '^prewarm timing ns: configure=(\d{1,19}) factory1=(\d{1,19}) factory2=(\d{1,19}) driver1=(\d{1,19}) driver2=(\d{1,19})$') {
        for ($k = 1; $k -le 5; $k++) {
            $digits = $Matches[$k]
            $v = 0L
            if (-not [int64]::TryParse($digits, [ref]$v) -or ($digits.Length -gt 1 -and $digits.StartsWith('0'))) {
                Fail "timing field $k noncanonical or overflow"
            }
        }
    } else { Fail "timing line malformed: '$timingLine'" }

    # ---- cache-input ----
    $ciLine = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('prewarm cache-input:') } | Select-Object -First 1
    if ($Phase -eq 'cold') {
        if ($ciLine -cne 'prewarm cache-input: calls=1 loaded=0 initialBytes=0 payload=0') { Fail 'cache-input cold exact' }
    } elseif ($Phase -eq 'restored') {
        if ($ciLine -cmatch '^prewarm cache-input: calls=1 loaded=1 initialBytes=(\d{1,19}) payload=1$') {
            $init = 0L
            if (-not [int64]::TryParse($Matches[1], [ref]$init) -or $init -le 0 -or $Matches[1].StartsWith('0')) {
                Fail 'restored initialBytes must be positive Int64'
            } elseif ($null -ne $ExpectedInitialBytes) {
                # accept any provided numeric type; mismatch (including long) fails
                $expectedVal = 0L
                $providedOk = $false
                try {
                    if ($ExpectedInitialBytes -is [int] -or $ExpectedInitialBytes -is [long] -or
                        $ExpectedInitialBytes -is [int64]) {
                        $expectedVal = [int64]$ExpectedInitialBytes
                        $providedOk = $true
                    } elseif ($ExpectedInitialBytes -is [string]) {
                        $providedOk = [int64]::TryParse($ExpectedInitialBytes, [ref]$expectedVal)
                    }
                } catch { $providedOk = $false }
                if (-not $providedOk) {
                    Fail "ExpectedInitialBytes not a usable number: '$ExpectedInitialBytes'"
                } elseif ($init -ne $expectedVal) {
                    Fail "restored initialBytes $init != expected $expectedVal"
                }
            }
        } else { Fail "cache-input restored malformed: '$ciLine'" }
    }

    # ---- queue ----
    if ($WrongKey) {
        if (-not (($Lines | Where-Object { $_ -ceq 'prewarm queue: attempted=2 ready=1 failed=1 replay=0 busy=0' }).Count -eq 1)) { Fail 'queue wrong exact' }
    } else {
        if (-not (($Lines | Where-Object { $_ -ceq 'prewarm queue: attempted=2 ready=2 failed=0 replay=0 busy=0' }).Count -eq 1)) { Fail 'queue positive exact' }
    }

    # ---- leak: reserved wide count, unique, anchored ^[timestamp] Total ... 0 bytes.$ ----
    $leaks = @($Lines | Where-Object { $null -ne $_ -and $_ -match 'Total device memory leaked' })
    if ($leaks.Count -ne 1) { Fail "leak line count $($leaks.Count) != 1" } else {
        if ($leaks[0] -cnotmatch '^(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?Total device memory leaked: 0 bytes\.$') {
            Fail "leak line malformed/nonzero: '$($leaks[0])'"
        }
    }

    # ---- footer: ANY line containing word completed (or fragments) wide-counted ----
    $completed = @($Lines | Where-Object { $null -ne $_ -and $_ -cmatch 'completed' })
    if ($completed.Count -ne 1) { Fail "completed-word line count $($completed.Count) != 1" }
    foreach ($line in $completed) {
        if ($line -cnotmatch '^prewarm (cold|restored) completed: failures=\d+ liveErrors=\d+ postErrors=\d+ outcome=(CLEAN|FAILED)$') {
            Fail "malformed footer: '$line'"
        }
    }
    $wantFooter = "prewarm $Phase completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN"
    if ($WrongKey) {
        $wantFooter = "prewarm $Phase completed: failures=1 liveErrors=0 postErrors=0 outcome=FAILED"
    }
    if (-not (($Lines | Where-Object { $_ -ceq $wantFooter }).Count -eq 1)) { Fail "footer exact '$wantFooter'" }

    # ---- FAIL lines policy ----
    $failLines = @($Lines | Where-Object { $null -ne $_ -and $_ -cmatch 'FAIL:' })
    if ($WrongKey) {
        if ($failLines.Count -ne 1) { Fail "wrong mode FAIL lines $($failLines.Count) != 1" }
        elseif ($failLines[0] -cne 'FAIL: prewarm rejected creator key') { Fail "wrong sole FAIL mismatch: '$($failLines[0])'" }
    } else {
        if ($failLines.Count -gt 0) { Fail 'positive log has FAIL lines' }
    }
    foreach ($bad in @('VUID-', 'HAZARD', 'SKIPPED', 'INCOMPLETE', 'UNSUPPORTED')) {
        if ($text -cmatch [regex]::Escape($bad)) { Fail "forbidden token $bad" }
    }
    if (-not $script:judgeOk) { $script:SavedBytes = 0 }
    return $script:judgeOk
}

# ---------------------------------------------------------------------------
# SelfTest fixtures
# ---------------------------------------------------------------------------
function Invoke-SelfTest {
    $api = '03:41:05 [INFO]: Vulkan API: loader=1.4 requested=1.3 XR=1.3 instance=1.3 physical=1.4 effective=1.3'
    $paths = '03:41:05 [INFO]: Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0'
    $leak = '03:41:05 [INFO]: Total device memory leaked: 0 bytes.'

    $cold = @(
        $api, $paths,
        'prewarm validation: ENABLED',
        'prewarm mode: phase=cold wrongKey=0',
        'prewarm cache-input: calls=1 loaded=0 initialBytes=0 payload=0',
        'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=1 beforeCreates=0 keys=DISTINCT',
        'prewarm queue: attempted=2 ready=2 failed=0 replay=0 busy=0',
        'prewarm factory: calls=2 driverSuccesses=2 cache=EXACT keys=EXACT',
        'prewarm saved: bytes=11857 validated=1 dirty=0',
        'prewarm ownership: queued=1 released=1',
        'prewarm timing ns: configure=120200 factory1=1866600 factory2=252600 driver1=1863400 driver2=251500',
        $leak,
        'prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1',
        'prewarm cold completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN'
    )
    $restored = @(
        $api, $paths,
        'prewarm validation: ENABLED',
        'prewarm mode: phase=restored wrongKey=0',
        'prewarm cache-input: calls=1 loaded=1 initialBytes=11857 payload=1',
        'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=1 beforeCreates=0 keys=DISTINCT',
        'prewarm queue: attempted=2 ready=2 failed=0 replay=0 busy=0',
        'prewarm factory: calls=2 driverSuccesses=2 cache=EXACT keys=EXACT',
        'prewarm saved: bytes=11857 validated=1 dirty=0',
        'prewarm ownership: queued=1 released=1',
        'prewarm timing ns: configure=120200 factory1=1866600 factory2=252600 driver1=1863400 driver2=251500',
        $leak,
        'prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1',
        'prewarm restored completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN'
    )
    $wrong = @(
        $api, $paths,
        'prewarm validation: ENABLED',
        'prewarm mode: phase=cold wrongKey=1',
        'prewarm cache-input: calls=1 loaded=0 initialBytes=0 payload=0',
        'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=1 beforeCreates=0 keys=DISTINCT',
        'prewarm queue: attempted=2 ready=1 failed=1 replay=0 busy=0',
        'prewarm factory: calls=2 driverSuccesses=2 cache=EXACT keys=EXACT',
        'prewarm saved: bytes=11857 validated=1 dirty=0',
        'prewarm ownership: queued=1 released=1',
        'prewarm timing ns: configure=120200 factory1=1866600 factory2=252600 driver1=1863400 driver2=251500',
        $leak,
        'prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1',
        'FAIL: prewarm rejected creator key',
        'prewarm cold completed: failures=1 liveErrors=0 postErrors=0 outcome=FAILED'
    )

    $fixtures = @()
    $fixtures += @{ Name = 'canon-cold'; Lines = $cold; Exit = 0; Phase = 'cold'; Want = $true }
    $fixtures += @{ Name = 'canon-restored'; Lines = $restored; Exit = 0; Phase = 'restored'; Want = $true; ExpectedInitialBytes = [long]11857 }
    $fixtures += @{ Name = 'long-matched'; Lines = $restored; Exit = 0; Phase = 'restored'; Want = $true; ExpectedInitialBytes = [long]11857 }
    $fixtures += @{ Name = 'canon-wrong'; Lines = $wrong; Exit = 1; Phase = 'cold'; WrongKey = $true; Want = $true }

    # ---- Lead's 7 parser reds, now must be False ----
    $fixtures += @{ Name = 'red-long-mismatch'; Lines = $restored; Exit = 0; Phase = 'restored'; Want = $false; ExpectedInitialBytes = [long]999 }
    $fixtures += @{ Name = 'red-long-mismatch-int'; Lines = $restored; Exit = 0; Phase = 'restored'; Want = $false; ExpectedInitialBytes = 999 }
    $fixtures += @{ Name = 'red-junk-leak'; Lines = ($cold + @('junk Total device memory leaked: 0 bytes.')); Exit = 0; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'red-nospace-disabled'; Lines = ($cold + @('prewarmvalidation: DISABLED')); Exit = 0; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'red-foreign-truncated-footer'; Lines = ($cold + @('foo completed failures=1')); Exit = 0; Phase = 'cold'; Want = $false }
    $a2 = $cold.Clone(); $a2[0] = $api.Replace('effective=1.3', 'effective=1.30'); $fixtures += @{ Name = 'red-effective-1.30'; Lines = $a2; Exit = 0; Phase = 'cold'; Want = $false }
    $p2 = $cold.Clone(); $p2[1] = $paths.Replace('sync2=0', 'sync2=01'); $fixtures += @{ Name = 'red-sync2-01'; Lines = $p2; Exit = 0; Phase = 'cold'; Want = $false }
    $p2 = $cold.Clone(); $p2 += $paths; $fixtures += @{ Name = 'red-duplicate-paths'; Lines = $p2; Exit = 0; Phase = 'cold'; Want = $false }

    # ---- additional single-variable negatives ----
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=0 liveErrors=0 postErrors=0';                  $fixtures += @{ Name = 'footer-truncated'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=01 liveErrors=0 postErrors=0 outcome=CLEAN';   $fixtures += @{ Name = 'footer-01'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=0 liveErrors=0 postErrors=0 outcome=FAILED';   $fixtures += @{ Name = 'footer-outcome'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=1 liveErrors=0 postErrors=0 outcome=CLEAN';    $fixtures += @{ Name = 'footer-failures1'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=0 liveErrors=1 postErrors=0 outcome=CLEAN';    $fixtures += @{ Name = 'footer-live1'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm cold completed: failures=0 liveErrors=0 postErrors=1 outcome=CLEAN';    $fixtures += @{ Name = 'footer-post1'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2 += 'prewarm cold completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN';       $fixtures += @{ Name = 'footer-duplicate'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'exit-ntstatus'; Lines = $cold; Exit = -1073740791; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'exit-positive-nonzero'; Lines = $cold; Exit = 7; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'negative-exit-0'; Lines = $wrong.Clone(); Exit = 0; Phase = 'cold'; WrongKey = $true; Want = $false }
    $fixtures += @{ Name = 'negative-exit-2'; Lines = $wrong.Clone(); Exit = 2; Phase = 'cold'; WrongKey = $true; Want = $false }
    $c2 = $cold.Clone(); $c2[2] = 'prewarm validation: DISABLED';                                                    $fixtures += @{ Name = 'validation-disabled'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2 += 'prewarm validation: ENABLED';                                                       $fixtures += @{ Name = 'validation-duplicate'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $a2 = $cold.Clone(); $a2 += $api;                                                                                $fixtures += @{ Name = 'api-duplicate'; Lines = $a2; Exit = 0; Phase = 'cold'; Want = $false }
    $a2 = $cold.Clone(); $a2[0] = $api + ' trailing';                                                                $fixtures += @{ Name = 'api-tail-junk'; Lines = $a2; Exit = 0; Phase = 'cold'; Want = $false }
    $p2 = $cold.Clone(); $p2[1] = $paths + ' trailing';                                                              $fixtures += @{ Name = 'paths-tail-junk'; Lines = $p2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[4] = 'prewarm cacheinput: calls=1 loaded=0 initialBytes=0 payload=0';                  $fixtures += @{ Name = 'marker-nospace'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[4] = 'prewarm cache-extra: junk';                                                       $fixtures += @{ Name = 'marker-unknown'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $fixtures += @{ Name = 'phase-unknown-arg'; Lines = $cold; Exit = 0; Phase = 'unknownPhase'; Want = $false }
    $c2 = $cold.Clone(); $c2[6] = 'prewarm queue: attempted=0 ready=2 failed=0 replay=0 busy=0';                    $fixtures += @{ Name = 'queue-zero-attempted'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[5] = 'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=0 beforeCreates=0 keys=DISTINCT'; $fixtures += @{ Name = 'manifest-no-duplicate'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[5] = 'prewarm manifest: entries=2 modules=2 layouts=1 duplicate=1 beforeCreates=1 keys=DISTINCT'; $fixtures += @{ Name = 'manifest-before1'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[8] = 'prewarm saved: bytes=0 validated=1 dirty=0';                                      $fixtures += @{ Name = 'saved-bytes-0'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[8] = 'prewarm saved: bytes=11857 validated=1 dirty=1';                                  $fixtures += @{ Name = 'saved-dirty-1'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[8] = 'prewarm saved: bytes=99999999999999999999999 validated=1 dirty=0';                $fixtures += @{ Name = 'saved-int64-overflow'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $r2 = $restored.Clone(); $r2[4] = 'prewarm cache-input: calls=1 loaded=1 initialBytes=11857 payload=0';          $fixtures += @{ Name = 'restored-payload-only0'; Lines = $r2; Exit = 0; Phase = 'restored'; Want = $false }
    $c2 = $cold.Clone(); $c2[10] = 'prewarm timing ns: configure=120200 factory1=1866600 factory2=252600 driver1=1863400'; $fixtures += @{ Name = 'timing-missing-field'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[10] = 'prewarm timing ns: configure=120200 factory1=1866600 factory2=252600 driver1=1863400 driver2=251500 junk'; $fixtures += @{ Name = 'timing-tail-junk'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = @();                                                                                                       $fixtures += @{ Name = 'empty-lines'; Lines = $c2; Exit = 0; Phase = 'cold'; Want = $false }
    $c2 = $cold.Clone(); $c2[13] = 'prewarm restor completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN';   $fixtures += @{ Name = 'footer-phase-mismatch'; Lines = $c2; Exit = 0; Phase = 'restored'; Want = $false }

    # Lead rev3: each mutation changes exactly one line/field/argument.
    $leadMutations = @(
        @{Name='lead-junk-leak-replacement'; Index=11; Value='junk Total device memory leaked: 0 bytes.'},
        @{Name='lead-junk-paths-replacement'; Index=1; Value=('junk '+$paths)},
        @{Name='lead-junk-api-replacement'; Index=0; Value=('junk '+$api)},
        @{Name='lead-timing-overflow'; Index=10; Value=($cold[10] -replace 'configure=\d+','configure=9999999999999999999')},
        @{Name='lead-footer-foreign'; Index=13; Value='prewarm_cold completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN'},
        @{Name='lead-validation-unavailable'; Index=2; Value='prewarm validation: UNAVAILABLE'},
        @{Name='lead-leak-nonzero'; Index=11; Value=($leak -replace ' 0 bytes',' 1 bytes')},
        @{Name='lead-sync2-on'; Index=1; Value=($paths -replace 'sync2=0','sync2=1')},
        @{Name='lead-effective-1.2'; Index=0; Value=($api -replace 'effective=1\.3','effective=1.2')}
    )
    foreach ($m in $leadMutations) {
        $changed = $cold.Clone(); $changed[$m.Index] = $m.Value
        $fixtures += @{Name=$m.Name; Lines=$changed; Exit=0; Phase='cold'; Want=$false}
    }
    foreach ($m in @(
        @{Name='lead-vuid'; Value='VUID-test-00000'},
        @{Name='lead-hazard'; Value='HAZARD-test'},
        @{Name='lead-positive-fail'; Value='[INFO]: FAIL: unexpected'},
        @{Name='lead-incomplete'; Value='INCOMPLETE: missing phase'},
        @{Name='lead-skipped'; Value='SKIPPED: phase'},
        @{Name='lead-duplicate-leak'; Value=$leak},
        @{Name='lead-no-space-appended'; Value='prewarmvalidation: DISABLED'},
        @{Name='lead-malformed-footer-appended'; Value='foo completed failures=1'}
    )) {
        $fixtures += @{Name=$m.Name; Lines=($cold+@($m.Value)); Exit=0; Phase='cold'; Want=$false}
    }
    $fixtures += @{Name='lead-missing-footer'; Lines=@($cold[0..12]); Exit=0; Phase='cold'; Want=$false}
    $fixtures += @{Name='lead-null-lines'; Lines=$null; Exit=0; Phase='cold'; Want=$false}
    $fixtures += @{Name='lead-arbitrary-phase'; Lines=$cold; Exit=0; Phase='bogus'; Want=$false}
    $fixtures += @{Name='lead-auto-fence-valid'; Lines=@($cold | ForEach-Object { $_ -replace 'timeline=1','timeline=0' }); Exit=0; Phase='cold'; Want=$true}
    $changed=$restored.Clone(); $changed[4]=$restored[4] -replace 'initialBytes=11857','initialBytes=011857'
    $fixtures += @{Name='lead-initial-leading-zero'; Lines=$changed; Exit=0; Phase='restored'; Want=$false}
    $changed=$wrong.Clone(); $changed[13]='FAIL: unrelated error'
    $fixtures += @{Name='lead-wrong-unrelated-fail'; Lines=$changed; Exit=1; Phase='cold'; WrongKey=$true; Want=$false}
    $fixtures += @{Name='lead-wrong-duplicate-fail'; Lines=($wrong+@('FAIL: prewarm rejected creator key')); Exit=1; Phase='cold'; WrongKey=$true; Want=$false}
    $changed=$wrong.Clone(); $changed[14]=$wrong[14] -replace 'failures=1','failures=0'
    $fixtures += @{Name='lead-wrong-zero-failures'; Lines=$changed; Exit=1; Phase='cold'; WrongKey=$true; Want=$false}

    $declared = $fixtures.Count
    $executed = 0
    $passed = 0
    foreach ($f in $fixtures) {
        $executed++
        $params = @{ Lines = [string[]]$f.Lines; ExitCode = [int]$f.Exit; Phase = $f.Phase }
        if ($f.WrongKey) { $params.WrongKey = $true }
        if ($null -ne $f.ExpectedInitialBytes) { $params.ExpectedInitialBytes = $f.ExpectedInitialBytes }
        $result = Test-PrewarmLog @params
        if ($result -eq $f.Want) { $passed++ }
        else {
            Write-Host "  SELFTEST FAIL: $($f.Name) (got $result want $($f.Want))" -ForegroundColor Red
            $script:scriptFailedAny = $true
        }
    }
    Write-Host ("SelfTest fixtures declared={0} executed={1} passed={2}" -f $declared, $executed, $passed)
    if ($passed -ne $declared -or $executed -ne $declared) { $script:scriptFailedAny = $true }
}

# ---------------------------------------------------------------------------
# formal run
# ---------------------------------------------------------------------------
function Invoke-Formal {
    Write-Host 'building vulkan_pipeline_prewarm_smoke (current source)...' -ForegroundColor Cyan
    & cmake --build (Join-Path $repo 'native\build\vulkan-modernization\win64-simulator') --config Release --target vulkan_pipeline_prewarm_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "prewarm harness build failed (exit $LASTEXITCODE); refusing a stale exe" }
    if (-not (Test-Path $EngineExe)) { throw "harness exe missing: $EngineExe" }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $runRoot = Join-Path $runRootBase ("run-$stamp-" + $PID)
    New-Item -ItemType Directory -Force -Path $runRootBase | Out-Null
    New-Item -ItemType Directory -Path $runRoot | Out-Null
    Write-Host "run root: $runRoot"

    # save by existence (not truthiness); empty string preserved
    $saved = @{}
    foreach ($name in @('PATH', 'VK_LAYER_PATH', 'VK_IMPLICIT_LAYER_PATH', 'CC_VULKAN_FORCE_API',
            'CC_VULKAN_COMPLETION', 'CC_VULKAN_SUBMIT', 'CC_VULKAN_BARRIER',
            'CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES')) {
        $item = Get-Item ("Env:" + $name) -ErrorAction SilentlyContinue
        if ($null -ne $item) { $saved[$name] = $item.Value }
    }
    $oldCwd = (Get-Location).Path
    try {
        $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
        $emptyImplicit = Join-Path $runRoot 'empty-implicit'
        New-Item -ItemType Directory -Force -Path $emptyImplicit | Out-Null
        $env:VK_IMPLICIT_LAYER_PATH = $emptyImplicit
        $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' +
                    (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
        $env:CC_VULKAN_FORCE_API = '1.3'
        $env:CC_VULKAN_COMPLETION = 'auto'
        $env:CC_VULKAN_SUBMIT = 'legacy'
        $env:CC_VULKAN_BARRIER = 'legacy'
        Remove-Item Env:CC_VULKAN_RENDERING -ErrorAction SilentlyContinue
        Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue

        Set-Location $runRoot
        $cases = @(
            @{ Name = 'cold';     Args = @('--phase', 'cold') },
            @{ Name = 'restored'; Args = @('--phase', 'restored') },
            @{ Name = 'wrong';    Args = @('--phase', 'cold', '--wrong-key') }
        )
        [long]$coldSavedBytes = 0
        foreach ($case in $cases) {
            $log = Join-Path $runRoot ($case.Name + '.log')
            cmd /c """$EngineExe"" $($case.Args -join ' ') > ""$log"" 2>&1"
            $code = $LASTEXITCODE
            $lines = @(Get-Content $log -ErrorAction SilentlyContinue)
            $phase = 'cold'; if ($case.Name -eq 'restored') { $phase = 'restored' }
            $params = @{ Lines = $lines; ExitCode = $code; Phase = $phase }
            if ($case.Name -eq 'wrong') { $params.WrongKey = $true }
            if ($case.Name -eq 'restored') {
                if ($coldSavedBytes -le 0) { $params.ExpectedInitialBytes = [long]::MinValue } # forces mismatch
                else { $params.ExpectedInitialBytes = $coldSavedBytes }
            }
            $script:SavedBytes = 0
            $judge = Test-PrewarmLog @params
            if ($case.Name -eq 'cold') {
                $coldSavedBytes = 0L
                if ($judge) { $coldSavedBytes = [long]$script:SavedBytes }
                if ($coldSavedBytes -le 0) { $script:scriptFailedAny = $true; Write-Host '  cold judge FAIL must not yield stale SavedBytes' -ForegroundColor Red }
            }
            if (-not $judge) { $script:scriptFailedAny = $true }
            Write-Host ("  {0}: exit={1} judge={2} savedBytes={3}" -f $case.Name, $code, $(if ($judge) { 'PASS' } else { 'FAIL' }), $script:SavedBytes) -ForegroundColor $(if ($judge) { 'Green' } else { 'Red' })
        }
    } catch {
        Write-Host "EXCEPTION: $_" -ForegroundColor Red
        $script:scriptFailedAny = $true
    } finally {
        foreach ($name in @('PATH', 'VK_LAYER_PATH', 'VK_IMPLICIT_LAYER_PATH', 'CC_VULKAN_FORCE_API',
                'CC_VULKAN_COMPLETION', 'CC_VULKAN_SUBMIT', 'CC_VULKAN_BARRIER',
                'CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES')) {
            if ($saved.ContainsKey($name)) { Set-Item -Path ("Env:" + $name) -Value $saved[$name] }
            else { Remove-Item ("Env:" + $name) -ErrorAction SilentlyContinue }
        }
        Set-Location $oldCwd
    }
}

if ($SelfTest) {
    Invoke-SelfTest
} else {
    Invoke-Formal
}
if ($script:scriptFailedAny) { exit 1 }
exit 0
