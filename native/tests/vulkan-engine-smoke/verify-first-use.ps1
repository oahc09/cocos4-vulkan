# PERF-A2 rev3 formal first-use verification (Windows, PS5 compatible).
#
# Usage (direct invocation):
#   & .\verify-first-use.ps1                 # formal matrix, validation both, 5 repeats
#   & .\verify-first-use.ps1 -SelfTest       # judge fixtures only
#   & .\verify-first-use.ps1 -Validation on  # ON tree only
#
# Builds the CURRENT source targets first (a red build refuses stale exes):
#   ON  tree: native\build\vulkan-modernization\win64-simulator
#   OFF tree: native\build\vulkan-modernization\win64-benchmark-off

param(
    [switch]$SelfTest,
    [string]$Validation = 'both',
    [int]$Repeats = 5
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$onExe = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator\vulkan-engine-smoke\Release\vulkan_first_use_smoke.exe'
$offExe = Join-Path $repo 'native\build\vulkan-modernization\win64-benchmark-off\vulkan-engine-smoke\Release\vulkan_first_use_smoke.exe'
$runRootBase = Join-Path $repo 'native\build\vulkan-modernization\firstuse-matrix'
$selfTestDir = Join-Path $runRootBase ("_selftest-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + "-" + $PID)

$script:failedAny = $false

# --------------------------------------------------------------------------
# shared judge: log + CSV are BOTH mandatory inputs; returns
# Pass/Reasons/SavedBytes/Timings. SavedBytes/Timings are zeroed on failure.
# Anchoring rule: '^' is ALWAYS the outermost element — optional timestamp
# prefixes live inside (?:...) groups, never outside the anchor.
# Wide identification is case-insensitive (FIRST-USE/first-usevalidation/
# TOTAL DEVICE... variants are all captured); the exact output match is
# case-sensitive.
# --------------------------------------------------------------------------
function Test-FirstUseLog {
    param(
        [string[]]$Lines,
        [object]$ExitCode,
        [string]$ExpectedPhase,
        [string]$Validation,
        [object]$ExpectedInitialBytes,
        [string]$CsvPath
    )
    $result = [pscustomobject]@{ Pass = $false; Reasons = (New-Object System.Collections.Generic.List[string]); SavedBytes = [int64]0; Timings = @{} }
    function Fail([string]$m) { $result.Reasons.Add($m) | Out-Null }

    try {
        if ($null -eq $Lines -or $Lines.Count -eq 0) { Fail 'empty log' }
        if ($null -eq $ExitCode) { Fail 'exit missing' }
        elseif ($ExitCode -isnot [int64] -and $ExitCode -isnot [int]) { Fail "exit type invalid: $ExitCode" }
        elseif ([int64]$ExitCode -ne 0) { Fail "exit $ExitCode != exact 0" }

        if ($ExpectedPhase -cne 'cold' -and $ExpectedPhase -cne 'restored') { Fail "invalid ExpectedPhase '$ExpectedPhase'" }
        if ($Validation -cne 'on' -and $Validation -cne 'off') { Fail "invalid Validation '$Validation'" }

        # CSV mandatory
        if ([string]::IsNullOrEmpty($CsvPath)) { Fail 'csv path missing' }
        elseif (-not (Test-Path $CsvPath)) { Fail "csv missing: $CsvPath" }

        if ($result.Reasons.Count -eq 0) {
            $text = $Lines -join "`n"

            # ---- first-use family: case-insensitive wide identification ----
            # captures FIRST-USE/first-usevalidation/no-space variants; the
            # total must be exactly 7 (10 would mean duplicates/appends).
            $fuTotal = 0
            foreach ($line in $Lines) {
                if ($null -eq $line) { continue }
                if ($line -imatch '^first-use' -or $line -imatch '^first-use\S+') {
                    $fuTotal++
                }
            }
            if ($fuTotal -ne 7) { Fail "first-use prefixed lines $fuTotal != 7" }
            $classes = @('first-use validation:', 'first-use mode:', 'first-use cache-input:',
                'first-use evidence:', 'first-use saved:', 'first-use timing ns:')
            foreach ($c in $classes) {
                $n = @($Lines | Where-Object { $null -ne $_ -and $_ -imatch [regex]::Escape($c) }).Count
                if ($n -ne 1) { Fail "marker '$c' wide count $n != 1" }
            }
            foreach ($line in $Lines) {
                if ($null -ne $line -and $line -imatch '^first-use' -and -not ($line -cmatch '^first-use (validation|mode|cache-input|evidence|saved|timing ns|cold|restored)')) {
                    Fail "unknown/malformed first-use line: '$line'"
                }
            }

            # validation exact (case-sensitive)
            $wantVal = 'DISABLED'; if ($Validation -ceq 'on') { $wantVal = 'ENABLED' }
            if (-not (($Lines | Where-Object { $_ -ceq "first-use validation: $wantVal" }).Count -eq 1)) {
                Fail "validation line != 'first-use validation: $wantVal'"
            }

            # mode exact
            if (-not (($Lines | Where-Object { $_ -ceq "first-use mode: phase=$ExpectedPhase validation=$Validation skipDraw=0 skipSubmit=0" }).Count -eq 1)) {
                Fail 'mode line mismatch'
            }

            # API: case-insensitive wide count, then ^-anchored exact schema
            # ('^' outermost; the timestamp prefix lives inside (?:...)).
            $apiAll = @($Lines | Where-Object { $null -ne $_ -and $_ -imatch 'vulkan\s*api\b' })
            if ($apiAll.Count -ne 1) { Fail "API wide count $($apiAll.Count) != 1" }
            elseif ($apiAll[0] -cnotmatch '^(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?Vulkan API: loader=\d+\.\d+ requested=\d+\.\d+ XR=\d+\.\d+ instance=\d+\.\d+ physical=\d+\.\d+ effective=1\.3$') {
                Fail "API line malformed/effective!=1.3: '$($apiAll[0])'"
            }

            # paths: same pattern
            $pthAll = @($Lines | Where-Object { $null -ne $_ -and $_ -imatch 'vulkan\s*paths\b' })
            if ($pthAll.Count -ne 1) { Fail "paths wide count $($pthAll.Count) != 1" }
            elseif ($pthAll[0] -cnotmatch '^(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?Vulkan paths: timeline=[01] sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0$') {
                Fail "paths line malformed: '$($pthAll[0])'"
            }

            # cache-input
            $ci = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('first-use cache-input:') } | Select-Object -First 1
            if ($ExpectedPhase -ceq 'cold') {
                if ($ci -cne 'first-use cache-input: calls=1 loaded=0 initialBytes=0 payload=0') { Fail 'cache-input cold exact' }
            } else {
                if ($ci -cmatch '^first-use cache-input: calls=1 loaded=1 initialBytes=(\d{1,19}) payload=1$') {
                    $init = [int64]0
                    $digits = $Matches[1]
                    if (-not [int64]::TryParse($digits, [ref]$init) -or $init -le 0 -or $digits.StartsWith('0')) {
                        Fail 'restored initialBytes non-canonical/non-positive'
                    } else {
                        if ($null -eq $ExpectedInitialBytes) { Fail 'restored without cold link (ExpectedInitialBytes missing)' }
                        else {
                            $exp = [int64]0
                            $ok = $false
                            if ($ExpectedInitialBytes -is [int64] -or $ExpectedInitialBytes -is [int]) {
                                $exp = [int64]$ExpectedInitialBytes
                                $ok = ($exp -gt 0)
                            } elseif ($ExpectedInitialBytes -is [string]) {
                                # canonical positive ASCII digits only: no leading
                                # zero, no +/- sign, no whitespace, no overflow
                                if ($ExpectedInitialBytes -cmatch '^\d{1,19}$' -and -not $ExpectedInitialBytes.StartsWith('0') -and
                                    [int64]::TryParse($ExpectedInitialBytes, [ref]$exp)) { $ok = $true }
                            }
                            if (-not $ok) { Fail "ExpectedInitialBytes unusable/non-positive: '$ExpectedInitialBytes'" }
                            elseif ($init -ne $exp) { Fail "restored initialBytes $init != expected $exp" }
                        }
                    }
                } else { Fail "cache-input restored malformed: '$ci'" }
            }

            # evidence
            if (-not (($Lines | Where-Object { $_ -ceq 'first-use evidence: begin=1 graphics=1 draw=1 submit=1 present=1 chain=1' }).Count -eq 1)) {
                Fail 'evidence line exact'
            }

            # saved
            $sv = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('first-use saved:') } | Select-Object -First 1
            if ($sv -cmatch '^first-use saved: bytes=(\d{1,19}) validated=1 dirty=0$') {
                $b = [int64]0
                if ([int64]::TryParse($Matches[1], [ref]$b) -and $b -gt 0 -and -not $Matches[1].StartsWith('0')) {
                    $result.SavedBytes = $b
                } else { Fail 'saved bytes non-canonical' }
            } else { Fail "saved line malformed: '$sv'" }

            # timing
            $tm = $Lines | Where-Object { $null -ne $_ -and $_.StartsWith('first-use timing ns:') } | Select-Object -First 1
            $pattern = '^first-use timing ns: backend_init=(\d{1,19}) cache_config=(\d{1,19}) swapchain_create=(\d{1,19}) ' +
                'shader_compile=(\d{1,19}) pso_factory=(\d{1,19}) pso_driver=(\d{1,19}) combined_graphics=(\d{1,19}) ' +
                'frame_to_present_api=(\d{1,19}) frame_flow=(\d{1,19}) main_to_present_api=(\d{1,19})$'
            if ($tm -cmatch $pattern) {
                $names = @('backend_init', 'cache_config', 'swapchain_create', 'shader_compile', 'pso_factory',
                    'pso_driver', 'combined_graphics', 'frame_to_present_api', 'frame_flow', 'main_to_present_api')
                $vals = @{}
                $bad = $false
                for ($i = 1; $i -le 10; ++$i) {
                    $v = [int64]0
                    if (-not [int64]::TryParse($Matches[$i], [ref]$v) -or $v -lt 0 -or ($Matches[$i].Length -gt 1 -and $Matches[$i].StartsWith('0'))) {
                        Fail "timing field $($names[$i-1]) non-canonical"; $bad = $true; continue
                    }
                    $vals[$names[$i - 1]] = $v
                }
                if (-not $bad) {
                    if ($vals['pso_factory'] -lt $vals['pso_driver']) { Fail 'pso_factory < pso_driver' }
                    if ($vals['combined_graphics'] -lt $vals['shader_compile'] -or
                        ($vals['combined_graphics'] - $vals['shader_compile']) -lt $vals['pso_factory']) { Fail 'combined < shader+factory' }
                    if ($vals['frame_flow'] -lt $vals['frame_to_present_api']) { Fail 'frame_flow < frame_to_present_api' }
                    if ($vals['main_to_present_api'] -le $vals['frame_to_present_api']) { Fail 'main_to_present_api <= frame_to_present_api' }
                    $result.Timings = $vals
                }
            } else { Fail "timing line malformed: '$tm'" }

            # leak: case-insensitive wide match (no colon/space requirement), then
            # ^-anchored exact: optional real INFO timestamp or bare line, 0 bytes.
            $leaks = @($Lines | Where-Object { $null -ne $_ -and $_ -imatch 'total device memory leaked' })
            if ($leaks.Count -ne 1) { Fail "leak count $($leaks.Count) != 1" }
            elseif ($leaks[0] -cnotmatch '^(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?Total device memory leaked: 0 bytes\.$') {
                Fail "leak line malformed/nonzero: '$($leaks[0])'"
            }

            # footer: case-insensitive wide 'completed', then exact
            $comp = @($Lines | Where-Object { $null -ne $_ -and $_ -imatch 'completed' })
            if ($comp.Count -ne 1) { Fail "completed lines $($comp.Count) != 1" }
            foreach ($line in $comp) {
                if ($line -cnotmatch '^first-use (cold|restored) completed: failures=\d+ liveErrors=\d+ postErrors=\d+ outcome=(CLEAN|FAILED)$') {
                    Fail "malformed footer: '$line'"
                }
            }
            $wantFooter = "first-use $ExpectedPhase completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN"
            if (-not (($Lines | Where-Object { $_ -ceq $wantFooter }).Count -eq 1)) { Fail "footer exact '$wantFooter'" }

            # forbidden tokens (case-insensitive)
            foreach ($bad in @('VUID-', 'HAZARD', 'SKIPPED', 'INCOMPLETE', 'UNSUPPORTED')) {
                if ($text -imatch [regex]::Escape($bad)) { Fail "forbidden token $bad" }
            }
            $fails = @($Lines | Where-Object { $null -ne $_ -and $_ -cmatch 'FAIL:' })
            if ($fails.Count -gt 0) { Fail "positive log has $($fails.Count) FAIL lines" }

            # CSV: mandatory; canonical decimal Int64 per field == log
            $csvLines = @()
            try { $csvLines = @(Get-Content $CsvPath -ErrorAction Stop) } catch { Fail "csv read: $_" }
            if (-not ($csvLines.Count -eq 2)) { Fail "csv rows $($csvLines.Count) != 2 (header+data)" }
            else {
                $hdr = 'phase,validation,skip_draw,skip_submit,outcome,backend_init_ns,cache_config_ns,' +
                    'swapchain_create_ns,shader_compile_ns,pso_factory_ns,pso_driver_ns,combined_graphics_ns,' +
                    'frame_to_present_api_ns,frame_flow_ns,main_to_present_api_ns'
                if ($csvLines[0] -cne $hdr) { Fail 'csv header mismatch' }
                $cells = $csvLines[1] -split ','
                if ($cells.Count -ne 15) { Fail "csv columns $($cells.Count) != 15" }
                else {
                    if ($cells[0] -cne $ExpectedPhase) { Fail 'csv phase mismatch' }
                    if ($cells[1] -cne $Validation) { Fail 'csv validation mismatch' }
                    if ($cells[2] -cne '0' -or $cells[3] -cne '0') { Fail 'csv skip flags not 0' }
                    if ($cells[4] -cne 'CLEAN') { Fail 'csv outcome not CLEAN' }
                    if ($result.Timings.Count -eq 10) {
                        $csvNames = @('backend_init', 'cache_config', 'swapchain_create', 'shader_compile',
                            'pso_factory', 'pso_driver', 'combined_graphics', 'frame_to_present_api',
                            'frame_flow', 'main_to_present_api')
                        for ($i = 0; $i -lt 10; ++$i) {
                            $cell = $cells[5 + $i]
                            # reject +sign, whitespace, leading zero, overflow
                            $canonical = $cell -cmatch '^\d{1,19}$' -and -not ($cell.Length -gt 1 -and $cell.StartsWith('0'))
                            $v = [int64]0
                            if (-not $canonical -or -not [int64]::TryParse($cell, [ref]$v)) {
                                Fail "csv metric $($csvNames[$i]) non-canonical"; continue
                            }
                            if ($v -ne $result.Timings[$csvNames[$i]]) { Fail "csv metric $($csvNames[$i]) != log" }
                        }
                    }
                }
            }
        }
    } catch {
        Fail "judge exception: $_"
    }
    $result.Pass = ($result.Reasons.Count -eq 0)
    if (-not $result.Pass) {
        $result.SavedBytes = [int64]0
        $result.Timings = @{}
    }
    return $result
}

# --------------------------------------------------------------------------
# gate helper
# --------------------------------------------------------------------------
function Select-RestoredSample {
    param([bool]$ColdPassed, [int64]$ColdSavedBytes)
    if ($ColdPassed -and $ColdSavedBytes -gt 0) { return [int64]$ColdSavedBytes }
    return $null
}

# --------------------------------------------------------------------------
# SelfTest (unique ms+PID directory; never deleted, never reused)
# --------------------------------------------------------------------------
function Invoke-SelfTest {
    New-Item -ItemType Directory -Force -Path $selfTestDir | Out-Null

    $api = '03:41:05 [INFO]: Vulkan API: loader=1.4 requested=1.3 XR=1.3 instance=1.3 physical=1.4 effective=1.3'
    $paths = '03:41:05 [INFO]: Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0'
    $leak = '03:41:05 [INFO]: Total device memory leaked: 0 bytes.'
    $timing = 'first-use timing ns: backend_init=648602000 cache_config=149800 swapchain_create=43338400 shader_compile=48881700 pso_factory=2393800 pso_driver=2369700 combined_graphics=51275700 frame_to_present_api=2795000 frame_flow=2850500 main_to_present_api=763746700'

    function BuildLog([string]$phase, [string]$val) {
        $valLine = 'ENABLED'; if ($val -eq 'off') { $valLine = 'DISABLED' }
        $init = '0'; $loaded = '0'; $payload = '0'
        if ($phase -eq 'restored') { $init = '14048'; $loaded = '1'; $payload = '1' }
        $modeVal = 'on'; if ($val -eq 'off') { $modeVal = 'off' }
        return @(
            $api, $paths,
            "first-use validation: $valLine",
            "first-use mode: phase=$phase validation=$modeVal skipDraw=0 skipSubmit=0",
            "first-use cache-input: calls=1 loaded=$loaded initialBytes=$init payload=$payload",
            'first-use evidence: begin=1 graphics=1 draw=1 submit=1 present=1 chain=1',
            'first-use saved: bytes=14048 validated=1 dirty=0',
            $timing,
            $leak,
            "first-use $phase completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN"
        )
    }

    $csvHeader = 'phase,validation,skip_draw,skip_submit,outcome,backend_init_ns,cache_config_ns,' +
        'swapchain_create_ns,shader_compile_ns,pso_factory_ns,pso_driver_ns,combined_graphics_ns,' +
        'frame_to_present_api_ns,frame_flow_ns,main_to_present_api_ns'
    $coldCsvPath = Join-Path $selfTestDir 'cold.csv'
    $restCsvPath = Join-Path $selfTestDir 'restored.csv'
    $coldOffCsvPath = Join-Path $selfTestDir 'cold-off.csv'
    $restOffCsvPath = Join-Path $selfTestDir 'restored-off.csv'
    [System.IO.File]::WriteAllLines($coldCsvPath, @($csvHeader, "cold,on,0,0,CLEAN,648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))
    [System.IO.File]::WriteAllLines($restCsvPath, @($csvHeader, "restored,on,0,0,CLEAN,648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))
    [System.IO.File]::WriteAllLines($coldOffCsvPath, @($csvHeader, "cold,off,0,0,CLEAN,648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))
    [System.IO.File]::WriteAllLines($restOffCsvPath, @($csvHeader, "restored,off,0,0,CLEAN,648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))

    $script:fixtures = New-Object System.Collections.Generic.List[object]
    function Add([string]$name, [string[]]$lines, [object]$exit, [string]$phase, [string]$val, [bool]$want,
                 [object]$init = $null, [string]$csv = '') {
        $script:fixtures.Add([pscustomobject]@{ Name = $name; Lines = $lines; Exit = $exit; Phase = $phase; Val = $val; Want = $want; Init = $init; Csv = $csv }) | Out-Null
    }

    # positives
    Add 'valid-cold-on' (BuildLog 'cold' 'on') 0 'cold' 'on' $true $null $coldCsvPath
    Add 'valid-restored-on' (BuildLog 'restored' 'on') 0 'restored' 'on' $true ([int64]14048) $restCsvPath
    Add 'valid-cold-off' (BuildLog 'cold' 'off') 0 'cold' 'off' $true $null $coldOffCsvPath
    Add 'valid-restored-off' (BuildLog 'restored' 'off') 0 'restored' 'off' $true ([long]14048) $restOffCsvPath
    Add 'long-matched' (BuildLog 'restored' 'on') 0 'restored' 'on' $true ([long]14048) $restCsvPath

    # --- Lead rev2 five reds: REPLACEMENT single mutations ---
    $l = BuildLog 'cold' 'on'; $l[0] = 'junk Vulkan API: loader=1.4 requested=1.3 XR=1.3 instance=1.3 physical=1.4 effective=1.3'
    Add 'red-junk-api-prefix-replacement' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[1] = 'junk Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0'
    Add 'red-junk-paths-prefix-replacement' $l 0 'cold' 'on' $false $null $coldCsvPath
    Add 'red-restored-noncanonical-expected-leading-zero' (BuildLog 'restored' 'on') 0 'restored' 'on' $false '014048' $restCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'Total device memory leaked:4096 bytes.'
    Add 'red-append-malformed-leak' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'FIRST-USE saved: bytes=0 validated=0 dirty=1'
    Add 'red-append-uppercase-marker' $l 0 'cold' 'on' $false $null $coldCsvPath

    # --- previous families retained ---
    Add 'red-missing-csv-argument' (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null ''
    Add 'red-restored-without-cold-link' (BuildLog 'restored' 'on') 0 'restored' 'on' $false $null $restCsvPath
    # API/paths single-line REPLACEMENT tail junk / duplicate
    $l = BuildLog 'cold' 'on'; $l[0] = $api + ' trailing'; Add 'api-tail-junk' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[0] = $api; $l += $api; Add 'api-duplicate' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[1] = $paths + ' trailing'; Add 'paths-tail-junk' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[1] = $paths; $l += $paths; Add 'paths-duplicate' $l 0 'cold' 'on' $false $null $coldCsvPath
    # mixed case / no-space / truncated marker appends
    $l = BuildLog 'cold' 'on'; $l += 'FIRST-USE validation: ENABLED'; Add 'append-uppercase-marker-family' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'first-useevidence: begin=1 graphics=1 draw=1 submit=1 present=1 chain=1'; Add 'append-nospace-evidence' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'first-use cold completed: failures=0 liveErrors=0 postErrors=0'; Add 'append-truncated-footer' $l 0 'cold' 'on' $false $null $coldCsvPath
    # forbidden token appends
    $l = BuildLog 'cold' 'on'; $l += '04:41:05 [ERROR]: VUID-12345'; Add 'vuid' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'hazard: mixed case'; Add 'hazard' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'SKIPPED: frame'; Add 'skipped' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'INCOMPLETE: budget'; Add 'incomplete' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += '06:54:00 [INFO]: Vulkanpaths:timeline=0 sync2=1'; Add 'timestamped-nospace-paths' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += '06:54:00 [INFO]: VulkanAPI:effective=1.2'; Add 'timestamped-nospace-api' $l 0 'cold' 'on' $false $null $coldCsvPath
    # footer family
    $l = BuildLog 'cold' 'on'; $l[-1] = 'first-use cold completed: failures=0 liveErrors=0 postErrors=0'; Add 'footer-truncated' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[-1] = 'first-use cold completed: failures=01 liveErrors=0 postErrors=0 outcome=CLEAN'; Add 'footer-01' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += 'first-use cold completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN'; Add 'footer-duplicate' $l 0 'cold' 'on' $false $null $coldCsvPath
    # validation family
    $l = BuildLog 'cold' 'on'; $l[2] = 'first-use validation: DISABLED'; Add 'validation-wrong' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[2] = 'first-usevalidation: ENABLED'; Add 'validation-nospace' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[2] = 'first-use validation: ENABLED'; $l += 'first-use validation: ENABLED'; Add 'validation-duplicate' $l 0 'cold' 'on' $false $null $coldCsvPath
    # leak family
    $l = BuildLog 'cold' 'on'; $l[8] = $leak.Replace(' 0 bytes', ' 4096 bytes'); Add 'leak-nonzero' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l += $leak; Add 'leak-duplicate' $l 0 'cold' 'on' $false $null $coldCsvPath
    # saved family
    $l = BuildLog 'cold' 'on'; $l[6] = 'first-use saved: bytes=0 validated=1 dirty=0'; Add 'saved-0' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[6] = 'first-use saved: bytes=99999999999999999999999 validated=1 dirty=0'; Add 'saved-overflow' $l 0 'cold' 'on' $false $null $coldCsvPath
    # evidence family
    $l = BuildLog 'cold' 'on'; $l[5] = 'first-use evidence: begin=1 graphics=1 draw=1 submit=1 present=1 chain=0'; Add 'evidence-chain0' $l 0 'cold' 'on' $false $null $coldCsvPath
    # timing family
    $l = BuildLog 'cold' 'on'; $l[7] = 'first-use timing ns: backend_init=648602000 cache_config=149800 swapchain_create=43338400 shader_compile=48881700 pso_factory=2393800 pso_driver=2369700 combined_graphics=51275700 frame_to_present_api=2795000 frame_flow=2850500'; Add 'timing-missing' $l 0 'cold' 'on' $false $null $coldCsvPath
    $l = BuildLog 'cold' 'on'; $l[7] = 'first-use timing ns: backend_init=0648602000 cache_config=149800 swapchain_create=43338400 shader_compile=48881700 pso_factory=2393800 pso_driver=2369700 combined_graphics=51275700 frame_to_present_api=2795000 frame_flow=2850500 main_to_present_api=763746700'; Add 'timing-leading0' $l 0 'cold' 'on' $false $null $coldCsvPath
    # exit/unknown
    Add 'exit-ntstatus' (BuildLog 'cold' 'on') (-1073740791) 'cold' 'on' $false $null $coldCsvPath
    Add 'phase-unknown' (BuildLog 'cold' 'on') 0 'unknownPhase' 'on' $false $null $coldCsvPath
    Add 'validation-unknown' (BuildLog 'cold' 'on') 0 'cold' 'unknownValidation' $false $null $coldCsvPath

    # CSV per-metric mismatches (10 independent)
    $metricNames = @('backend_init_ns', 'cache_config_ns', 'swapchain_create_ns', 'shader_compile_ns',
        'pso_factory_ns', 'pso_driver_ns', 'combined_graphics_ns', 'frame_to_present_api_ns',
        'frame_flow_ns', 'main_to_present_api_ns')
    $metricIdx = 5
    foreach ($mn in $metricNames) {
        $cells = @('cold', 'on', '0', '0', 'CLEAN', '648602000', '149800', '43338400', '48881700', '2393800', '2369700', '51275700', '2795000', '2850500', '763746700')
        $cells[$metricIdx] = '1'
        $mCsv = Join-Path $selfTestDir ("metric-$mn.csv")
        [System.IO.File]::WriteAllLines($mCsv, @($csvHeader, ($cells -join ',')))
        Add "csv-metric-$mn" (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null $mCsv
        $metricIdx++
    }
    # CSV + sign / whitespace negatives
    $plusCsv = Join-Path $selfTestDir 'plus.csv'
    [System.IO.File]::WriteAllLines($plusCsv, @($csvHeader, "cold,on,0,0,CLEAN,+648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))
    Add 'csv-plus-sign' (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null $plusCsv
    $wsCsv = Join-Path $selfTestDir 'ws.csv'
    [System.IO.File]::WriteAllLines($wsCsv, @($csvHeader, "cold,on,0,0,CLEAN, 648602000,149800,43338400,48881700,2393800,2369700,51275700,2795000,2850500,763746700"))
    Add 'csv-whitespace' (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null $wsCsv
    $emptyCsv = Join-Path $selfTestDir 'empty.csv'
    [System.IO.File]::WriteAllText($emptyCsv, '')
    Add 'csv-empty-file' (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null $emptyCsv
    Add 'csv-missing-file' (BuildLog 'cold' 'on') 0 'cold' 'on' $false $null (Join-Path $selfTestDir 'nope.csv')

    # restored init family
    Add 'restored-init-missing' (BuildLog 'restored' 'on') 0 'restored' 'on' $false $null $restCsvPath
    Add 'restored-init-0' (BuildLog 'restored' 'on') 0 'restored' 'on' $false ([int64]0) $restCsvPath
    Add 'restored-init-negative' (BuildLog 'restored' 'on') 0 'restored' 'on' $false ([int64]-5) $restCsvPath
    Add 'restored-init-mismatch' (BuildLog 'restored' 'on') 0 'restored' 'on' $false ([int64]999) $restCsvPath
    Add 'restored-init-unparseable' (BuildLog 'restored' 'on') 0 'restored' 'on' $false 'abc' $restCsvPath

    # Select-RestoredSample gate control flow
    $g1 = Select-RestoredSample -ColdPassed $true -ColdSavedBytes ([int64]14048)
    $g2 = Select-RestoredSample -ColdPassed $false -ColdSavedBytes ([int64]14048)
    $g3 = Select-RestoredSample -ColdPassed $true -ColdSavedBytes ([int64]0)
    $gateOk = ($g1 -eq [int64]14048) -and ($null -eq $g2) -and ($null -eq $g3)
    if (-not $gateOk) { $script:failedAny = $true; Write-Host '  SELFTEST FAIL: restored gate control flow' -ForegroundColor Red }

    $declared = $script:fixtures.Count
    $executed = 0; $passed = 0
    foreach ($f in $script:fixtures) {
        $executed++
        $params = @{ Lines = [string[]]$f.Lines; ExitCode = $f.Exit; ExpectedPhase = $f.Phase; Validation = $f.Val; ExpectedInitialBytes = $f.Init; CsvPath = $f.Csv }
        $r = Test-FirstUseLog @params
        if ($r.Pass -eq $f.Want) { $passed++ }
        else {
            Write-Host ("  SELFTEST FAIL: {0} got={1} want={2} reasons={3}" -f $f.Name, $r.Pass, $f.Want, ($r.Reasons -join '; ')) -ForegroundColor Red
            $script:failedAny = $true
        }
    }
    Write-Host ("SelfTest fixtures declared={0} executed={1} passed={2} (gate control flow: {3})" -f $declared, $executed, $passed, $(if ($gateOk) { 'OK' } else { 'FAIL' }))
    if ($executed -ne $declared -or $passed -ne $declared) { $script:failedAny = $true }
}

# --------------------------------------------------------------------------
# formal matrix
# --------------------------------------------------------------------------
function Invoke-Formal {
    if ($Validation -ne 'both' -and $Validation -ne 'on' -and $Validation -ne 'off') {
        throw "unknown -Validation '$Validation'"
    }
    if ($Repeats -lt 5) { throw "Repeats must be >= 5 (got $Repeats)" }

    if ($Validation -eq 'both' -or $Validation -eq 'on') {
        Write-Host 'building ON target (win64-simulator)...' -ForegroundColor Cyan
        & cmake --build (Join-Path $repo 'native\build\vulkan-modernization\win64-simulator') --config Release --target vulkan_first_use_smoke --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "ON build failed (exit $LASTEXITCODE)" }
    }
    if ($Validation -eq 'both' -or $Validation -eq 'off') {
        Write-Host 'building OFF target (win64-benchmark-off)...' -ForegroundColor Cyan
        & cmake --build (Join-Path $repo 'native\build\vulkan-modernization\win64-benchmark-off') --config Release --target vulkan_first_use_smoke --parallel 4
        if ($LASTEXITCODE -ne 0) { throw "OFF build failed (exit $LASTEXITCODE)" }
    }

    # unique run root: ms + PID, refuse reuse
    $runRoot = $null
    for ($i = 0; $i -lt 100 -and -not $runRoot; ++$i) {
        $candidate = Join-Path $runRootBase ("run-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + "-" + $PID)
        if (-not (Test-Path $candidate)) { $runRoot = $candidate }
        else { Start-Sleep -Milliseconds 7 }
    }
    if (-not $runRoot) { throw 'cannot allocate unique run root' }
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    Write-Host "run root: $runRoot"

    $saved = @{}
    foreach ($name in @('PATH', 'VK_LAYER_PATH', 'VK_IMPLICIT_LAYER_PATH', 'CC_VULKAN_FORCE_API',
            'CC_VULKAN_COMPLETION', 'CC_VULKAN_SUBMIT', 'CC_VULKAN_BARRIER',
            'CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES')) {
        $item = Get-Item ("Env:" + $name) -ErrorAction SilentlyContinue
        if ($item) { $saved[$name] = $item.Value }
    }
    $sentinelExistsBefore = @{ PATH = $saved.ContainsKey('PATH'); RENDERING = $saved.ContainsKey('CC_VULKAN_RENDERING') }
    $oldCwd = (Get-Location).Path
    $samples = New-Object System.Collections.Generic.List[string]
    $seenKeys = New-Object System.Collections.Generic.List[string]
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

        $combos = @()
        if ($Validation -eq 'both' -or $Validation -eq 'on') { $combos += @{ Val = 'on'; Exe = $onExe } }
        if ($Validation -eq 'both' -or $Validation -eq 'off') { $combos += @{ Val = 'off'; Exe = $offExe } }

        foreach ($combo in $combos) {
            for ($rep = 1; $rep -le $Repeats; ++$rep) {
                $pairDir = Join-Path $runRoot ("{0}-rep{1}" -f $combo.Val, $rep)
                if (Test-Path $pairDir) { throw "pair dir already exists (reuse refused): $pairDir" }
                New-Item -ItemType Directory -Force -Path $pairDir | Out-Null
                Set-Location $pairDir

                $coldLog = Join-Path $pairDir 'cold.log'
                cmd /c """$($combo.Exe)"" --phase cold --validation $($combo.Val) > ""$coldLog"" 2>&1"
                $coldExit = $LASTEXITCODE
                $coldLines = @(Get-Content $coldLog -ErrorAction SilentlyContinue)
                $coldCsvSrc = Join-Path $pairDir 'first-use-cwd\first-use.csv'
                $coldJudge = Test-FirstUseLog -Lines $coldLines -ExitCode $coldExit -ExpectedPhase 'cold' -Validation $combo.Val -CsvPath $coldCsvSrc
                if ($coldJudge.Pass) {
                    Copy-Item $coldCsvSrc (Join-Path $pairDir 'cold.csv') -Force
                    $coldJudge = Test-FirstUseLog -Lines $coldLines -ExitCode $coldExit -ExpectedPhase 'cold' -Validation $combo.Val -CsvPath (Join-Path $pairDir 'cold.csv')
                }
                $coldBytes = [int64]$coldJudge.SavedBytes
                if ($coldJudge.Pass) {
                    $samples.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13}" -f
                        $combo.Val, $rep, $coldBytes, 'cold',
                        $coldJudge.Timings['backend_init'], $coldJudge.Timings['cache_config'], $coldJudge.Timings['swapchain_create'],
                        $coldJudge.Timings['shader_compile'], $coldJudge.Timings['pso_factory'], $coldJudge.Timings['pso_driver'],
                        $coldJudge.Timings['combined_graphics'], $coldJudge.Timings['frame_to_present_api'],
                        $coldJudge.Timings['frame_flow'], $coldJudge.Timings['main_to_present_api'])) | Out-Null
                } else {
                    Write-Host ("  {0} rep{1} cold FAIL: cold sample not collected; restored pair BLOCKED" -f $combo.Val, $rep) -ForegroundColor Red
                    $script:failedAny = $true
                    foreach ($r in $coldJudge.Reasons) { Write-Host "    - $r" -ForegroundColor Red }
                    continue
                }
                Write-Host ("  {0} rep{1} cold: exit={2} judge=PASS savedBytes={3}" -f $combo.Val, $rep, $coldExit, $coldBytes) -ForegroundColor Green

                $restExpected = Select-RestoredSample -ColdPassed $coldJudge.Pass -ColdSavedBytes $coldBytes
                if ($null -eq $restExpected) { throw 'restored gate refused after passing cold (unexpected)' }
                $restLog = Join-Path $pairDir 'restored.log'
                cmd /c """$($combo.Exe)"" --phase restored --validation $($combo.Val) > ""$restLog"" 2>&1"
                $restExit = $LASTEXITCODE
                $restLines = @(Get-Content $restLog -ErrorAction SilentlyContinue)
                $restCsvSrc = Join-Path $pairDir 'first-use-cwd\first-use.csv'
                $restJudge = Test-FirstUseLog -Lines $restLines -ExitCode $restExit -ExpectedPhase 'restored' -Validation $combo.Val -ExpectedInitialBytes $restExpected -CsvPath $restCsvSrc
                if ($restJudge.Pass) {
                    Copy-Item $restCsvSrc (Join-Path $pairDir 'restored.csv') -Force
                    $restJudge = Test-FirstUseLog -Lines $restLines -ExitCode $restExit -ExpectedPhase 'restored' -Validation $combo.Val -ExpectedInitialBytes $restExpected -CsvPath (Join-Path $pairDir 'restored.csv')
                }
                if (-not $restJudge.Pass) {
                    $script:failedAny = $true
                    Write-Host ("  {0} rep{1} restored FAIL" -f $combo.Val, $rep) -ForegroundColor Red
                    foreach ($r in $restJudge.Reasons) { Write-Host "    - $r" -ForegroundColor Red }
                    continue
                }
                Write-Host ("  {0} rep{1} restored: exit={2} judge=PASS" -f $combo.Val, $rep, $restExit) -ForegroundColor Green
                $samples.Add(("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13}" -f
                    $combo.Val, $rep, $restExpected, 'restored',
                    $restJudge.Timings['backend_init'], $restJudge.Timings['cache_config'], $restJudge.Timings['swapchain_create'],
                    $restJudge.Timings['shader_compile'], $restJudge.Timings['pso_factory'], $restJudge.Timings['pso_driver'],
                    $restJudge.Timings['combined_graphics'], $restJudge.Timings['frame_to_present_api'],
                    $restJudge.Timings['frame_flow'], $restJudge.Timings['main_to_present_api'])) | Out-Null
            }
        }

        # strict row accounting: rows == combos * Repeats * 2; duplicate
        # validation/rep/phase keys fail
        $expectedRows = $combos.Count * $Repeats * 2
        if ($samples.Count -ne $expectedRows) {
            $script:failedAny = $true
            Write-Host ("FAIL: samples rows {0} != expected {1}" -f $samples.Count, $expectedRows) -ForegroundColor Red
        }
        foreach ($row in $samples) {
            $p = $row -split ','
            $key = "{0}/{1}/{2}" -f $p[0], $p[1], $p[3]
            if ($seenKeys -ccontains $key) { $script:failedAny = $true; Write-Host "FAIL: duplicate key $key" -ForegroundColor Red }
            $seenKeys.Add($key) | Out-Null
        }

        $samplesPath = Join-Path $runRoot 'samples.csv'
        $sw = [System.IO.StreamWriter]::new($samplesPath)
        $sw.WriteLine('validation,repeat,saved_bytes,phase,backend_init_ns,cache_config_ns,swapchain_create_ns,' +
            'shader_compile_ns,pso_factory_ns,pso_driver_ns,combined_graphics_ns,frame_to_present_api_ns,' +
            'frame_flow_ns,main_to_present_api_ns')
        foreach ($row in $samples) { $sw.WriteLine($row) }
        $sw.Flush(); $sw.Close()
        Write-Host "samples.csv: $samplesPath ($($samples.Count) qualified process rows)" -ForegroundColor Cyan
        # summary.csv: per validation/phase/metric, n=Repeats, nearest-rank
        # P95/P99, median as safe midpoint double for even n (descriptive only)
        $summaryPath = Join-Path $runRoot 'summary.csv'
        $sw = [System.IO.StreamWriter]::new($summaryPath)
        $sw.WriteLine('validation,phase,metric,n,median,p95,p99')
        $metricNames = @('backend_init_ns', 'cache_config_ns', 'swapchain_create_ns', 'shader_compile_ns',
            'pso_factory_ns', 'pso_driver_ns', 'combined_graphics_ns', 'frame_to_present_api_ns',
            'frame_flow_ns', 'main_to_present_api_ns')
        $groups = @{}
        foreach ($row in $samples) {
            $p = $row -split ','
            for ($mi = 0; $mi -lt 10; ++$mi) {
                $key = "{0}/{1}/{2}" -f $p[0], $p[3], $metricNames[$mi]
                if (-not $groups.ContainsKey($key)) { $groups[$key] = @() }
                $groups[$key] = @($groups[$key]) + [int64]$p[4 + $mi]
            }
        }
        foreach ($key in $groups.Keys) {
            try {
            $sortedVals = New-Object System.Collections.Generic.List[int64]
            foreach ($v in $groups[$key]) { $sortedVals.Add([int64]$v) }
            $sorted = @($sortedVals.ToArray() | Sort-Object)
            $n = $sorted.Count
            if ($n -lt 1) { continue }
            $mid = [int][math]::Floor($n / 2)
            if ($n % 2 -eq 1) {
                $median = [double]$sorted[$mid]
            } else {
                $median = ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
            }
            $i95 = [int][math]::Ceiling(0.95 * $n) - 1
            $i99 = [int][math]::Ceiling(0.99 * $n) - 1
            if ($i95 -lt 0) { $i95 = 0 }
            if ($i99 -lt 0) { $i99 = 0 }
            if ($i95 -gt $n - 1) { $i95 = $n - 1 }
            if ($i99 -gt $n - 1) { $i99 = $n - 1 }
            $p95 = [double]$sorted[$i95]
            $p99 = [double]$sorted[$i99]
            $parts = $key -split '/'
            $outLine = $parts[0] + ',' + $parts[1] + ',' + $parts[2] + ',' + $n + ',' + $median + ',' + $p95 + ',' + $p99
            $sw.WriteLine($outLine)
            } catch {
                Write-Host ("  key=" + $key + " n=" + $sorted.Count + " mid=" + $mid + " i95=" + $i95 + " i99=" + $i99 + " sortedType=" + $sorted.GetType().FullName)
                $script:failedAny = $true
            }
        }
        $sw.Flush(); $sw.Close()
        Write-Host "summary.csv: $summaryPath ($($groups.Count) groups, descriptive only)" -ForegroundColor Cyan
        foreach ($key in $groups.Keys) {
            if ($groups[$key].Count -ne $Repeats) {
                $script:failedAny = $true
                Write-Host ("FAIL: group {0} n={1} != Repeats {2}" -f $key, $groups[$key].Count, $Repeats) -ForegroundColor Red
            }
        }
        Write-Host 'NOTE: finite Win first-frame numbers are descriptive; not game/display/process-cold-start; no warm-uplift or 3% gate claim.' -ForegroundColor Yellow
    } catch {
        Write-Host "EXCEPTION: $_" -ForegroundColor Red
        $script:failedAny = $true
    } finally {
        foreach ($name in @('PATH', 'VK_LAYER_PATH', 'VK_IMPLICIT_LAYER_PATH', 'CC_VULKAN_FORCE_API',
                'CC_VULKAN_COMPLETION', 'CC_VULKAN_SUBMIT', 'CC_VULKAN_BARRIER',
                'CC_VULKAN_RENDERING', 'CC_VULKAN_DISABLE_FEATURES')) {
            if ($saved.ContainsKey($name)) { Set-Item -Path ("Env:" + $name) -Value $saved[$name] }
            else { Remove-Item ("Env:" + $name) -ErrorAction SilentlyContinue }
        }
        $pathOk = ($env:PATH -eq $saved['PATH'])
        $renderOk = $true
        if ($sentinelExistsBefore.RENDERING) { $renderOk = ($env:CC_VULKAN_RENDERING -eq $saved['CC_VULKAN_RENDERING']) }
        else { $renderOk = ($null -eq $env:CC_VULKAN_RENDERING) }
        Write-Host ("env sentinel: PATH restored={0} RENDERING-restored={1}" -f $pathOk, $renderOk)
        if (-not $pathOk -or -not $renderOk) { $script:failedAny = $true }
        Set-Location $oldCwd
    }
}

if ($Validation -ne 'both' -and $Validation -ne 'on' -and $Validation -ne 'off') {
    Write-Error "unknown -Validation '$Validation'"
    exit 1
}
if ($Repeats -lt 5) {
    Write-Error 'Repeats must be >= 5'
    exit 1
}
if ($SelfTest) {
    Invoke-SelfTest
} else {
    Invoke-Formal
}
if ($script:failedAny) { exit 1 }
exit 0
