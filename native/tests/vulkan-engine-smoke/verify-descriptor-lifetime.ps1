[CmdletBinding()]
param(
    [string]$BuildDir='', [string[]]$Cases=@(),
    [ValidateSet('auto','fence','timeline')][string]$Completion='auto',
    [ValidateSet('legacy','submit2')][string]$Submit='legacy',
    [ValidateSet('legacy','sync2')][string]$Barrier='legacy',
    [ValidateSet('','1.1','1.2','1.3')][string]$ApiVersion='', [switch]$TemplateUpdates, [switch]$SelfTest
)
$ErrorActionPreference='Continue'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
$outputRoot=Join-Path $repo 'native\build\vulkan-modernization'
if (-not $BuildDir) {$BuildDir=Join-Path $outputRoot 'win64-simulator'}
$defaultCases=@('control','update-after-record','update-during-record')
$allCases=$defaultCases+@('delayed-dynamic-slot','delayed-dynamic-offset')
function Test-DescriptorLog {
    param([AllowEmptyCollection()][string[]]$Lines,[int]$ExitCode,[string]$CaseName,[string]$Backend='auto',[string]$Effective='',[bool]$Templates=$false,[string]$Encoder='legacy',[string]$BarrierEncoder='legacy')
    $reasons=@()
    if ($ExitCode -ne 0) {$reasons+='nonzero exit'}
    if (@($Lines | Where-Object {$_ -ceq 'validation + best practices + sync: ENABLED'}).Count -ne 1) {$reasons+='validation missing/duplicated'}
    $templateValue=[int]$Templates
    if (@($Lines | Where-Object {$_ -ceq "descriptor update path: template=$templateValue cachedTemplatePFN=$templateValue"}).Count -ne 1) {$reasons+='missing selected descriptor path'}
    if (@($Lines | Where-Object {$_ -ceq "descriptor config: template=$templateValue autoBarrier=0"}).Count -ne 1) {$reasons+='wrong configuration'}
    $outcome=if ($CaseName -eq 'control') {'CLEAN'} else {'PRESERVED'}
    $footer=@($Lines | Where-Object {$_ -match ('^'+[regex]::Escape($CaseName)+' completed:')})
    if ($footer.Count -ne 1 -or $footer[0] -cne "$CaseName completed: failures=0 liveErrors=0 postErrors=0 outcome=$outcome") {$reasons+='missing/duplicate/unclean footer'}
    if ($Lines | Where-Object {$_ -match 'VUID-|SYNC-HAZARD|FAIL:|SKIPPED|INCOMPLETE'}) {$reasons+='error/incomplete marker'}
    $leak=@($Lines | Where-Object {$_ -match 'Total device memory leaked:'})
    if ($leak.Count -ne 1 -or $leak[0] -notmatch 'Total device memory leaked: 0 bytes\.$') {$reasons+='missing/duplicate/nonzero memory leak'}
    $encoderLines=@($Lines | Where-Object {$_ -match '^descriptor encoder:'})
    $encoderPattern=if ($Encoder -eq 'submit2') {'^descriptor encoder: submit=submit2 legacyCalls=0 submit2Calls=[1-9][0-9]*$'} else {'^descriptor encoder: submit=legacy legacyCalls=[1-9][0-9]* submit2Calls=0$'}
    if ($encoderLines.Count -ne 1 -or $encoderLines[0] -notmatch $encoderPattern) {$reasons+='actual submit encoder mismatch'}
    $pathLines=@($Lines | Where-Object {$_ -match 'Vulkan paths:'})
    $barrierBit=if ($BarrierEncoder -eq 'sync2') {'1'} else {'0'}
    if ($pathLines.Count -ne 1 -or $pathLines[0] -notmatch ('Vulkan paths:.*\bsync2='+$barrierBit+'\b')) {$reasons+='barrier configuration mismatch'}
    $dispatches=if ($CaseName -eq 'update-during-record') {2} else {1}
    foreach ($marker in @('descriptor submit: originalAttempt=1',"descriptor execution: bound=1 dispatches=$dispatches attempted=1 forwarded=1",'descriptor data: mismatches=0/64 first=0xa5')) {
        if (@($Lines | Where-Object {$_ -ceq $marker}).Count -ne 1) {$reasons+="missing/duplicate phase: $marker"}
    }
    if ($CaseName -in @('update-after-record','update-during-record') -and @($Lines | Where-Object {$_ -match '^descriptor mutation: oldSet=.+ lastWritten=.+ sameSet=0 slotStable=1$'}).Count -ne 1) {$reasons+='missing descriptor version separation'}
    if ($CaseName -in @('delayed-dynamic-slot','delayed-dynamic-offset')) {
        if (@($Lines | Where-Object {$_ -ceq 'dynamic points: recording=1 submitting=1 unrelated=0'}).Count -ne 1) {$reasons+='incorrect physical slot point association'}
        $gateLines=@($Lines | Where-Object {$_ -match '^dynamic gate:'})
        if ($gateLines.Count -eq 1 -and $gateLines[0] -match 'recordedSlot=(\d+) submitSlot=(\d+) point=(\d+) completed=(\d+)$') {
            if ($Matches[1] -eq $Matches[2] -or [UInt64]$Matches[4] -ge [UInt64]$Matches[3]) {$reasons+='inconsistent pending/slot evidence'}
            $recordedSlot=$Matches[1]
        }
        $reuseLines=@($Lines | Where-Object {$_ -match '^dynamic reuse:'})
        if ($reuseLines.Count -eq 1 -and $reuseLines[0] -match 'originalSlot=(\d+) currentSlot=(\d+)') {
            if ($Matches[1] -ne $Matches[2] -or $Matches[1] -ne $recordedSlot) {$reasons+='instance reuse did not reach recorded slot'}
        }
        if (@($Lines | Where-Object {$_ -match '^dynamic gate: injected=1 pending=1 recordedSlot=\d+ submitSlot=\d+ point=[1-9]\d* completed=\d+$'}).Count -ne 1) {$reasons+='missing real pending gate'}
        if (@($Lines | Where-Object {$_ -match '^dynamic reuse: originalSlot=\d+ currentSlot=\d+ byte=0x5a gateReleased=[01]$'}).Count -ne 1) {$reasons+='missing real instance reuse'}
        if (@($Lines | Where-Object {$_ -ceq 'dynamic release: signaled=1'}).Count -ne 1) {$reasons+='gate was not released'}
    }
    if ($CaseName -eq 'delayed-dynamic-offset') {
        $offsetLines=@($Lines | Where-Object {$_ -match '^dynamic offset:'})
        if ($offsetLines.Count -ne 1 -or $offsetLines[0] -notmatch '^dynamic offset: requested=([1-9]\d*) bound=([1-9]\d*)$' -or $Matches[1] -ne $Matches[2]) {$reasons+='missing/incorrect actual dynamic offset'}
    }
    if ($CaseName -eq 'update-during-record') {
        foreach ($marker in @('descriptor refresh: rebound=1','descriptor second data: mismatches=0/64 first=0x5a')) {
            if (@($Lines | Where-Object {$_ -ceq $marker}).Count -ne 1) {$reasons+="missing second dispatch phase: $marker"}
        }
    }
    if ($Backend -ne 'auto') {
        $timeline=if ($Backend -eq 'timeline') {'1'} else {'0'}
        if (@($Lines | Where-Object {$_ -match ('Vulkan paths: timeline='+$timeline+'\b')}).Count -ne 1) {$reasons+='wrong completion backend'}
    }
    if ($Effective -and @($Lines | Where-Object {$_ -match ('Vulkan API:.*effective='+[regex]::Escape($Effective)+'$')}).Count -ne 1) {$reasons+='wrong effective API'}
    [pscustomobject]@{Pass=($reasons.Count -eq 0);Reasons=$reasons;Footer=($footer | Select-Object -First 1)}
}
if ($SelfTest) {
    $base=@('validation + best practices + sync: ENABLED','descriptor config: template=0 autoBarrier=0',
        'Total device memory leaked: 0 bytes.','descriptor submit: originalAttempt=1',
        'descriptor execution: bound=1 dispatches=1 attempted=1 forwarded=1',
        'descriptor data: mismatches=0/64 first=0xa5','descriptor update path: template=0 cachedTemplatePFN=0',
        'control completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN',
        'descriptor encoder: submit=legacy legacyCalls=1 submit2Calls=0',
        'Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0')
    $mutation=@($base -replace '^control completed:','update-after-record completed:' -replace 'outcome=CLEAN','outcome=PRESERVED')+@('descriptor mutation: oldSet=0x15 lastWritten=0x16 sameSet=0 slotStable=1')
    $fixtures=@(
        @{Name='valid';Lines=$base;Pass=$true},
        @{Name='missing-footer';Lines=@($base | Where-Object {$_ -notmatch '^control completed:'});Pass=$false},
        @{Name='duplicate-footer';Lines=($base+$base[7]);Pass=$false},
        @{Name='only-crash';Lines=$base;Code=-1073741819;Pass=$false},
        @{Name='only-vuid';Lines=($base+'VUID-fixture');Pass=$false},
        @{Name='no-bind';Lines=($base -replace 'bound=1','bound=0');Pass=$false},
        @{Name='no-dispatch';Lines=($base -replace 'dispatches=1','dispatches=0');Pass=$false},
        @{Name='no-successful-submit';Lines=($base -replace 'forwarded=1','forwarded=0');Pass=$false},
        @{Name='wrong-data';Lines=($base -replace 'mismatches=0','mismatches=64');Pass=$false},
        @{Name='nonzero-leak';Lines=($base -replace 'leaked: 0','leaked: 64');Pass=$false},
        @{Name='duplicate-leak';Lines=($base+$base[2]);Pass=$false},
        @{Name='no-validation';Lines=@($base | Where-Object {$_ -ne 'validation + best practices + sync: ENABLED'});Pass=$false},
        @{Name='skipped-only';Lines=($base+'SKIPPED');Pass=$false},
        @{Name='mutation-valid';Case='update-after-record';Lines=$mutation;Pass=$true},
        @{Name='mutation-same-set';Case='update-after-record';Lines=($mutation -replace 'sameSet=0','sameSet=1');Pass=$false},
        @{Name='mutation-slot-changed';Case='update-after-record';Lines=($mutation -replace 'slotStable=1','slotStable=0');Pass=$false},
        @{Name='mutation-missing';Case='update-after-record';Lines=($mutation | Where-Object {$_ -notlike 'descriptor mutation:*'});Pass=$false}
    )
    $during=@($mutation -replace 'update-after-record','update-during-record' -replace 'dispatches=1','dispatches=2')+@('descriptor refresh: rebound=1','descriptor second data: mismatches=0/64 first=0x5a')
    $fixtures+=@(
        @{Name='during-valid';Case='update-during-record';Lines=$during;Pass=$true},
        @{Name='during-no-rebind';Case='update-during-record';Lines=($during -replace 'rebound=1','rebound=0');Pass=$false},
        @{Name='during-old-data';Case='update-during-record';Lines=($during -replace 'first=0x5a','first=0xa5');Pass=$false}
    )
    $template=@($during -replace 'template=0','template=1' -replace 'cachedTemplatePFN=0','cachedTemplatePFN=1')
    $fixtures+=@(
        @{Name='template-valid';Case='update-during-record';Templates=$true;Lines=$template;Pass=$true},
        @{Name='template-wrong-mode';Case='update-during-record';Templates=$true;Lines=($template -replace 'config: template=1','config: template=0');Pass=$false}
    )
    $dynamic=@($base -replace '^control completed:','delayed-dynamic-slot completed:' -replace 'outcome=CLEAN','outcome=PRESERVED')+@(
        'dynamic gate: injected=1 pending=1 recordedSlot=0 submitSlot=1 point=3 completed=2',
        'dynamic reuse: originalSlot=0 currentSlot=0 byte=0x5a gateReleased=1','dynamic release: signaled=1',
        'dynamic points: recording=1 submitting=1 unrelated=0')
    $fixtures+=@(
        @{Name='dynamic-valid';Case='delayed-dynamic-slot';Lines=$dynamic;Pass=$true},
        @{Name='dynamic-no-gate';Case='delayed-dynamic-slot';Lines=($dynamic | Where-Object {$_ -notlike 'dynamic gate:*'});Pass=$false},
        @{Name='dynamic-not-pending';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'pending=1','pending=0');Pass=$false},
        @{Name='dynamic-wrong-data';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'mismatches=0','mismatches=64');Pass=$false},
        @{Name='dynamic-no-release';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'signaled=1','signaled=0');Pass=$false},
        @{Name='dynamic-no-recorded-point';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'recording=1','recording=0');Pass=$false},
        @{Name='dynamic-unrelated-point';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'unrelated=0','unrelated=1');Pass=$false})
    $offset=@($dynamic -replace 'delayed-dynamic-slot','delayed-dynamic-offset')+@('dynamic offset: requested=64 bound=64')
    $fixtures+=@(
        @{Name='dynamic-same-slot';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'submitSlot=1','submitSlot=0');Pass=$false},
        @{Name='dynamic-completed-point';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'completed=2','completed=3');Pass=$false},
        @{Name='dynamic-wrong-slot-reuse';Case='delayed-dynamic-slot';Lines=($dynamic -replace 'currentSlot=0','currentSlot=2');Pass=$false},
        @{Name='offset-valid';Case='delayed-dynamic-offset';Lines=$offset;Pass=$true},
        @{Name='offset-missing';Case='delayed-dynamic-offset';Lines=($offset | Where-Object {$_ -notlike 'dynamic offset:*'});Pass=$false},
        @{Name='offset-wrong-binding';Case='delayed-dynamic-offset';Lines=($offset -replace 'bound=64','bound=0');Pass=$false})
    $modern=@($base -replace 'submit=legacy legacyCalls=1 submit2Calls=0','submit=submit2 legacyCalls=0 submit2Calls=1')
    $fixtures+=@(
        @{Name='encoder-modern-valid';Encoder='submit2';Lines=$modern;Pass=$true},
        @{Name='encoder-missing';Lines=@($base | Where-Object {$_ -notmatch '^descriptor encoder:'});Pass=$false},
        @{Name='encoder-duplicate';Lines=($base+@($base | Where-Object {$_ -match '^descriptor encoder:'}));Pass=$false},
        @{Name='encoder-wrong-route';Encoder='submit2';Lines=$base;Pass=$false},
        @{Name='encoder-legacy-not-submitted';Lines=($base -replace 'legacyCalls=1','legacyCalls=0');Pass=$false},
        @{Name='encoder-modern-not-submitted';Encoder='submit2';Lines=($modern -replace 'submit2Calls=1','submit2Calls=0');Pass=$false},
        @{Name='encoder-mixed-route';Encoder='submit2';Lines=($modern -replace 'legacyCalls=0','legacyCalls=1');Pass=$false})
    $fixtures+=@(
        @{Name='barrier-sync2-valid';Barrier='sync2';Lines=($base -replace 'sync2=0','sync2=1');Pass=$true},
        @{Name='barrier-missing';Lines=@($base | Where-Object {$_ -notmatch 'Vulkan paths:'});Pass=$false},
        @{Name='barrier-mismatch';Barrier='sync2';Lines=$base;Pass=$false},
        @{Name='barrier-duplicate';Lines=($base+$base[-1]);Pass=$false})
    $good=$true
    foreach ($fixture in $fixtures) {
        $case=if ($fixture.Case) {$fixture.Case} else {'control'}
        $result=Test-DescriptorLog -Lines @($fixture.Lines) -ExitCode $fixture.Code -CaseName $case -Templates ([bool]$fixture.Templates) -Encoder $(if ($fixture.Encoder) {$fixture.Encoder} else {'legacy'}) -BarrierEncoder $(if ($fixture.Barrier) {$fixture.Barrier} else {'legacy'})
        $ok=$result.Pass -eq $fixture.Pass
        Write-Host "SelfTest $($fixture.Name): $ok"
        if (-not $ok) {$good=$false;$result.Reasons | ForEach-Object {Write-Host "  $_"}}
    }
    exit $(if ($good) {0} else {1})
}
if ($Cases.Count -eq 0) { $Cases=$defaultCases } else { $Cases=@($Cases | Select-Object -Unique) }
foreach ($case in $Cases) { if ($allCases -notcontains $case) { Write-Error "unknown case: $case"; exit 1 } }
$variables = @('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_COMPLETION','CC_VULKAN_FORCE_API','CC_VULKAN_DISABLE_FEATURES','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER')
$previous = @{}
foreach ($variable in $variables) { $previous[$variable]=[Environment]::GetEnvironmentVariable($variable,'Process') }
$locationPushed=$false
try {
    & cmake --build $BuildDir --config Release --target vulkan_descriptor_lifetime_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'descriptor build failed' }
    $exe = Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_descriptor_lifetime_smoke.exe'
    $env:VK_LAYER_PATH='D:\Software\VulkanSDK\1.4.341.1\Bin'
    $empty=Join-Path $outputRoot 'empty-implicit-layers'
    New-Item -ItemType Directory -Force -Path $empty | Out-Null
    $env:VK_IMPLICIT_LAYER_PATH=$empty
    $env:PATH=(Join-Path $repo 'native\external\win64\libs') + ';' + (Join-Path $repo 'native\external\win64\libs\v8\Release') + ';' + $env:PATH
    $env:CC_VULKAN_COMPLETION=if ($Completion -eq 'auto') {$null} else {$Completion}
    $env:CC_VULKAN_FORCE_API=if ($ApiVersion) {$ApiVersion} else {$null}
    $env:CC_VULKAN_DISABLE_FEATURES=$null
    $env:CC_VULKAN_SUBMIT=$Submit
    $env:CC_VULKAN_BARRIER=$Barrier
    Push-Location $outputRoot; $locationPushed=$true
    $good=$true
    foreach ($case in $Cases) {
        $log=Join-Path $outputRoot ("descriptor-{0}-{1}-v{2}-tpl{5}-enc{6}-bar{7}-{3:yyyyMMdd-HHmmss-fff}-{4}.log" -f $case,$Completion,$ApiVersion,(Get-Date),$PID,[int][bool]$TemplateUpdates,$Submit,$Barrier)
        $nativeFlags=if ($TemplateUpdates) {' --template'} else {''}
        cmd /c """$exe"" --case ""$case""$nativeFlags > ""$log"" 2>&1"
        $code=$LASTEXITCODE
        $lines=@([System.IO.File]::ReadAllLines($log))
        $result=Test-DescriptorLog -Lines $lines -ExitCode $code -CaseName $case -Backend $Completion -Effective $ApiVersion -Templates ([bool]$TemplateUpdates) -Encoder $Submit -BarrierEncoder $Barrier
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
