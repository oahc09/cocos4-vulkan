# Recording-lifetime contract verification (M3-G2/M3-I, strict).
#
# ALL specified cases must PASS. Any failure/crash/NTSTATUS = overall FAIL.
# -Completion auto|fence|timeline (default auto); -ApiVersion 1.1|1.2|1.3 optional.
# -Cases for subsets; -SelfTest exercises the judge with fixtures.
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string[]]$Cases = @(),
    [ValidateSet('auto','fence','timeline')][string]$Completion = 'auto',
    [ValidateSet('','1.1','1.2','1.3')][string]$ApiVersion = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Continue'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'native\build\vulkan-modernization\win64-simulator' }
$vmRoot = Join-Path $repo 'native\build\vulkan-modernization'
$allCases = @('control', 'delayed-resource', 'pool-reset', 'pending-drop', 'facade-rebegin', 'secondary-lifetime')

# ---- strict judge -------------------------------------------------------------
function Test-RecordingLifetimeLog {
    param(
        [AllowEmptyCollection()][string[]]$Lines = @(),
        [int]$ExitCode = 0,
        [string]$CaseName = "",
        [string]$ExpectTimeline = '',   # '' = skip; 'on'|'off'
        [string]$ExpectEffective = ''  # '' = skip; '1.1'|'1.2'|'1.3'
    )
    $reasons = @()
    if ($ExitCode -ne 0) { $reasons += "exit $ExitCode != 0" }
    if (-not ($Lines | Where-Object { $_ -match 'validation \+ best practices \+ sync: ENABLED' })) {
        $reasons += "validation not ENABLED"
    }
    # unique footer: exact line anchor, precise zeros, valid outcome token
    $footers = @($Lines | Where-Object { $_ -match ("^" + [regex]::Escape($CaseName) + " completed: failures=0 liveErrors=0 postErrors=0 outcome=[A-Z-]+$") })
    $anyFooters = @($Lines | Where-Object { $_ -match ("^" + [regex]::Escape($CaseName) + " completed:") })
    if ($anyFooters.Count -ne 1) { $reasons += "footer count=$($anyFooters.Count) (expected 1)" }
    elseif ($footers.Count -ne 1) { $reasons += "footer present but not clean" }
    if ($Lines | Where-Object { $_ -match 'SKIPPED|INCOMPLETE' }) { $reasons += "SKIPPED/INCOMPLETE" }
    $vuids = ($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD' }).Count
    if ($vuids -gt 0) { $reasons += "$vuids VUID/hazard lines" }
    $fails = ($Lines | Where-Object { $_ -match '^FAIL:' }).Count
    if ($fails -gt 0) { $reasons += "$fails FAIL lines" }
    # memory leak line: must be exactly 1 line and exactly 0 bytes
    $memLeaks = @($Lines | Where-Object { $_ -match 'Total device memory leaked:' })
    if ($memLeaks.Count -ne 1) { $reasons += "memory-leak line count=$($memLeaks.Count) (expected 1)" }
    elseif ($memLeaks[0] -notmatch 'Total device memory leaked: 0 bytes\.$') { $reasons += "memory leak non-zero: $($memLeaks[0])" }
    # completion backend verification
    if ($ExpectTimeline) {
        $tl = ($Lines | Where-Object { $_ -match 'timeline=([01])' } | Select-Object -First 1)
        if (-not $tl) { $reasons += "no timeline= line found" }
        elseif ($ExpectTimeline -eq 'on' -and $tl -notmatch 'timeline=1') { $reasons += "expected timeline=1 but got: $tl" }
        elseif ($ExpectTimeline -eq 'off' -and $tl -notmatch 'timeline=0') { $reasons += "expected timeline=0 but got: $tl" }
    }
    if ($ExpectEffective) {
        $eff = ($Lines | Where-Object { $_ -match 'effective=([0-9.]+)' } | Select-Object -First 1)
        if (-not $eff) { $reasons += "no effective= line found" }
        elseif ($eff -notmatch ("effective=" + [regex]::Escape($ExpectEffective) + "`$")) { $reasons += "expected effective=$ExpectEffective but got: $eff" }
    }
    # key stage markers per case
    $markers = @{
        'control'            = @('control: submitObserved=')
        'delayed-resource'   = @('delayed: submit attempt:', 'delayed: after rotation')
        'pool-reset'         = @('pool-reset: slot=', 'pool-reset: submit calls')
        'pending-drop'       = @('pending-drop: after user destroy', 'pending-drop: after rotation')
        'facade-rebegin'     = @('facade: original=', 'facade: first submit:', 'facade: second submit:')
        'secondary-lifetime' = @('secondary: primary=', 'secondary: FB NOT destroyed')
    }
    # M5-I: real Classic secondary inheritance evidence marker (unique + well-formed)
    if ($CaseName -eq 'secondary-lifetime') {
        $goodM = @($Lines | Where-Object { $_ -match ('^CLASSIC_SECONDARY_INHERITANCE: rp=0?x?0*[1-9a-fA-F][0-9a-fA]* fbo=0?x?[0-9a-fA]+ subpass=0 continue=1 dynamicPNext=0 executed=0?x?0*[1-9a-fA][0-9a-fA]* result=0' + [char]36) })
        $anyM = @($Lines | Where-Object { $_ -match '^CLASSIC_SECONDARY_INHERITANCE:' })
        if ($anyM.Count -ne 1) { $reasons += "CLASSIC_SECONDARY_INHERITANCE count=$($anyM.Count) (expected 1)" }
        elseif ($goodM.Count -ne 1) { $reasons += "CLASSIC_SECONDARY_INHERITANCE present but malformed: $($anyM[0])" }
    }
    if ($markers.ContainsKey($CaseName)) {
        foreach ($pat in $markers[$CaseName]) {
            if (-not ($Lines | Where-Object { $_ -match [regex]::Escape($pat) })) {
                $reasons += "missing stage marker '$pat'"
            }
        }
    }
    return [pscustomobject]@{ Pass = ($reasons.Count -eq 0); Reasons = $reasons; Footer = ($anyFooters | Select-Object -First 1) }
}

function Invoke-SelfTest {
    $base = @(
        'validation + best practices + sync: ENABLED'
        'Vulkan paths: timeline=1 sync2=0 submit2=0'
        'Vulkan API: loader=1.4 instance=1.1 physical=1.4 effective=1.1'
        'Total device memory leaked: 0 bytes.'
        'control: submitObserved=1'
        'control completed: failures=0 liveErrors=0 postErrors=0 outcome=PASS-CLEAN'
    )
    # Each negative retains every other condition of its valid base.
    $fixtures = @(
        @{ Name='valid'; Lines=$base; Exit=0; Expect=$true }
        @{ Name='missing-footer'; Lines=$base[0..4]; Exit=0; Expect=$false }
        @{ Name='duplicate-footer'; Lines=($base + $base[-1]); Exit=0; Expect=$false }
        @{ Name='failures-one'; Lines=($base -replace 'failures=0','failures=1'); Exit=0; Expect=$false }
        @{ Name='failures-zero-prefix'; Lines=($base -replace 'failures=0','failures=01'); Exit=0; Expect=$false }
        @{ Name='live-errors'; Lines=($base -replace 'liveErrors=0','liveErrors=1'); Exit=0; Expect=$false }
        @{ Name='post-errors'; Lines=($base -replace 'postErrors=0','postErrors=1'); Exit=0; Expect=$false }
        @{ Name='footer-junk'; Lines=($base[0..4] + ($base[-1] + ' junk')); Exit=0; Expect=$false }
        @{ Name='crash-only'; Lines=$base; Exit=-1073741819; Expect=$false }
        @{ Name='fail-only'; Lines=($base + 'FAIL: fixture'); Exit=0; Expect=$false }
        @{ Name='vuid-only'; Lines=($base + 'VUID-fixture'); Exit=0; Expect=$false }
        @{ Name='skipped-only'; Lines=($base + 'SKIPPED'); Exit=0; Expect=$false }
        @{ Name='incomplete-only'; Lines=($base + 'INCOMPLETE'); Exit=0; Expect=$false }
        @{ Name='missing-validation'; Lines=$base[1..5]; Exit=0; Expect=$false }
        @{ Name='missing-leak'; Lines=($base | Where-Object { $_ -notlike 'Total device memory*' }); Exit=0; Expect=$false }
        @{ Name='nonzero-leak'; Lines=($base -replace 'leaked: 0','leaked: 4096'); Exit=0; Expect=$false }
        @{ Name='duplicate-zero-leak'; Lines=($base + $base[3]); Exit=0; Expect=$false }
        @{ Name='nonzero-then-zero-leak'; Lines=(@('Total device memory leaked: 4096 bytes.') + $base); Exit=0; Expect=$false }
        @{ Name='zero-then-nonzero-leak'; Lines=($base + 'Total device memory leaked: 4096 bytes.'); Exit=0; Expect=$false }
        @{ Name='missing-stage'; Lines=($base | Where-Object { $_ -notlike 'control: submitObserved=*' }); Exit=0; Expect=$false }
        @{ Name='timeline-mismatch'; Lines=$base; Exit=0; Timeline='off'; Expect=$false }
        @{ Name='timeline-matches'; Lines=$base; Exit=0; Timeline='on'; Expect=$true }
        @{ Name='effective-matches'; Lines=$base; Exit=0; Effective='1.1'; Expect=$true }
        @{ Name='effective-mismatch'; Lines=($base -replace 'effective=1.1','effective=1x1'); Exit=0; Effective='1.1'; Expect=$false }
    )
    $pendingBase = @(
        'validation + best practices + sync: ENABLED'
        'Total device memory leaked: 0 bytes.'
        'pending-drop: after user destroy (pending): destroyFB=0 destroyView=0'
        'pending-drop: after rotation: destroyFB=1 destroyView=1'
        'pending-drop completed: failures=0 liveErrors=0 postErrors=0 outcome=DROPPED-AND-DESTROYED'
    )
    $fixtures += @(
        @{ Name='pending-valid'; Case='pending-drop'; Lines=$pendingBase; Exit=0; Expect=$true }
        @{ Name='pending-missing-stage'; Case='pending-drop'; Lines=($pendingBase[0..1] + $pendingBase[3..4]); Exit=0; Expect=$false }
    )
    $good=$true
    foreach ($fixture in $fixtures) {
        $name=if ($fixture.Case) {$fixture.Case} else {'control'}
        $result=Test-RecordingLifetimeLog -Lines @($fixture.Lines) -ExitCode $fixture.Exit -CaseName $name -ExpectTimeline $fixture.Timeline -ExpectEffective $fixture.Effective
        $ok=$result.Pass -eq $fixture.Expect
        Write-Host "SelfTest $($fixture.Name): $ok"
        if (-not $ok) { $good=$false; $result.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    # M5-I marker fixtures (shared judge, synthetic logs)
    $baseLog = @(
        'validation + best practices + sync: ENABLED'
        'secondary: primary=0x1 forwarded=1 execute=1'
        'secondary: FB NOT destroyed (destroyFB=0 destroyView=0)'
        'Total device memory leaked: 0 bytes.'
        'secondary-lifetime completed: failures=0 liveErrors=0 postErrors=0 outcome=SECONDARY-OK'
    )
    $goodMarker = 'CLASSIC_SECONDARY_INHERITANCE: rp=0x1234 fbo=0xabcd subpass=0 continue=1 dynamicPNext=0 executed=0x5678 result=0'
    $markerFixtures = @(
        @{ Name='sec-marker-missing';   Lines=$baseLog },
        @{ Name='sec-marker-duplicate'; Lines=($baseLog[0..0] + $goodMarker + $goodMarker + $baseLog[1..4]) },
        @{ Name='sec-marker-malformed'; Lines=($baseLog[0..0] + ($goodMarker -replace 'subpass=0','subpass=2') + $baseLog[1..4]) },
        @{ Name='sec-marker-null-rp';   Lines=($baseLog[0..0] + ($goodMarker -replace 'rp=0x1234','rp=0x0') + $baseLog[1..4]) },
        @{ Name='sec-marker-bad-result';Lines=($baseLog[0..0] + ($goodMarker -replace 'result=0$','result=-3') + $baseLog[1..4]) },
        @{ Name='sec-marker-valid';     Lines=($baseLog[0..0] + $goodMarker + $baseLog[1..4]); Expect=$true }
    )
    foreach ($mf in $markerFixtures) {
        $result = Test-RecordingLifetimeLog -Lines @($mf.Lines) -ExitCode 0 -CaseName 'secondary-lifetime'
        $ok = $result.Pass -eq [bool]$mf.Expect
        Write-Host "SelfTest $($mf.Name): $ok"
        if (-not $ok) { $good=$false; $result.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    return $good
}

if ($SelfTest) {
    $ok = Invoke-SelfTest
    Write-Host ("SelfTest exit: {0}" -f $(if ($ok) {0} else {1}))
    exit $(if ($ok) {0} else {1})
}

if ($Cases.Count -gt 0) {
    $Cases = $Cases | Select-Object -Unique  # deduplicate, don't run same case twice
    foreach ($c in $Cases) {
        if ($allCases -notcontains $c) { Write-Error "unknown case '$c'. valid: $($allCases -join ', ')"; exit 1 }
    }
} else { $Cases = $allCases }

# ---- build + run -------------------------------------------------------------
$oldPath = $env:PATH; $oldLayer = $env:VK_LAYER_PATH; $oldImplicit = $env:VK_IMPLICIT_LAYER_PATH
$oldForce = $env:CC_VULKAN_FORCE_API; $oldDisable = $env:CC_VULKAN_DISABLE_FEATURES; $oldCompletion = $env:CC_VULKAN_COMPLETION
$oldLoc = Get-Location
try {
    & cmake --build $BuildDir --config Release --target vulkan_recording_lifetime_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_recording_lifetime_smoke.exe'

    $env:VK_LAYER_PATH = 'D:\Software\VulkanSDK\1.4.341.1\Bin'
    $empty = Join-Path $vmRoot 'empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH = $empty
    $env:PATH = (Join-Path $repo 'native\external\win64\libs') + ';' + (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    # set completion policy for this run
    if ($Completion -eq 'auto') { Remove-Item Env:CC_VULKAN_COMPLETION -ErrorAction SilentlyContinue }
    else { $env:CC_VULKAN_COMPLETION = $Completion }
    if ($ApiVersion) { $env:CC_VULKAN_FORCE_API = $ApiVersion } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    Push-Location $vmRoot

    $expectTl = ''
    if ($Completion -eq 'fence') { $expectTl = 'off' }
    elseif ($Completion -eq 'timeline') { $expectTl = 'on' }

    $all = $true
    foreach ($c in $Cases) {
        $tag2 = if ($Completion -ne 'auto') { $Completion } else { 'auto' }
        if ($ApiVersion) { $tag2 += "-v$ApiVersion" }
        $log = Join-Path $vmRoot ("rec-life-{0}-{1}-{2:yyyyMMdd-HHmmss-fff}-{3}.log" -f $c, $tag2, (Get-Date), $PID)
        cmd /c """$exe"" --case ""$c"" > ""$log"" 2>&1"
        $code = $LASTEXITCODE
        $text = if (Test-Path $log) { [System.IO.File]::ReadAllText($log) } else { '' }
        if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
        $lines = @($text -split "`r?`n")
        $v = Test-RecordingLifetimeLog -Lines $lines -ExitCode $code -CaseName $c -ExpectTimeline $expectTl -ExpectEffective $ApiVersion
        $tag = if ($v.Pass) {'PASS'} else {'FAIL'}
        Write-Host ("  [{0}] {1} [{2}]: exit={3} {4}" -f $tag, $c, $tag2, $code, $v.Footer)
        if (-not $v.Pass) {
            $all = $false
            $v.Reasons | Select-Object -First 3 | ForEach-Object { Write-Host "    reason: $_" -ForegroundColor Red }
            $lines | Where-Object { $_ -match '^FAIL:|VUID-' } | Select-Object -First 2 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        }
    }
    Write-Host "`n=== recording-lifetime overall ($Completion$(if($ApiVersion){"/v$ApiVersion"})): $(if ($all) {'PASS'} else {'FAIL'}) ==="
    exit $(if ($all) {0} else {1})
} catch { Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
finally {
    $env:PATH = $oldPath
    if ($null -ne $oldLayer) { $env:VK_LAYER_PATH = $oldLayer } else { Remove-Item Env:VK_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldImplicit) { $env:VK_IMPLICIT_LAYER_PATH = $oldImplicit } else { Remove-Item Env:VK_IMPLICIT_LAYER_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $oldForce) { $env:CC_VULKAN_FORCE_API = $oldForce } else { Remove-Item Env:CC_VULKAN_FORCE_API -ErrorAction SilentlyContinue }
    if ($null -ne $oldDisable) { $env:CC_VULKAN_DISABLE_FEATURES = $oldDisable } else { Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue }
    if ($null -ne $oldCompletion) { $env:CC_VULKAN_COMPLETION = $oldCompletion } else { Remove-Item Env:CC_VULKAN_COMPLETION -ErrorAction SilentlyContinue }
    if ((Get-Location).Path -ne $oldLoc.Path) { Pop-Location }
}
