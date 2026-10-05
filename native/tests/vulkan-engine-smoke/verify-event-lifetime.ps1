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
$allCases = @('control-full','split-buffer-pair','held-observation','unmatched-split-end','event-reuse')

function Test-EventLog {
    param([AllowEmptyCollection()][string[]]$Lines, [int]$ExitCode,
          [string]$CaseName, [string]$Backend='auto', [string]$Effective='')
    $reasons=@()
    $escaped=[regex]::Escape($CaseName)
    $outcome=@{'control-full'='CLEAN';'unmatched-split-end'='CLEAN';'split-buffer-pair'='SPLIT-OK';'held-observation'='HELD-OBSERVED';'event-reuse'='REUSE-OK'}[$CaseName]
    if ($ExitCode -ne 0) {$reasons+='nonzero exit'}
    if (@($Lines | Where-Object {$_ -ceq 'validation + best practices + sync: ENABLED'}).Count -ne 1) {$reasons+='validation missing/duplicated'}
    $footer=@($Lines | Where-Object {$_ -match ('^'+$escaped+' completed:')})
    if ($footer.Count -ne 1 -or $footer[0] -cne "$CaseName completed: failures=0 liveErrors=0 postErrors=0 outcome=$outcome") {$reasons+='missing/duplicate/unclean footer'}
    if ($Lines | Where-Object {$_ -match 'VUID-|SYNC-HAZARD|FAIL:|SKIPPED|INCOMPLETE'}) {$reasons+='error/incomplete marker'}
    $leak=@($Lines | Where-Object {$_ -match 'Total device memory leaked:'})
    if ($leak.Count -ne 1 -or $leak[0] -notmatch 'Total device memory leaked: 0 bytes\.$') {$reasons+='missing/duplicate/nonzero memory leak'}
    $eventCount=if ($CaseName -eq 'split-buffer-pair' -or $CaseName -eq 'held-observation') {1} elseif ($CaseName -eq 'event-reuse') {2} else {0}
    if (@($Lines | Where-Object {$_ -ceq "event destruction: created=$eventCount destroyed=$eventCount hostReset=0"}).Count -ne 1) {$reasons+='event ownership/destruction mismatch'}
    if ($CaseName -eq 'control-full' -or $CaseName -eq 'unmatched-split-end') {
        if (@($Lines | Where-Object {$_ -ceq 'control: mismatches=0/64 first=0xa5'}).Count -ne 1) {$reasons+='full barrier data mismatch'}
    } elseif ($CaseName -eq 'split-buffer-pair') {
        if (@($Lines | Where-Object {$_ -ceq 'split: setEvent=1 waitEvents=1 resetEvent=1 createEvent=1 destroyEvent=0'}).Count -ne 1) {$reasons+='missing split pair commands'}
        if (@($Lines | Where-Object {$_ -ceq 'split: mismatches=0/64 first=0xa5'}).Count -ne 1) {$reasons+='split data mismatch'}
    } elseif ($CaseName -eq 'event-reuse') {
        $pairs=@($Lines | Where-Object {$_ -match '^reuse: pair1 cb=[0-9A-Fa-fx]+ event=[0-9A-Fa-fx]+ pair2 cb=[0-9A-Fa-fx]+ event=[0-9A-Fa-fx]+ created=2$'})
        $distinct=$false
        if ($pairs.Count -eq 1 -and $pairs[0] -match ' event=(?<first>[0-9A-Fa-fx]+) pair2 cb=[0-9A-Fa-fx]+ event=(?<second>[0-9A-Fa-fx]+) ') {
            try {
                $first=[Convert]::ToUInt64(($Matches.first -replace '^0[xX]',''),16)
                $second=[Convert]::ToUInt64(($Matches.second -replace '^0[xX]',''),16)
                $distinct=$first -ne 0 -and $second -ne 0 -and $first -ne $second
            } catch { $distinct=$false }
        }
        if (-not $distinct) {$reasons+='missing/null/duplicate pending event identities'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: identities distinct=1 matched=1 reused=1'}).Count -ne 1) {$reasons+='missing matched/reused identity evidence'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: final: set=3 wait=3 reset=3 create=2 destroy=0 hostReset=0'}).Count -ne 1) {$reasons+='missing event command/cache counts'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: submit1: forwarded cb1=1 cb2=0 submits=1'}).Count -ne 1) {$reasons+='submit1 must forward only pair1'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: submit2: forwarded cb2=1'}).Count -ne 1) {$reasons+='submit2 must forward pair2'}
        if (@($Lines | Where-Object {$_ -match '^reuse: pair3: cb=.+ create=2 \(was 2\)$'}).Count -ne 1) {$reasons+='pair3 must reuse event (create=2)'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: submit3: forwarded cb3=1'}).Count -ne 1) {$reasons+='submit3 must forward pair3'}
        if (@($Lines | Where-Object {$_ -ceq 'reuse: submissions first=1 second=1 third=1 completed=1'}).Count -ne 1) {$reasons+='missing three-submission phase marker'}
    } elseif ($CaseName -eq 'held-observation') {
        if (@($Lines | Where-Object {$_ -match '^held: synthetic held completion observation; GPU already idle prefix=\d+ point=[1-9]\d*$'}).Count -ne 1) {$reasons+='missing synthetic scope'}
        if (@($Lines | Where-Object {$_ -match '^held: after CB destroy: hostReset=0 destroyEvent=0 queries=[1-9]\d*$'}).Count -ne 1) {$reasons+='event destroyed/reset before observed retirement'}
        if (@($Lines | Where-Object {$_ -ceq 'held: after real retirement: destroyEvent=1 hostReset=0'}).Count -ne 1) {$reasons+='event not retired exactly once'}
    }
    if ($Backend -ne 'auto') {
        $timeline=if ($Backend -eq 'timeline') {'1'} else {'0'}
        if (@($Lines | Where-Object {$_ -match ('Vulkan paths: timeline='+$timeline+'\b')}).Count -ne 1) {$reasons+='wrong completion backend'}
    }
    if ($Effective -and @($Lines | Where-Object {$_ -match ('Vulkan API:.*effective='+[regex]::Escape($Effective)+'$')}).Count -ne 1) {$reasons+='wrong effective API'}
    [pscustomobject]@{Pass=($reasons.Count -eq 0);Reasons=$reasons;Footer=($footer | Select-Object -First 1)}
}

if ($SelfTest) {
    $base=@('validation + best practices + sync: ENABLED',
            'Total device memory leaked: 0 bytes.',
            'event destruction: created=0 destroyed=0 hostReset=0',
            'control: mismatches=0/64 first=0xa5',
            'control-full completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN')
    $held=@($base[0..1])+@('event destruction: created=1 destroyed=1 hostReset=0',
            'held: synthetic held completion observation; GPU already idle prefix=1 point=2',
            'held: after CB destroy: hostReset=0 destroyEvent=0 queries=1',
            'held: after real retirement: destroyEvent=1 hostReset=0',
            'held-observation completed: failures=0 liveErrors=0 postErrors=0 outcome=HELD-OBSERVED')
    $split=@($base[0..1])+@('event destruction: created=1 destroyed=1 hostReset=0',
            'split: setEvent=1 waitEvents=1 resetEvent=1 createEvent=1 destroyEvent=0',
            'split: mismatches=0/64 first=0xa5',
            'split-buffer-pair completed: failures=0 liveErrors=0 postErrors=0 outcome=SPLIT-OK')
    $reuse=@($base[0..1])+@(
        'event destruction: created=2 destroyed=2 hostReset=0',
        'reuse: pair1 cb=0xAAA event=0x15 pair2 cb=0xBBB event=0x16 created=2',
        'reuse: submit1: forwarded cb1=1 cb2=0 submits=1',
        'reuse: submit2: forwarded cb2=1',
        'reuse: pair3: cb=0xCCC create=2 (was 2)',
        'reuse: identities distinct=1 matched=1 reused=1',
        'reuse: submit3: forwarded cb3=1',
        'reuse: final: set=3 wait=3 reset=3 create=2 destroy=0 hostReset=0',
        'reuse: submissions first=1 second=1 third=1 completed=1',
        'event-reuse completed: failures=0 liveErrors=0 postErrors=0 outcome=REUSE-OK')
    $fixtures=@(
        @{Name='valid';Lines=$base;Pass=$true},
        @{Name='crash-only';Lines=$base;Code=-1073741819;Pass=$false},
        @{Name='missing-footer';Lines=$base[0..3];Pass=$false},
        @{Name='duplicate-footer';Lines=($base+$base[-1]);Pass=$false},
        @{Name='footer-junk';Lines=($base[0..3]+($base[-1]+' junk'));Pass=$false},
        @{Name='failure-prefix';Lines=($base -replace 'failures=0','failures=01');Pass=$false},
        @{Name='data-mismatch';Lines=($base -replace 'mismatches=0','mismatches=64');Pass=$false},
        @{Name='leak-only';Lines=($base -replace 'leaked: 0','leaked: 64');Pass=$false},
        @{Name='duplicate-zero-leak';Lines=($base+$base[1]);Pass=$false},
        @{Name='missing-validation';Lines=$base[1..4];Pass=$false},
        @{Name='fail-only';Lines=($base+'FAIL: fixture');Pass=$false},
        @{Name='hazard-only';Lines=($base+'SYNC-HAZARD-fixture');Pass=$false},
        @{Name='skipped-only';Lines=($base+'SKIPPED');Pass=$false},
        @{Name='incomplete-only';Lines=($base+'INCOMPLETE');Pass=$false},
        @{Name='held-valid';Case='held-observation';Lines=$held;Pass=$true},
        @{Name='counter-not-observed';Case='held-observation';Lines=($held -replace 'queries=1','queries=0');Pass=$false},
        @{Name='early-destroy';Case='held-observation';Lines=($held -replace 'after CB destroy: hostReset=0 destroyEvent=0','after CB destroy: hostReset=0 destroyEvent=1');Pass=$false},
        @{Name='host-reset';Case='held-observation';Lines=($held -replace 'after CB destroy: hostReset=0','after CB destroy: hostReset=1');Pass=$false},
        @{Name='not-retired';Case='held-observation';Lines=($held -replace 'after real retirement: destroyEvent=1','after real retirement: destroyEvent=0');Pass=$false},
        @{Name='scope-missing';Case='held-observation';Lines=($held | Where-Object {$_ -notlike 'held: synthetic*'});Pass=$false},
        @{Name='split-valid';Case='split-buffer-pair';Lines=$split;Pass=$true},
        @{Name='no-wait';Case='split-buffer-pair';Lines=($split -replace 'waitEvents=1','waitEvents=0');Pass=$false},
        @{Name='no-device-reset';Case='split-buffer-pair';Lines=($split -replace 'resetEvent=1','resetEvent=0');Pass=$false},
        @{Name='unmatched-valid';Case='unmatched-split-end';Lines=($base -replace 'control-full completed:','unmatched-split-end completed:');Pass=$true},
        @{Name='reuse-valid';Case='event-reuse';Lines=$reuse;Pass=$true},
        @{Name='reuse-same-event';Case='event-reuse';Lines=($reuse -replace 'pair2 cb=0xBBB event=0x16','pair2 cb=0xBBB event=0x15');Pass=$false},
        @{Name='reuse-not-submitted-third';Case='event-reuse';Lines=($reuse | Where-Object {$_ -notlike 'reuse: submit3:*'});Pass=$false},
        @{Name='reuse-extra-create';Case='event-reuse';Lines=($reuse -replace 'cb=0xCCC create=2','cb=0xCCC create=3');Pass=$false},
        @{Name='reuse-missing-destroy';Case='event-reuse';Lines=($reuse -replace 'created=2 destroyed=2','created=2 destroyed=1');Pass=$false},
        @{Name='reuse-not-completed';Case='event-reuse';Lines=($reuse -replace 'completed=1','completed=0');Pass=$false},
        @{Name='reuse-not-matched';Case='event-reuse';Lines=($reuse -replace 'matched=1','matched=0');Pass=$false},
        @{Name='reuse-duplicate-submit-stage';Case='event-reuse';Lines=($reuse+'reuse: submit2: forwarded cb2=1');Pass=$false},
        @{Name='reuse-missing-identities';Case='event-reuse';Lines=($reuse | Where-Object {$_ -notlike 'reuse: identities*'});Pass=$false}
    )
    $good=$true
    foreach ($fixture in $fixtures) {
        $case=if ($fixture.Case) {$fixture.Case} else {'control-full'}
        $result=Test-EventLog -Lines @($fixture.Lines) -ExitCode $fixture.Code -CaseName $case
        $ok=$result.Pass -eq $fixture.Pass
        Write-Host "SelfTest $($fixture.Name): $ok"
        if (-not $ok) {$good=$false;$result.Reasons | ForEach-Object {Write-Host "  $_"}}
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
    & cmake --build $BuildDir --config Release --target vulkan_event_lifetime_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'event build failed' }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_event_lifetime_smoke.exe'
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
        $log=Join-Path $outputRoot ("event-{0}-{1}-v{2}-{3:yyyyMMdd-HHmmss-fff}-{4}.log" -f $case,$Completion,$ApiVersion,(Get-Date),$PID)
        cmd /c """$exe"" --case ""$case"" > ""$log"" 2>&1"
        $code=$LASTEXITCODE
        $lines=@([System.IO.File]::ReadAllLines($log))
        $result=Test-EventLog -Lines $lines -ExitCode $code -CaseName $case -Backend $Completion -Effective $ApiVersion
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
