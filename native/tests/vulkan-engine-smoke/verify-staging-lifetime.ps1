[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string[]]$Cases = @(),
    [ValidateSet('auto','fence','timeline')][string]$Completion = 'auto',
    [ValidateSet('','1.1','1.2','1.3')][string]$ApiVersion = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Continue'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$outputRoot = Join-Path $repo 'native\build\vulkan-modernization'
if (-not $BuildDir) { $BuildDir = Join-Path $outputRoot 'win64-simulator' }
$allCases = @('control','delayed-staging','reuse-after-drop','reuse-after-completion')

function Test-StagingLog {
    param([AllowEmptyCollection()][string[]]$Lines, [int]$ExitCode,
          [string]$CaseName, [string]$Backend = 'auto', [string]$Effective = '')
    $reasons = @()
    $casePattern = [regex]::Escape($CaseName)
    $outcome = @{ 'control'='CLEAN'; 'delayed-staging'='PRESERVED'; 'reuse-after-drop'='REUSED'; 'reuse-after-completion'='REUSED' }[$CaseName]
    if ($ExitCode -ne 0) { $reasons += "exit=$ExitCode" }
    if (@($Lines | Where-Object { $_ -ceq 'validation + best practices + sync: ENABLED' }).Count -ne 1) { $reasons += 'validation missing/duplicated' }
    $footers = @($Lines | Where-Object { $_ -match ('^' + $casePattern + ' completed:') })
    if ($footers.Count -ne 1 -or $footers[0] -cne "$CaseName completed: failures=0 liveErrors=0 postErrors=0 outcome=$outcome") { $reasons += 'missing/duplicate/unclean footer' }
    if ($Lines | Where-Object { $_ -match 'VUID-|SYNC-HAZARD|FAIL:|SKIPPED|INCOMPLETE' }) { $reasons += 'error or incomplete marker' }
    $memory = @($Lines | Where-Object { $_ -match 'Total device memory leaked:' })
    if ($memory.Count -ne 1 -or $memory[0] -notmatch 'Total device memory leaked: 0 bytes\.$') { $reasons += 'missing/duplicate/nonzero leak' }
    $pattern = if ($CaseName -like 'reuse-*') { '5a' } else { 'a5' }
    $data = @($Lines | Where-Object { $_ -match ('^staging data: case=' + $casePattern + ' ') })
    if ($data.Count -ne 1 -or $data[0] -cne "staging data: case=$CaseName bytes=64 mismatches=0 first=0x$pattern expected=0x$pattern") { $reasons += 'readback missing/duplicated/incorrect' }
    if (@($Lines | Where-Object { $_ -ceq "staging submit: case=$CaseName forwarded=1" }).Count -ne 1) { $reasons += 'real submission missing/duplicated' }
    if ($CaseName -eq 'delayed-staging') {
        if (@($Lines | Where-Object { $_ -match '^staging range: case=delayed-staging sameBuffer=[01] overlap=0 oldOffset=\d+ newOffset=\d+$' }).Count -ne 1) { $reasons += 'protected range missing' }
        if (@($Lines | Where-Object { $_ -match '^staging isolation: rotations=[1-9]\d* commandPoolResets=0$' }).Count -ne 1) { $reasons += 'pool-reset isolation missing' }
    }
    if ($CaseName -like 'reuse-*' -and @($Lines | Where-Object { $_ -ceq "staging reuse: case=$CaseName sameBuffer=1 sameOffset=1" }).Count -ne 1) { $reasons += 'allocation reuse missing' }
    if ($CaseName -eq 'reuse-after-drop' -and @($Lines | Where-Object { $_ -ceq 'staging drop: beforeReplacementSubmit=0' }).Count -ne 1) { $reasons += 'pending drop evidence missing' }
    if ($Backend -ne 'auto') {
        $timeline = if ($Backend -eq 'timeline') { '1' } else { '0' }
        if (@($Lines | Where-Object { $_ -match ('Vulkan paths: timeline=' + $timeline + '\b') }).Count -ne 1) { $reasons += 'completion backend mismatch' }
    }
    if ($Effective -and @($Lines | Where-Object { $_ -match ('Vulkan API:.*effective=' + [regex]::Escape($Effective) + '$') }).Count -ne 1) { $reasons += 'effective API mismatch' }
    [pscustomobject]@{ Pass=($reasons.Count -eq 0); Reasons=$reasons; Footer=($footers | Select-Object -First 1) }
}

if ($SelfTest) {
    $base = @('validation + best practices + sync: ENABLED',
              'Total device memory leaked: 0 bytes.',
              'staging submit: case=control forwarded=1',
              'staging data: case=control bytes=64 mismatches=0 first=0xa5 expected=0xa5',
              'control completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN')
    $fixtures = @(
        @{Name='valid';Lines=$base;Code=0;Pass=$true},
        @{Name='missing-footer';Lines=$base[0..3];Code=0;Pass=$false},
        @{Name='duplicate-footer';Lines=($base + $base[-1]);Code=0;Pass=$false},
        @{Name='crash-only';Lines=$base;Code=-1073741819;Pass=$false},
        @{Name='mismatches';Lines=($base -replace 'mismatches=0','mismatches=64');Code=0;Pass=$false},
        @{Name='wrong-byte';Lines=($base -replace 'first=0xa5','first=0x5a');Code=0;Pass=$false},
        @{Name='not-forwarded';Lines=($base -replace 'forwarded=1','forwarded=0');Code=0;Pass=$false},
        @{Name='missing-submit';Lines=($base | Where-Object { $_ -notlike 'staging submit:*' });Code=0;Pass=$false},
        @{Name='missing-validation';Lines=$base[1..4];Code=0;Pass=$false},
        @{Name='missing-leak';Lines=($base | Where-Object { $_ -notlike 'Total device*' });Code=0;Pass=$false},
        @{Name='nonzero-leak';Lines=($base -replace 'leaked: 0','leaked: 4096');Code=0;Pass=$false},
        @{Name='duplicate-zero-leak';Lines=($base + $base[1]);Code=0;Pass=$false},
        @{Name='nonzero-then-zero-leak';Lines=(@('Total device memory leaked: 4096 bytes.') + $base);Code=0;Pass=$false},
        @{Name='failure-zero-prefix';Lines=($base -replace 'failures=0','failures=01');Code=0;Pass=$false},
        @{Name='footer-junk';Lines=($base[0..3] + ($base[-1] + ' junk'));Code=0;Pass=$false},
        @{Name='fail-only';Lines=($base + 'FAIL: fixture');Code=0;Pass=$false},
        @{Name='vuid-only';Lines=($base + 'VUID-fixture');Code=0;Pass=$false},
        @{Name='skipped-only';Lines=($base + 'SKIPPED');Code=0;Pass=$false},
        @{Name='incomplete-only';Lines=($base + 'INCOMPLETE');Code=0;Pass=$false}
    )
    $delayed = @($base -replace 'case=control','case=delayed-staging' -replace 'control completed:','delayed-staging completed:' -replace 'outcome=CLEAN','outcome=PRESERVED') + @(
        'staging range: case=delayed-staging sameBuffer=1 overlap=0 oldOffset=0 newOffset=64',
        'staging isolation: rotations=3 commandPoolResets=0'
    )
    $drop = @($base -replace 'case=control','case=reuse-after-drop' -replace 'control completed:','reuse-after-drop completed:' -replace 'outcome=CLEAN','outcome=REUSED' -replace '0xa5','0x5a') + @(
        'staging reuse: case=reuse-after-drop sameBuffer=1 sameOffset=1',
        'staging drop: beforeReplacementSubmit=0'
    )
    $completed = @($drop -replace 'reuse-after-drop','reuse-after-completion' | Where-Object { $_ -notlike 'staging drop:*' })
    $fixtures += @(
        @{Name='delayed-valid';Case='delayed-staging';Lines=$delayed;Code=0;Pass=$true},
        @{Name='delayed-missing-range';Case='delayed-staging';Lines=($delayed | Where-Object { $_ -notlike 'staging range:*' });Code=0;Pass=$false},
        @{Name='delayed-overlap';Case='delayed-staging';Lines=($delayed -replace 'overlap=0','overlap=1');Code=0;Pass=$false},
        @{Name='delayed-pool-reset';Case='delayed-staging';Lines=($delayed -replace 'commandPoolResets=0','commandPoolResets=1');Code=0;Pass=$false},
        @{Name='drop-valid';Case='reuse-after-drop';Lines=$drop;Code=0;Pass=$true},
        @{Name='drop-missing-proof';Case='reuse-after-drop';Lines=($drop | Where-Object { $_ -notlike 'staging drop:*' });Code=0;Pass=$false},
        @{Name='drop-not-reused';Case='reuse-after-drop';Lines=($drop -replace 'sameOffset=1','sameOffset=0');Code=0;Pass=$false},
        @{Name='completed-valid';Case='reuse-after-completion';Lines=$completed;Code=0;Pass=$true},
        @{Name='completed-missing-reuse';Case='reuse-after-completion';Lines=($completed | Where-Object { $_ -notlike 'staging reuse:*' });Code=0;Pass=$false}
    )
    $good = $true
    foreach ($fixture in $fixtures) {
        $fixtureCase = if ($fixture.Case) {$fixture.Case} else {'control'}
        $result = Test-StagingLog -Lines @($fixture.Lines) -ExitCode $fixture.Code -CaseName $fixtureCase
        $ok = $result.Pass -eq $fixture.Pass
        Write-Host "SelfTest $($fixture.Name): $ok"
        if (-not $ok) { $good=$false }
    }
    exit $(if ($good) {0} else {1})
}
if ($Cases.Count -eq 0) { $Cases=$allCases } else { $Cases=@($Cases | Select-Object -Unique) }
foreach ($case in $Cases) { if ($allCases -notcontains $case) { Write-Error "unknown case: $case"; exit 1 } }
$variables = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_COMPLETION','CC_VULKAN_FORCE_API','CC_VULKAN_DISABLE_FEATURES')
$previous = @{}
foreach ($variable in $variables) { $previous[$variable]=[Environment]::GetEnvironmentVariable($variable,'Process') }
$locationPushed=$false
try {
    & cmake --build $BuildDir --config Release --target vulkan_staging_lifetime_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'staging build failed' }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_staging_lifetime_smoke.exe'
    $env:VK_LAYER_PATH='D:\Software\VulkanSDK\1.4.341.1\Bin'
    $empty=Join-Path $outputRoot 'empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH=$empty
    $env:PATH=(Join-Path $repo 'native\external\win64\libs') + ';' + (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    $env:CC_VULKAN_COMPLETION=if ($Completion -eq 'auto') {$null} else {$Completion}
    $env:CC_VULKAN_FORCE_API=if ($ApiVersion) {$ApiVersion} else {$null}
    $env:CC_VULKAN_DISABLE_FEATURES=$null
    Push-Location $outputRoot; $locationPushed=$true
    $good=$true
    foreach ($case in $Cases) {
        $log=Join-Path $outputRoot ("staging-{0}-{1}-v{2}-{3:yyyyMMdd-HHmmss-fff}-{4}.log" -f $case,$Completion,$ApiVersion,(Get-Date),$PID)
        cmd /c """$exe"" --case ""$case"" > ""$log"" 2>&1"
        $code=$LASTEXITCODE
        $lines=@([System.IO.File]::ReadAllLines($log))
        $result=Test-StagingLog -Lines $lines -ExitCode $code -CaseName $case -Backend $Completion -Effective $ApiVersion
        Write-Host "$(if ($result.Pass) {'PASS'} else {'FAIL'}) $case exit=${code}: $($result.Footer)"
        Write-Host "log: $log"
        if (-not $result.Pass) { $good=$false; $result.Reasons | ForEach-Object { Write-Host "  $_" } }
    }
    exit $(if ($good) {0} else {1})
} catch { Write-Host "FAIL: $($_.Exception.Message)"; exit 1 }
finally {
    if ($locationPushed) { Pop-Location }
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable,$previous[$variable],'Process') }
}
