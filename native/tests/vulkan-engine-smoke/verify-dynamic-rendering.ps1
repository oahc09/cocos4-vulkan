# M6-A0..M6-A2b: diagnostic comparisons and the engine Hybrid path.
# Builds the CURRENT target first (red build fails fast), isolates the policy
# environment (legacy submit/barrier forced so the vkQueueSubmit hook observes
# the real path), runs each case in its own process/log, judges with strict
# full-line-anchored evidence; SelfTest exercises the judge with single-
# condition fixtures (including the four Lead red samples).
#   powershell -ExecutionPolicy Bypass -File verify-dynamic-rendering.ps1 [-Cases classic,compare] [-SelfTest]
param(
    [string[]]$Cases = @(),
    [switch]$SelfTest,
    [switch]$EnginePath
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$buildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator'
$exe = Join-Path $buildDir 'vulkan-engine-smoke\Release\vulkan_dynamic_rendering_smoke.exe'
$outRoot = Join-Path $repo 'native\build\vulkan-modernization\dyn-render-matrix'
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$runRoot = Join-Path $outRoot "run-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

function Get-DynamicEvidenceHandle {
    param([string]$Line, [string]$Field)
    $pattern = '(?:^| )' + [regex]::Escape($Field) + '=((?:0[xX])?[0-9A-Fa-f]+)(?: |$)'
    $m = [regex]::Match($Line, $pattern)
    if (-not $m.Success) { return '' }
    return (($m.Groups[1].Value -replace '^0[xX]', '').TrimStart([char]'0')).ToUpperInvariant()
}

function Test-DynamicRenderingLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$CaseName = 'classic',
        [switch]$EnginePath
    )
    $reasons = @()
    if ($CaseName -notin @('classic','compare','draw-classic','draw-compare','owned-compare','owned-failure')) {
        return [pscustomobject]@{ Pass = $false; Reasons = @("unknown case $CaseName") }
    }
    $isCompare = $CaseName -in @('compare','draw-compare','owned-compare')
    $isDraw = $CaseName -in @('draw-classic','draw-compare','owned-compare','owned-failure')
    $isOwned = $CaseName -in @('owned-compare','owned-failure')
    if ($EnginePath -and -not $isOwned) {
        return [pscustomobject]@{ Pass = $false; Reasons = @('EnginePath requires an owned case') }
    }
    if ($ExitCode -ne 0) { $reasons += "exit $ExitCode != 0" }
    if (@($Lines | Where-Object { $_ -match '^validation \+ best practices \+ sync: ENABLED$' }).Count -ne 1 -or
        @($Lines | Where-Object { $_ -match '^validation \+ best practices \+ sync:' }).Count -ne 1) {
        $reasons += "validation not ENABLED"
    }
    # Runtime-path evidence: exactly one line with the selected mode and capability.
    $wantRuntime = if ($EnginePath) { 1 } else { 0 }
    $runtime = @($Lines | Where-Object { $_ -match ("^runtime: dynamicRendering=$wantRuntime capsEnabled=[01] effective=1\.3$") })
    $anyRuntime = @($Lines | Where-Object { $_ -match '^runtime: ' })
    if ($runtime.Count -ne 1) { $reasons += "runtime evidence count=$($runtime.Count) (expected exactly 1 dynamicRendering=$wantRuntime line)" }
    elseif ($anyRuntime.Count -ne 1) { $reasons += "extra runtime lines: $($anyRuntime.Count)" }
    elseif (($isCompare -or $isOwned) -and $runtime[0] -notmatch 'capsEnabled=1 ') {
        $reasons += 'compare requires enabled Dynamic Rendering'
    }
    # footer: exact clean line; count ALL footers regardless of prefix
    $footers = @($Lines | Where-Object { $_ -match ("^" + [regex]::Escape($CaseName) + " completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN$") })
    $anyFooters = @($Lines | Where-Object { $_ -match '^.* completed:' })
    if ($anyFooters.Count -ne 1) { $reasons += "footer count=$($anyFooters.Count) (expected 1 total)" }
    elseif ($footers.Count -ne 1) { $reasons += "footer present but not clean: $($anyFooters[0])" }
    if ($Lines | Where-Object { $_ -match 'SKIPPED|INCOMPLETE|UNSUPPORTED' }) { $reasons += "SKIPPED/INCOMPLETE/UNSUPPORTED" }
    $vuids = ($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count
    if ($vuids -gt 0) { $reasons += "$vuids VUID/hazard lines" }
    $fails = ($Lines | Where-Object { $_ -match '^FAIL:' }).Count
    if ($fails -gt 0) { $reasons += "$fails FAIL lines" }
    # bytes line: case-prefixed, exact size, zero mismatches, unique
    $bytes = @($Lines | Where-Object { $_ -match ("^" + [regex]::Escape($CaseName) + ": size=17x11 totalBytes=748 mismatches=0$") })
    if ($bytes.Count -ne 1) { $reasons += "clean bytes line count=$($bytes.Count) (expected 1)" }
    if (@($Lines | Where-Object { $_ -match '^[A-Za-z-]+: size=' }).Count -ne 1) { $reasons += 'extra bytes lines' }
    # phase evidence (full-line anchors, real pointer values)
    $nonzeroHandle = '(?:0[xX])?0*[1-9A-Fa-f][0-9A-Fa-f]*'
    $pc = @($Lines | Where-Object { $_ -match ("^phase-classic: rp=1/1 cb=$nonzeroHandle chain=1$") })
    if ($pc.Count -ne 1) { $reasons += "phase-classic evidence count=$($pc.Count)" }
    if (@($Lines | Where-Object { $_ -match '^phase-classic:' }).Count -ne 1) { $reasons += 'extra/missing classic phase lines' }
    if (@($Lines | Where-Object { $_ -match '^phase-dynamic:' }).Count -ne 1) { $reasons += 'extra/missing dynamic phase lines' }
    if ($CaseName -in @('classic','draw-classic','owned-failure')) {
        $pd = @($Lines | Where-Object { $_ -match '^phase-dynamic: calls=0/0 \(actual counters\)$' })
        if ($pd.Count -ne 1) { $reasons += "phase-dynamic(actual-zero) count=$($pd.Count)" }
    } else {
        $pd = @($Lines | Where-Object { $_ -match ("^phase-dynamic: calls=1/1 cb=$nonzeroHandle chain=1$") })
        if ($pd.Count -ne 1) { $reasons += "phase-dynamic(1/1 chain) count=$($pd.Count)" }
        $diff = @($Lines | Where-Object { $_ -match '^compare: classicVsDynamic diff bytes=0/748$' })
        if ($diff.Count -ne 1) { $reasons += "missing/unclean classicVsDynamic diff line" }
    }
    # M6-A1 draw evidence: real pipeline creation + per-branch bind/draw chains
    if ($isDraw) {
        $pipelineClassic = @($Lines | Where-Object { $_ -match ("^pipeline-classic: result=0 handle=$nonzeroHandle subpass=0$") })
        if ($pipelineClassic.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-classic:' }).Count -ne 1) {
            $reasons += 'invalid/missing/extra pipeline-classic evidence'
        }
        $dc = @($Lines | Where-Object { $_ -match ("^phase-draw-classic: bind=$nonzeroHandle draw=1 cb=$nonzeroHandle chain=1$") })
        if ($dc.Count -ne 1) { $reasons += "phase-draw-classic chain count=$($dc.Count)" }
        elseif ($pipelineClassic.Count -eq 1 -and $pc.Count -eq 1) {
            $created = Get-DynamicEvidenceHandle $pipelineClassic[0] 'handle'
            $bound = Get-DynamicEvidenceHandle $dc[0] 'bind'
            if ($created -ne $bound) { $reasons += "draw-classic bind handle != created pipeline" }
            if ((Get-DynamicEvidenceHandle $dc[0] 'cb') -ne
                (Get-DynamicEvidenceHandle $pc[0] 'cb')) { $reasons += 'classic draw CB != render/copy/submit CB' }
        }
        if (@($Lines | Where-Object { $_ -match '^phase-draw-classic:' }).Count -ne 1) { $reasons += 'extra/missing classic draw lines' }
        if ($CaseName -in @('draw-compare','owned-compare')) {
            $pv = @($Lines | Where-Object { $_ -match ("^pipeline-dynamic: result=0 handle=$nonzeroHandle colors=1 samples=1$") })
            if ($pv.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-dynamic:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-dynamic evidence' }
            $dd = @($Lines | Where-Object { $_ -match ("^phase-draw-dynamic: bind=$nonzeroHandle draw=1 cb=$nonzeroHandle chain=1$") })
            if ($dd.Count -ne 1) { $reasons += "phase-draw-dynamic chain count=$($dd.Count)" }
            elseif ($pv.Count -eq 1 -and $pd.Count -eq 1) {
                if ((Get-DynamicEvidenceHandle $pv[0] 'handle') -ne
                    (Get-DynamicEvidenceHandle $dd[0] 'bind')) { $reasons += 'dynamic draw bind != created pipeline' }
                if ((Get-DynamicEvidenceHandle $pd[0] 'cb') -ne
                    (Get-DynamicEvidenceHandle $dd[0] 'cb')) { $reasons += 'dynamic draw CB != render/copy/submit CB' }
            }
            if (@($Lines | Where-Object { $_ -match '^phase-draw-dynamic:' }).Count -ne 1) { $reasons += 'extra/missing dynamic draw lines' }
            if (@($Lines | Where-Object { $_ -match '^pipeline-signature: color=37 depth=0 stencil=0 samples=1 viewMask=0 rpNull=1 subpass=0$' }).Count -ne 1 -or
                @($Lines | Where-Object { $_ -match '^pipeline-signature:' }).Count -ne 1) { $reasons += 'missing/incorrect actual Dynamic pipeline signature' }
        } else {
            $badDyn = @($Lines | Where-Object { $_ -match '^pipeline-dynamic:' -or $_ -match '^phase-draw-dynamic:' })
            if ($badDyn.Count -ne 0) { $reasons += "classic-only cases must not create/use a dynamic variant" }
        }
    }
    # every draw case prints its real creation accounting
    if ($isDraw) {
        $wantDyn = if ($isOwned) { 1 } else { 0 }
        $wantElig = if ($CaseName -eq 'owned-failure') { 1 } else { 0 }
        $wantDiag = if ($CaseName -eq 'draw-compare') { 1 } else { 0 }
        $cr = @($Lines | Where-Object { $_ -match ("^pipeline-creates: main=1 dynamic=$wantDyn eligibility=$wantElig unknown=0 diagnostic=$wantDiag$") })
        if ($cr.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-creates:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-creates (draw)' }
        if ($CaseName -eq 'draw-classic') {
            if (@($Lines | Where-Object { $_ -match '^pipeline-baseline: policy=OFF variant=NULL creates=0$' }).Count -ne 1 -or
                @($Lines | Where-Object { $_ -match '^pipeline-baseline:' }).Count -ne 1) { $reasons += 'missing/unclean policy-OFF baseline line' }
        }
    }

    # ---- M6-A2b formal engine-path evidence ----
    $enginePathLines = @($Lines | Where-Object { $_ -match '^engine-path:' })
    $engineCallsLines = @($Lines | Where-Object { $_ -match '^engine-calls:' })
    if ($EnginePath) {
        if ($enginePathLines.Count -ne 1) { $reasons += "engine-path line count=$($enginePathLines.Count) (expected 1)" }
        elseif ($enginePathLines[0] -notmatch '^engine-path: gfxRendering=1 gfxDraw=1 runtimeDynamic=1 restored=1$') {
            $reasons += "engine-path line not exact: $($enginePathLines[0])"
        }
        if ($engineCallsLines.Count -ne 1) { $reasons += "engine-calls line count=$($engineCallsLines.Count) (expected 1)" }
        else {
            $wantCalls = if ($CaseName -eq 'owned-compare') { 'classic=1/1 dynamic=1/1 draws=2 submits=2/2' } else { 'classic=1/1 dynamic=0/0 draws=1 submits=1/1' }
            if ($engineCallsLines[0] -ne "engine-calls: $wantCalls") { $reasons += "engine-calls mismatch: $($engineCallsLines[0])" }
        }
    } else {
        # diagnostic-mode logs must NOT carry engine-path evidence
        if ($enginePathLines.Count -ne 0) { $reasons += 'diagnostic log carries engine-path evidence' }
        if ($engineCallsLines.Count -ne 0) { $reasons += 'diagnostic log carries engine-calls evidence' }
    }

    # M7-F4: full input key publication; reserved prefix catches malformed extras.
    $keyLines = @($Lines | Where-Object { $_ -match '^pipeline-keys' })
    if ($isOwned) {
        $wantKey = if ($CaseName -eq 'owned-compare') { 'EXACT' } else { 'EMPTY' }
        if ($keyLines.Count -ne 1 -or $keyLines[0] -ne "pipeline-keys: classic=EXACT dynamic=$wantKey ownership=OK") {
            $reasons += 'invalid/missing/extra complete pipeline key evidence'
        }
    } elseif ($keyLines.Count -ne 0) { $reasons += 'non-owned case carries owned key evidence' }

    # ---- M6-A2a owned-case evidence ----
    if ($isOwned) {
        $H = $nonzeroHandle
        $wantRet  = if ($CaseName -eq 'owned-compare') { 'classicDestroyed=1 dynDestroyed=1 partialDestroyed=0 eligibilityDestroyed=0' } else { 'classicDestroyed=1 dynDestroyed=0 partialDestroyed=1 eligibilityDestroyed=1' }
        $ret = @($Lines | Where-Object { $_ -match ("^retirement: $wantRet$") })
        if ($ret.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^retirement:' }).Count -ne 1) { $reasons += 'invalid/missing/extra retirement line' }
        # pending-cache: unique line, pending == the LAST executed branch CB
        $pCache = @($Lines | Where-Object { $_ -match ("^pending-cache: pending=$H fresh=$H distinct=1$") })
        if ($pCache.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pending-cache:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pending-cache line' }
        elseif ($pd.Count -eq 1 -and $pc.Count -eq 1) {
            $execCBsrc = if ($CaseName -eq 'owned-compare') { $pd[0] } else { $pc[0] }
            $execCB = Get-DynamicEvidenceHandle $execCBsrc 'cb'
            $pendingCB = Get-DynamicEvidenceHandle $pCache[0] 'pending'
            $freshCB = Get-DynamicEvidenceHandle $pCache[0] 'fresh'
            if ($pendingCB -ne $execCB) { $reasons += 'pending-cache CB != last executed branch CB' }
            if ($freshCB -eq $pendingCB) { $reasons += 'pending-cache fresh == pending' }
        }
        # pending-owned: facade destroyed, lease held both handles
        $wantPartial = if ($CaseName -eq 'owned-compare') { 0 } else { 1 }
        $pOwned = @($Lines | Where-Object { $_ -match ("^pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 partialDestroyed=$wantPartial$") })
        if ($pOwned.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pending-owned:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pending-owned line' }
        # pipeline-owned: engine-held pair (failure: variant NULL + failed=1)
        if ($CaseName -eq 'owned-compare') {
            $po = @($Lines | Where-Object { $_ -match ("^pipeline-owned: classic=$H dynamic=$H failed=0$") })
            if ($po.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-owned:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-owned line' }
            else {
                $c1 = Get-DynamicEvidenceHandle $po[0] 'classic'
                $d1 = Get-DynamicEvidenceHandle $po[0] 'dynamic'
                if ($c1 -eq $d1) { $reasons += 'owned classic/dynamic handles identical' }
                if ($pipelineClassic.Count -eq 1 -and
                    (Get-DynamicEvidenceHandle $pipelineClassic[0] 'handle') -ne $c1) { $reasons += 'owned classic handle != main classic create' }
                if ($pv.Count -eq 1 -and
                    (Get-DynamicEvidenceHandle $pv[0] 'handle') -ne $d1) { $reasons += 'owned dynamic handle != real Dynamic create' }
            }
        } else {
            $po = @($Lines | Where-Object { $_ -match ("^pipeline-owned: classic=$H dynamic=NULL failed=1 partialDestroyed=1$") })
            if ($po.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-owned:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-owned failure line' }
            elseif ($pipelineClassic.Count -eq 1 -and
                (Get-DynamicEvidenceHandle $po[0] 'classic') -ne
                (Get-DynamicEvidenceHandle $pipelineClassic[0] 'handle')) { $reasons += 'failure held Classic != real main create' }
            # injected failure record: driver really created, we returned error
            $pf = @($Lines | Where-Object { $_ -match ("^pipeline-failure: driverResult=0 returned=-3 partial=$H partialDestroyed=1 rpFallback=1$") })
            if ($pf.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-failure:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-failure line' }
            # eligibility negative: real Classic create, no variant attempted
            $pe = @($Lines | Where-Object { $_ -match ("^pipeline-eligibility: discardRp variantCreates=0 variant=NULL result=0 handle=$H candidate=0 policy=ON$") })
            if ($pe.Count -ne 1 -or @($Lines | Where-Object { $_ -match '^pipeline-eligibility:' }).Count -ne 1) { $reasons += 'invalid/missing/extra pipeline-eligibility line' }
            elseif ($po.Count -eq 1 -and $pf.Count -eq 1) {
                $classicH = Get-DynamicEvidenceHandle $po[0] 'classic'
                $partialH = Get-DynamicEvidenceHandle $pf[0] 'partial'
                $eligH = Get-DynamicEvidenceHandle $pe[0] 'handle'
                if ($partialH -eq $classicH) { $reasons += 'partial handle == classic handle (must have coexisted)' }
                # Classic and eligibility coexist. The partial pipeline was
                # already destroyed, so its value need not differ from eligibility.
                if ($eligH -eq $classicH) { $reasons += 'eligibility handle == classic handle' }
            }
            # a failed Dynamic CREATE attempt is NOT dynamic rendering: the
            # actual-zero phase-dynamic + copies=1 checks above already cover it
        }
    }

    # Evidence for another case cannot supplement a clean footer in this case.
    $forbidden = @()
    if (-not $isOwned) { $forbidden += 'pipeline-owned','pipeline-failure','pipeline-eligibility','pending-cache','pending-owned','retirement' }
    elseif ($CaseName -eq 'owned-compare') { $forbidden += 'pipeline-failure','pipeline-eligibility' }
    if (-not $isDraw) { $forbidden += 'pipeline-classic','pipeline-dynamic','pipeline-signature','pipeline-creates','pipeline-baseline','phase-draw-classic','phase-draw-dynamic' }
    elseif ($CaseName -ne 'draw-classic') { $forbidden += 'pipeline-baseline' }
    if ($CaseName -in @('draw-classic','owned-failure')) { $forbidden += 'pipeline-signature' }
    foreach ($prefix in $forbidden) {
        if (@($Lines | Where-Object { $_ -match ('^' + [regex]::Escape($prefix) + ':') }).Count -ne 0) { $reasons += "unexpected $prefix evidence in $CaseName" }
    }

    $diffCount = @($Lines | Where-Object { $_ -match 'classicVsDynamic diff bytes=' }).Count
    if ($diffCount -ne $(if ($isCompare) {1} else {0})) { $reasons += 'extra/missing diff lines' }
    # stage: copies exact per case; submits positive with SUCCESS == calls
    $wantCopies = if ($CaseName -in @('classic','draw-classic','owned-failure')) { 1 } else { 2 }
    $stage = @($Lines | Where-Object { $_ -match '^stage:' })
    if ($stage.Count -ne 1) { $reasons += "stage line count=$($stage.Count) (expected 1)" }
    elseif ($stage[0] -notmatch ("^stage: copies=$wantCopies submits=$wantCopies/$wantCopies overflow=0$")) {
        $reasons += "stage mismatch: $($stage[0])"
    }
    # live/post validation evidence: exactly one clean line
    $val = @($Lines | Where-Object { $_ -match '^validation: liveErrors=0 postErrors=0$' })
    $anyVal = @($Lines | Where-Object { $_ -match '^validation: liveErrors=' })
    if ($val.Count -ne 1 -or $anyVal.Count -ne 1) { $reasons += "validation evidence count=$($val.Count) (expected 1 clean); saw: $($anyVal -join ' | ')" }
    # leak line exactly 0
    $leaks = @($Lines | Where-Object { $_ -match 'Total device memory leaked:' })
    if ($leaks.Count -ne 1) { $reasons += "leak line count=$($leaks.Count)" }
    elseif ($leaks[0] -notmatch 'Total device memory leaked: 0 bytes\.$') { $reasons += "non-zero leak: $($leaks[0])" }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons }
}

function Invoke-SelfTest {
    $base = @(
        'validation + best practices + sync: ENABLED',
        'runtime: dynamicRendering=0 capsEnabled=1 effective=1.3',
        'classic: size=17x11 totalBytes=748 mismatches=0',
        'phase-classic: rp=1/1 cb=00000080012AA8D0 chain=1',
        'phase-dynamic: calls=0/0 (actual counters)',
        'stage: copies=1 submits=1/1 overflow=0',
        'validation: liveErrors=0 postErrors=0',
        'Total device memory leaked: 0 bytes.',
        'classic completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
    )
    $fixtures = @(
        @{ Name='valid-classic';            Lines=$base; Exit=0; Expect=$true },
        @{ Name='lead-null-cb';             Lines=($base -replace 'cb=00000080012AA8D0','cb=0000000000000000'); Exit=0; Expect=$false },
        @{ Name='lead-extra-bad-phase';     Lines=($base + 'phase-classic: rp=0/0 cb=00000080012AA8D0 chain=0'); Exit=0; Expect=$false },
        @{ Name='lead-extra-bad-validation'; Lines=($base + 'validation: liveErrors=9 postErrors=0'); Exit=0; Expect=$false },
        @{ Name='lead-extra-bad-stage';     Lines=($base + 'stage: copies=1 submits=0/0 overflow=1 junk'); Exit=0; Expect=$false },
        @{ Name='lead-wrong-submit-count';  Lines=($base -replace 'submits=1/1','submits=2/2'); Exit=0; Expect=$false },
        @{ Name='lead-invalid-capability';  Lines=($base -replace 'capsEnabled=1','capsEnabled=2'); Exit=0; Expect=$false },
        @{ Name='missing-footer';           Lines=$base[0..7]; Exit=0; Expect=$false },
        @{ Name='duplicate-footer';         Lines=($base + $base[-1]); Exit=0; Expect=$false },
        @{ Name='footer-under-other-name';  Lines=($base + 'other completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'); Exit=0; Expect=$false },
        @{ Name='footer-bad-posterrors';    Lines=($base -replace 'postErrors=0 mismatches','postErrors=2 mismatches'); Exit=0; Expect=$false },
        @{ Name='bytes-nonzero-mismatch';   Lines=($base -replace 'classic: size=17x11 totalBytes=748 mismatches=0','classic: size=17x11 totalBytes=748 mismatches=4'); Exit=0; Expect=$false },
        @{ Name='bytes-missing';            Lines=($base | Where-Object { $_ -notmatch 'size=17x11' }); Exit=0; Expect=$false },
        @{ Name='lead-zero-submits';        Lines=($base -replace 'submits=1/1','submits=0/1'); Exit=0; Expect=$false },
        @{ Name='lead-stage-trailing-junk'; Lines=($base -replace 'overflow=0$','overflow=0 extra'); Exit=0; Expect=$false },
        @{ Name='failed-submit';            Lines=($base -replace 'submits=1/1','submits=1/2'); Exit=0; Expect=$false },
        @{ Name='overflow-set';             Lines=($base -replace 'overflow=0','overflow=1'); Exit=0; Expect=$false },
        @{ Name='duplicate-phase';          Lines=($base + $base[3]); Exit=0; Expect=$false },
        @{ Name='missing-phase-classic';    Lines=($base | Where-Object { $_ -notmatch '^phase-classic:' }); Exit=0; Expect=$false },
        @{ Name='vuid';                     Lines=($base + 'VUID-fixture'); Exit=0; Expect=$false },
        @{ Name='crash';                    Lines=$base; Exit=-1073741819; Expect=$false },
        @{ Name='fail-line';                Lines=($base + 'FAIL: fixture'); Exit=0; Expect=$false },
        @{ Name='no-validation-enabled';    Lines=($base[1..8]); Exit=0; Expect=$false },
        @{ Name='runtime-on';               Lines=($base -replace 'dynamicRendering=0','dynamicRendering=1'); Exit=0; Expect=$false },
        @{ Name='runtime-missing';          Lines=($base | Where-Object { $_ -notmatch '^runtime:' }); Exit=0; Expect=$false },
        @{ Name='lead-nonzero-validation';  Lines=($base -replace 'liveErrors=0 postErrors=0$','liveErrors=41 postErrors=0'); Exit=0; Expect=$false },
        @{ Name='lead-diff-wrong-count';    Case='compare'; Lines=@('validation + best practices + sync: ENABLED','runtime: dynamicRendering=0 capsEnabled=1 effective=1.3','compare: size=17x11 totalBytes=748 mismatches=0','compare: classicVsDynamic diff bytes=0/7489','phase-classic: rp=1/1 cb=00000080012AA8D0 chain=1','phase-dynamic: calls=1/1 cb=00000080012BB9E0 chain=1','stage: copies=2 submits=2/2 overflow=0','validation: liveErrors=0 postErrors=0','Total device memory leaked: 0 bytes.','compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'); Exit=0; Expect=$false },
        @{ Name='valid-compare';            Case='compare'; Lines=@('validation + best practices + sync: ENABLED','runtime: dynamicRendering=0 capsEnabled=1 effective=1.3','compare: size=17x11 totalBytes=748 mismatches=0','compare: classicVsDynamic diff bytes=0/748','phase-classic: rp=1/1 cb=00000080012AA8D0 chain=1','phase-dynamic: calls=1/1 cb=00000080012BB9E0 chain=1','stage: copies=2 submits=2/2 overflow=0','validation: liveErrors=0 postErrors=0','Total device memory leaked: 0 bytes.','compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'); Exit=0; Expect=$true },
        @{ Name='compare-missing-dynphase'; Case='compare'; Lines=@('validation + best practices + sync: ENABLED','runtime: dynamicRendering=0 capsEnabled=1 effective=1.3','compare: size=17x11 totalBytes=748 mismatches=0','compare: classicVsDynamic diff bytes=0/748','phase-classic: rp=1/1 cb=00000080012AA8D0 chain=1','stage: copies=2 submits=2/2 overflow=0','validation: liveErrors=0 postErrors=0','Total device memory leaked: 0 bytes.','compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'); Exit=0; Expect=$false },
        @{ Name='classic-has-dynamic-call'; Lines=($base -replace 'calls=0/0 \(actual counters\)','calls=1/1 cb=00000080012BB9E0 chain=1'); Exit=0; Expect=$false }
    )
    $dcBase = @(
        'validation + best practices + sync: ENABLED'
        'runtime: dynamicRendering=0 capsEnabled=1 effective=1.3'
        'draw-classic: size=17x11 totalBytes=748 mismatches=0'
        'pipeline-classic: result=0 handle=00001A000000001A subpass=0'
        'phase-classic: rp=1/1 cb=00000080015018D0 chain=1'
        'phase-dynamic: calls=0/0 (actual counters)'
        'phase-draw-classic: bind=00001A000000001A draw=1 cb=00000080015018D0 chain=1'
        'stage: copies=1 submits=1/1 overflow=0'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'draw-classic completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-baseline: policy=OFF variant=NULL creates=0'
        'pipeline-creates: main=1 dynamic=0 eligibility=0 unknown=0 diagnostic=0'
    )
    $dcmpBase = @(
        'validation + best practices + sync: ENABLED'
        'runtime: dynamicRendering=0 capsEnabled=1 effective=1.3'
        'draw-compare: size=17x11 totalBytes=748 mismatches=0'
        'compare: classicVsDynamic diff bytes=0/748'
        'pipeline-classic: result=0 handle=00001D000000001D subpass=0'
        'pipeline-dynamic: result=0 handle=00001E000000001E colors=1 samples=1'
        'pipeline-signature: color=37 depth=0 stencil=0 samples=1 viewMask=0 rpNull=1 subpass=0'
        'phase-classic: rp=1/1 cb=000000DB99E318D0 chain=1'
        'phase-dynamic: calls=1/1 cb=000000DB99E2E0D0 chain=1'
        'phase-draw-classic: bind=00001D000000001D draw=1 cb=000000DB99E318D0 chain=1'
        'phase-draw-dynamic: bind=00001E000000001E draw=1 cb=000000DB99E2E0D0 chain=1'
        'stage: copies=2 submits=2/2 overflow=0'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'draw-compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-creates: main=1 dynamic=0 eligibility=0 unknown=0 diagnostic=1'
    )
    $fixtures += @(
        @{ Name='valid-draw-classic';        Case='draw-classic';  Lines=$dcBase; Exit=0; Expect=$true }
        @{ Name='valid-draw-compare';        Case='draw-compare';  Lines=$dcmpBase; Exit=0; Expect=$true }
        @{ Name='draw-missing-draw-phase';   Case='draw-classic';  Lines=($dcBase | Where-Object { $_ -notmatch '^phase-draw-classic:' }); Exit=0; Expect=$false }
        @{ Name='draw-wrong-chain';          Case='draw-classic';  Lines=($dcBase -replace 'chain=1$','chain=0'); Exit=0; Expect=$false }
        @{ Name='draw-failed-pipeline';      Case='draw-classic';  Lines=($dcBase -replace 'pipeline-classic: result=0','pipeline-classic: result=-3'); Exit=0; Expect=$false }
        @{ Name='draw-wrong-bind-handle';    Case='draw-classic';  Lines=($dcBase -replace 'phase-draw-classic: bind=00001A000000001A','phase-draw-classic: bind=00000DEAD0000BEAF'); Exit=0; Expect=$false }
        @{ Name='draw-mismatch-samples';     Case='draw-compare';  Lines=($dcmpBase -replace 'colors=1 samples=1','colors=1 samples=4'); Exit=0; Expect=$false }
        @{ Name='draw-missing-variant';      Case='draw-compare';  Lines=($dcmpBase | Where-Object { $_ -notmatch '^pipeline-dynamic:' }); Exit=0; Expect=$false }
        @{ Name='draw-missing-dyn-draw';     Case='draw-compare';  Lines=($dcmpBase | Where-Object { $_ -notmatch '^phase-draw-dynamic:' }); Exit=0; Expect=$false }
        @{ Name='draw-nonzero-pixels';       Case='draw-compare';  Lines=($dcmpBase -replace 'draw-compare: size=17x11 totalBytes=748 mismatches=0','draw-compare: size=17x11 totalBytes=748 mismatches=748'); Exit=0; Expect=$false }
        @{ Name='draw-duplicate-evidence';   Case='draw-compare';  Lines=($dcmpBase + $dcmpBase[9]); Exit=0; Expect=$false }
        @{ Name='draw-classic-has-variant';  Case='draw-classic';  Lines=($dcBase + 'pipeline-dynamic: result=0 handle=00001E000000001E colors=1 samples=1'); Exit=0; Expect=$false }
        @{ Name='lead-dynamic-bind-mismatch'; Case='draw-compare'; Lines=($dcmpBase -replace 'phase-draw-dynamic: bind=00001E000000001E','phase-draw-dynamic: bind=0000000012345678'); Exit=0; Expect=$false }
        @{ Name='lead-extra-bad-draw'; Case='draw-compare'; Lines=($dcmpBase + 'phase-draw-dynamic: bind=0000000000000000 draw=0 cb=0000000000000000 chain=0'); Exit=0; Expect=$false }
        @{ Name='lead-disabled-dynamic-caps'; Case='draw-compare'; Lines=($dcmpBase -replace 'capsEnabled=1','capsEnabled=0'); Exit=0; Expect=$false }
        @{ Name='lead-null-dynamic-pipeline'; Case='draw-compare'; Lines=($dcmpBase -replace 'pipeline-dynamic: result=0 handle=00001E000000001E','pipeline-dynamic: result=0 handle=0000000000000000'); Exit=0; Expect=$false }
        @{ Name='lead-draw-cb-mismatch'; Case='draw-compare'; Lines=($dcmpBase -replace 'phase-draw-dynamic: bind=00001E000000001E draw=1 cb=000000DB99E2E0D0','phase-draw-dynamic: bind=00001E000000001E draw=1 cb=0000000001234567'); Exit=0; Expect=$false }
        @{ Name='lead-signature-format-mismatch'; Case='draw-compare'; Lines=($dcmpBase -replace 'color=37','color=44'); Exit=0; Expect=$false }
        @{ Name='lead-signature-missing'; Case='draw-compare'; Lines=($dcmpBase | Where-Object { $_ -notmatch '^pipeline-signature:' }); Exit=0; Expect=$false }
    )
    # Standalone fixtures derived from the real current-source owned traces.
    # They deliberately have no dependency on ignored artifacts or a GPU.
    $ownedCompare = @(
        'validation + best practices + sync: ENABLED'
        'runtime: dynamicRendering=0 capsEnabled=1 effective=1.3'
        'pipeline-classic: result=0 handle=00001D000000001D subpass=0'
        'pipeline-owned: classic=00001D000000001D dynamic=00001E000000001E failed=0'
        'pipeline-dynamic: result=0 handle=00001E000000001E colors=1 samples=1'
        'pipeline-signature: color=37 depth=0 stencil=0 samples=1 viewMask=0 rpNull=1 subpass=0'
        'pending-cache: pending=00000080014AE0D0 fresh=00000080014B50D0 distinct=1'
        'pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 partialDestroyed=0'
        'compare: classicVsDynamic diff bytes=0/748'
        'owned-compare: size=17x11 totalBytes=748 mismatches=0'
        'phase-classic: rp=1/1 cb=00000080014B18D0 chain=1'
        'phase-dynamic: calls=1/1 cb=00000080014AE0D0 chain=1'
        'phase-draw-classic: bind=00001D000000001D draw=1 cb=00000080014B18D0 chain=1'
        'phase-draw-dynamic: bind=00001E000000001E draw=1 cb=00000080014AE0D0 chain=1'
        'pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=0 diagnostic=0'
        'stage: copies=2 submits=2/2 overflow=0'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'retirement: classicDestroyed=1 dynDestroyed=1 partialDestroyed=0 eligibilityDestroyed=0'
        'owned-compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-keys: classic=EXACT dynamic=EXACT ownership=OK'
    )
    $ownedFailure = @(
        'validation + best practices + sync: ENABLED'
        'runtime: dynamicRendering=0 capsEnabled=1 effective=1.3'
        'pipeline-classic: result=0 handle=00001A000000001A subpass=0'
        'pipeline-owned: classic=00001A000000001A dynamic=NULL failed=1 partialDestroyed=1'
        'pipeline-failure: driverResult=0 returned=-3 partial=00001B000000001B partialDestroyed=1 rpFallback=1'
        'pipeline-eligibility: discardRp variantCreates=0 variant=NULL result=0 handle=00001D000000001D candidate=0 policy=ON'
        'pending-cache: pending=000000C2225818D0 fresh=000000C22257E0D0 distinct=1'
        'pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 partialDestroyed=1'
        'owned-failure: size=17x11 totalBytes=748 mismatches=0'
        'phase-classic: rp=1/1 cb=000000C2225818D0 chain=1'
        'phase-dynamic: calls=0/0 (actual counters)'
        'phase-draw-classic: bind=00001A000000001A draw=1 cb=000000C2225818D0 chain=1'
        'pipeline-creates: main=1 dynamic=1 eligibility=1 unknown=0 diagnostic=0'
        'stage: copies=1 submits=1/1 overflow=0'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'retirement: classicDestroyed=1 dynDestroyed=0 partialDestroyed=1 eligibilityDestroyed=1'
        'owned-failure completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-keys: classic=EXACT dynamic=EMPTY ownership=OK'
    )
    $fixtures += @(
        @{ Name='valid-owned-compare'; Case='owned-compare'; Lines=$ownedCompare; Exit=0; Expect=$true }
        @{ Name='valid-owned-failure'; Case='owned-failure'; Lines=$ownedFailure; Exit=0; Expect=$true }
        @{ Name='owned-missing-pending-cache'; Case='owned-compare'; Lines=($ownedCompare | Where-Object { $_ -notmatch '^pending-cache:' }); Exit=0; Expect=$false }
        @{ Name='owned-duplicate-pending-cache'; Case='owned-compare'; Lines=($ownedCompare + $ownedCompare[6]); Exit=0; Expect=$false }
        @{ Name='owned-fresh-equals-pending'; Case='owned-compare'; Lines=($ownedCompare -replace 'fresh=00000080014B50D0','fresh=00000080014AE0D0'); Exit=0; Expect=$false }
        @{ Name='owned-fresh-alias-spelling'; Case='owned-compare'; Lines=($ownedCompare -replace 'fresh=00000080014B50D0','fresh=0x80014ae0d0'); Exit=0; Expect=$false }
        @{ Name='owned-wrong-pending-cb'; Case='owned-compare'; Lines=($ownedCompare -replace 'pending=00000080014AE0D0','pending=0000000012345678'); Exit=0; Expect=$false }
        @{ Name='owned-early-destruction'; Case='owned-compare'; Lines=($ownedCompare -replace 'facade=destroyed classicDestroyed=0','facade=destroyed classicDestroyed=1'); Exit=0; Expect=$false }
        @{ Name='owned-pending-not-destroyed-facade'; Case='owned-compare'; Lines=($ownedCompare -replace 'facade=destroyed','facade=alive'); Exit=0; Expect=$false }
        @{ Name='owned-missing-retirement'; Case='owned-compare'; Lines=($ownedCompare | Where-Object { $_ -notmatch '^retirement:' }); Exit=0; Expect=$false }
        @{ Name='owned-duplicate-retirement'; Case='owned-compare'; Lines=($ownedCompare + $ownedCompare[18]); Exit=0; Expect=$false }
        @{ Name='owned-retirement-not-complete'; Case='owned-compare'; Lines=($ownedCompare -replace '^retirement: classicDestroyed=1 dynDestroyed=1','retirement: classicDestroyed=1 dynDestroyed=0'); Exit=0; Expect=$false }
        @{ Name='owned-retirement-trailing-junk'; Case='owned-compare'; Lines=($ownedCompare -replace '^retirement: (.*)$','retirement: $1 junk'); Exit=0; Expect=$false }
        @{ Name='owned-held-dynamic-mismatch'; Case='owned-compare'; Lines=($ownedCompare -replace '^pipeline-owned: classic=00001D000000001D dynamic=00001E000000001E','pipeline-owned: classic=00001D000000001D dynamic=0000000012345678'); Exit=0; Expect=$false }
        @{ Name='owned-held-classic-mismatch'; Case='owned-compare'; Lines=($ownedCompare -replace '^pipeline-owned: classic=00001D000000001D','pipeline-owned: classic=0000000012345678'); Exit=0; Expect=$false }
        @{ Name='owned-identical-held-handles'; Case='owned-compare'; Lines=($ownedCompare -replace '^pipeline-owned: classic=00001D000000001D dynamic=00001E000000001E','pipeline-owned: classic=00001D000000001D dynamic=00001D000000001D'); Exit=0; Expect=$false }
        @{ Name='owned-extra-unknown-create'; Case='owned-compare'; Lines=($ownedCompare -replace '^pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=0','pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=1'); Exit=0; Expect=$false }
        @{ Name='owned-extra-diagnostic-create'; Case='owned-compare'; Lines=($ownedCompare -replace '^pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=0 diagnostic=0','pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=0 diagnostic=1'); Exit=0; Expect=$false }
        @{ Name='owned-extra-failure-line'; Case='owned-compare'; Lines=($ownedCompare + $ownedFailure[4]); Exit=0; Expect=$false }
        @{ Name='owned-extra-eligibility-line'; Case='owned-compare'; Lines=($ownedCompare + $ownedFailure[5]); Exit=0; Expect=$false }
        @{ Name='owned-failure-held-classic-mismatch'; Case='owned-failure'; Lines=($ownedFailure -replace '^pipeline-owned: classic=00001A000000001A','pipeline-owned: classic=0000000012345678'); Exit=0; Expect=$false }
        @{ Name='owned-failure-partial-not-destroyed'; Case='owned-failure'; Lines=($ownedFailure -replace '^pipeline-failure: (.*) partialDestroyed=1','pipeline-failure: $1 partialDestroyed=0'); Exit=0; Expect=$false }
        @{ Name='owned-failure-partial-double-destroyed'; Case='owned-failure'; Lines=($ownedFailure -replace '^retirement: classicDestroyed=1 dynDestroyed=0 partialDestroyed=1','retirement: classicDestroyed=1 dynDestroyed=0 partialDestroyed=2'); Exit=0; Expect=$false }
        @{ Name='owned-failure-driver-not-success'; Case='owned-failure'; Lines=($ownedFailure -replace 'driverResult=0','driverResult=-3'); Exit=0; Expect=$false }
        @{ Name='owned-failure-not-injected'; Case='owned-failure'; Lines=($ownedFailure -replace 'returned=-3','returned=0'); Exit=0; Expect=$false }
        @{ Name='owned-failure-no-rp-fallback'; Case='owned-failure'; Lines=($ownedFailure -replace 'rpFallback=1','rpFallback=0'); Exit=0; Expect=$false }
        @{ Name='owned-failure-eligibility-has-variant'; Case='owned-failure'; Lines=($ownedFailure -replace 'discardRp variantCreates=0','discardRp variantCreates=1'); Exit=0; Expect=$false }
        @{ Name='owned-failure-eligibility-null-handle'; Case='owned-failure'; Lines=($ownedFailure -replace 'result=0 handle=00001D000000001D candidate','result=0 handle=0000000000000000 candidate'); Exit=0; Expect=$false }
        @{ Name='owned-failure-partial-equals-classic'; Case='owned-failure'; Lines=($ownedFailure -replace 'partial=00001B000000001B','partial=00001A000000001A'); Exit=0; Expect=$false }
        @{ Name='owned-failure-caps-disabled'; Case='owned-failure'; Lines=($ownedFailure -replace 'capsEnabled=1','capsEnabled=0'); Exit=0; Expect=$false }
        @{ Name='owned-failure-live-errors'; Case='owned-failure'; Lines=($ownedFailure -replace '^validation: liveErrors=0','validation: liveErrors=1'); Exit=0; Expect=$false }
        @{ Name='owned-failure-crash'; Case='owned-failure'; Lines=$ownedFailure; Exit=-1073741819; Expect=$false }
        @{ Name='classic-extra-owned-proof'; Case='classic'; Lines=($base + $ownedCompare[3]); Exit=0; Expect=$false }
    )
    # M6-A2b formal engine-path fixtures (self-contained literals from the
    # real Lead-verified logs; every fixture passes -EnginePath explicitly)
    $epCommon = @(
        'validation + best practices + sync: ENABLED'
        'runtime: dynamicRendering=1 capsEnabled=1 effective=1.3'
        'engine-path: gfxRendering=1 gfxDraw=1 runtimeDynamic=1 restored=1'
    )
    $epCompare = @(
        'owned-compare: size=17x11 totalBytes=748 mismatches=0'
        'compare: classicVsDynamic diff bytes=0/748'
        'pipeline-classic: result=0 handle=00001E000000001E subpass=0'
        'pipeline-owned: classic=00001E000000001E dynamic=00001F000000001F failed=0'
        'pipeline-dynamic: result=0 handle=00001F000000001F colors=1 samples=1'
        'pipeline-signature: color=37 depth=0 stencil=0 samples=1 viewMask=0 rpNull=1 subpass=0'
        'pending-cache: pending=000000DBA02CE0D0 fresh=000000DBA02D50D0 distinct=1'
        'pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 partialDestroyed=0'
        'phase-classic: rp=1/1 cb=000000DBA02D18D0 chain=1'
        'phase-dynamic: calls=1/1 cb=000000DBA02CE0D0 chain=1'
        'stage: copies=2 submits=2/2 overflow=0'
        'phase-draw-classic: bind=00001E000000001E draw=1 cb=000000DBA02D18D0 chain=1'
        'phase-draw-dynamic: bind=00001F000000001F draw=1 cb=000000DBA02CE0D0 chain=1'
        'pipeline-creates: main=1 dynamic=1 eligibility=0 unknown=0 diagnostic=0'
        'engine-calls: classic=1/1 dynamic=1/1 draws=2 submits=2/2'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'retirement: classicDestroyed=1 dynDestroyed=1 partialDestroyed=0 eligibilityDestroyed=0'
        'owned-compare completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-keys: classic=EXACT dynamic=EXACT ownership=OK'
    )
    $epFailure = @(
        'owned-failure: size=17x11 totalBytes=748 mismatches=0'
        'pipeline-classic: result=0 handle=00001A000000001A subpass=0'
        'pipeline-owned: classic=00001A000000001A dynamic=NULL failed=1 partialDestroyed=1'
        'pipeline-failure: driverResult=0 returned=-3 partial=00001B000000001B partialDestroyed=1 rpFallback=1'
        'pipeline-eligibility: discardRp variantCreates=0 variant=NULL result=0 handle=00001D000000001D candidate=0 policy=ON'
        'pending-cache: pending=000000C2225818D0 fresh=000000C22257E0D0 distinct=1'
        'pending-owned: facade=destroyed classicDestroyed=0 variantDestroyed=0 partialDestroyed=1'
        'phase-classic: rp=1/1 cb=000000C2225818D0 chain=1'
        'phase-dynamic: calls=0/0 (actual counters)'
        'stage: copies=1 submits=1/1 overflow=0'
        'phase-draw-classic: bind=00001A000000001A draw=1 cb=000000C2225818D0 chain=1'
        'pipeline-creates: main=1 dynamic=1 eligibility=1 unknown=0 diagnostic=0'
        'engine-calls: classic=1/1 dynamic=0/0 draws=1 submits=1/1'
        'validation: liveErrors=0 postErrors=0'
        'Total device memory leaked: 0 bytes.'
        'retirement: classicDestroyed=1 dynDestroyed=0 partialDestroyed=1 eligibilityDestroyed=1'
        'owned-failure completed: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN'
        'pipeline-keys: classic=EXACT dynamic=EMPTY ownership=OK'
    )
    $epCompareBase = $epCommon + $epCompare
    $epFailureBase = $epCommon + $epFailure
    $fixtures += @(
        @{ Name='ep-valid-compare';          Case='owned-compare'; EnginePath=$true; Lines=$epCompareBase; Exit=0; Expect=$true }
        @{ Name='ep-valid-failure';          Case='owned-failure'; EnginePath=$true; Lines=$epFailureBase; Exit=0; Expect=$true }
        @{ Name='ep-missing-engine-path';    Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase | Where-Object { $_ -notmatch '^engine-path:' }); Exit=0; Expect=$false }
        @{ Name='ep-duplicate-engine-path';  Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + $epCommon[2]); Exit=0; Expect=$false }
        @{ Name='ep-gfxdraw-zero';           Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'gfxDraw=1','gfxDraw=0'); Exit=0; Expect=$false }
        @{ Name='ep-restored-zero';          Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'restored=1','restored=0'); Exit=0; Expect=$false }
        @{ Name='ep-runtime-off';            Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'dynamicRendering=1','dynamicRendering=0'); Exit=0; Expect=$false }
        @{ Name='ep-missing-engine-calls';   Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase | Where-Object { $_ -notmatch '^engine-calls:' }); Exit=0; Expect=$false }
        @{ Name='ep-duplicate-engine-calls'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + 'engine-calls: classic=1/1 dynamic=1/1 draws=2 submits=2/2'); Exit=0; Expect=$false }
        @{ Name='ep-draws-wrong';            Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'draws=2','draws=3'); Exit=0; Expect=$false }
        @{ Name='ep-submits-wrong';          Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'submits=2/2','submits=1/2'); Exit=0; Expect=$false }
        @{ Name='ep-failure-dynamic-end';    Case='owned-failure'; EnginePath=$true; Lines=($epFailureBase -replace 'dynamic=0/0','dynamic=1/0'); Exit=0; Expect=$false }
        @{ Name='ep-ntstatus';               Case='owned-compare'; EnginePath=$true; Lines=$epCompareBase; Exit=-1073741819; Expect=$false }
        @{ Name='ep-diagnostic-carries-ep';  Case='owned-compare'; EnginePath=$false; Lines=($epCompareBase -replace 'dynamicRendering=1','dynamicRendering=0'); Exit=0; Expect=$false }
        @{ Name='ep-failure-calls-mismatch'; Case='owned-failure'; EnginePath=$true; Lines=($epFailureBase -replace 'draws=1 submits=1/1','draws=2 submits=2/2'); Exit=0; Expect=$false }
    )
    # Lead review: malformed extra evidence must not hide beside a valid line.
    $fixtures += @(
        @{ Name='ep-extra-path-without-space'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + 'engine-path:malformed'); Exit=0; Expect=$false }
        @{ Name='ep-extra-calls-without-space'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + 'engine-calls:malformed'); Exit=0; Expect=$false }
        @{ Name='diagnostic-path-without-space'; Case='owned-compare'; EnginePath=$false; Lines=($ownedCompare + 'engine-path:malformed'); Exit=0; Expect=$false }
        @{ Name='diagnostic-calls-without-space'; Case='owned-compare'; EnginePath=$false; Lines=($ownedCompare + 'engine-calls:malformed'); Exit=0; Expect=$false }
        @{ Name='ep-non-owned-judge'; Case='classic'; EnginePath=$true; Lines=(($base -replace 'dynamicRendering=0','dynamicRendering=1') + $epCommon[2] + 'engine-calls: classic=1/1 dynamic=0/0 draws=1 submits=1/1'); Exit=0; Expect=$false }
    )
    $fixtures += @(
        @{ Name='keys-missing'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase | Where-Object { $_ -notmatch '^pipeline-keys' }); Exit=0; Expect=$false },
        @{ Name='keys-duplicate'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + 'pipeline-keys: classic=EXACT dynamic=EXACT ownership=OK'); Exit=0; Expect=$false },
        @{ Name='keys-malformed-extra'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase + 'pipeline-keys_bad'); Exit=0; Expect=$false },
        @{ Name='keys-trailing-junk'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace '^pipeline-keys:(.*)$','pipeline-keys:$1 junk'); Exit=0; Expect=$false },
        @{ Name='keys-classic-bad'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace '^pipeline-keys: classic=EXACT','pipeline-keys: classic=BAD'); Exit=0; Expect=$false },
        @{ Name='keys-dynamic-bad'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'dynamic=EXACT','dynamic=BAD'); Exit=0; Expect=$false },
        @{ Name='keys-ownership-bad'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace '^pipeline-keys:(.*)ownership=OK$','pipeline-keys:$1ownership=BAD'); Exit=0; Expect=$false },
        @{ Name='keys-fallback-published'; Case='owned-failure'; EnginePath=$true; Lines=($epFailureBase -replace 'dynamic=EMPTY','dynamic=EXACT'); Exit=0; Expect=$false },
        @{ Name='keys-success-not-published'; Case='owned-compare'; EnginePath=$true; Lines=($epCompareBase -replace 'dynamic=EXACT','dynamic=EMPTY'); Exit=0; Expect=$false },
        @{ Name='keys-non-owned'; Case='draw-classic'; Lines=($dcBase + 'pipeline-keys: classic=EXACT dynamic=EMPTY ownership=OK'); Exit=0; Expect=$false }
    )
    $good = $true
    $executed = 0
    foreach ($f in $fixtures) {
        ++$executed
        $case = if ($f.Case) { $f.Case } else { 'classic' }
        $r = Test-DynamicRenderingLog -Lines @($f.Lines) -ExitCode $f.Exit -CaseName $case -EnginePath:([bool]$f.EnginePath)
        $ok = $r.Pass -eq $f.Expect
        Write-Host "SelfTest $($f.Name): $ok"
        if (-not $ok) { $good = $false; $r.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    Write-Host ("fixtures: {0}; executed: {1}" -f $fixtures.Count,$executed)
    return $good
}

if ($SelfTest) {
    $ok = Invoke-SelfTest
    Write-Host ("SelfTest exit: {0}" -f $(if ($ok) {0} else {1}))
    exit $(if ($ok) {0} else {1})
}

$all = @('classic', 'compare', 'draw-classic', 'draw-compare', 'owned-compare', 'owned-failure')
$epValid = @('owned-compare', 'owned-failure')
if ($PSBoundParameters.ContainsKey('Cases')) {
    $Cases = @($Cases | ForEach-Object { $_ -split ',' } | Select-Object -Unique)
    if ($Cases.Count -eq 0 -or @($Cases | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
        Write-Host 'FAIL: empty Cases entry'; exit 1
    }
    foreach ($c in $Cases) {
        if ($all -notcontains $c) { Write-Error "unknown case '$c'. valid: $($all -join ', ')"; exit 1 }
        if ($EnginePath -and $epValid -notcontains $c) {
            Write-Host "FAIL: -EnginePath only accepts: $($epValid -join ', ') (got '$c')" -ForegroundColor Red
            exit 1
        }
    }
} else { $Cases = $all }
if ($EnginePath -and -not $PSBoundParameters.ContainsKey('Cases')) {
    # formal engine-path mode default: only the two owned hybrid cases
    $Cases = $epValid
}

$envNames = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$oldEnv = @{}
foreach ($n in $envNames) { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
$oldCwd = (Get-Location).Path
$script:anyFail = $false
try {
    # build the CURRENT target: red build fails immediately
    Write-Host "building vulkan_dynamic_rendering_smoke..." -ForegroundColor Cyan
    Write-Host "run root: $runRoot"
    & cmake --build $buildDir --config Release --target vulkan_dynamic_rendering_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: build failed" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

    # policy isolation: legacy submit/barrier so the vkQueueSubmit hook sees
    # the real path; baseline 1.3; completion auto; no stale disables.
    # EnginePath mode turns the debug rendering path ON for the run.
    if ($EnginePath) { $env:CC_VULKAN_RENDERING = 'dynamic' }
    else { Remove-Item Env:CC_VULKAN_RENDERING -ErrorAction SilentlyContinue }
    $env:CC_VULKAN_FORCE_API = '1.3'
    $env:CC_VULKAN_COMPLETION = 'auto'
    $env:CC_VULKAN_SUBMIT = 'legacy'
    $env:CC_VULKAN_BARRIER = 'legacy'
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    $renderingPolicy = if ($EnginePath) { 'dynamic' } else { '(unset)' }
    Write-Host "policy: FORCE=1.3 COMPLETION=auto SUBMIT=legacy BARRIER=legacy RENDERING=$renderingPolicy DISABLE=(cleared)" -ForegroundColor Cyan

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
            $epArg = if ($EnginePath) { ' --engine-path' } else { '' }
            cmd /c "`"$exe`" --case $name$epArg > `"$caseDir\log.txt`" 2>&1"
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        $lines = if (Test-Path "$caseDir\log.txt") { Get-Content "$caseDir\log.txt" } else { @() }
        $r = Test-DynamicRenderingLog -Lines $lines -ExitCode $code -CaseName $name -EnginePath:($EnginePath)
        if ($r.Pass) { Write-Host "  [PASS] ${name}" -ForegroundColor Green }
        else { Write-Host "  [FAIL] ${name}: $($r.Reasons -join '; ')" -ForegroundColor Red; $script:anyFail = $true }
        $results += [pscustomobject]@{ Case = $name; Exit = $code; Pass = $r.Pass }
    }
    $results | Format-Table -AutoSize
    Write-Host ("`n=== dynamic-rendering overall: {0} ===" -f $(if ($script:anyFail) {'FAIL'} else {'PASS'}))
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
