# Finite clear experiment. Each result passes the shared main/phase/present judge.
[CmdletBinding()]
param(
    [ValidateSet('on','off','both')][string]$Validation='both',
    [ValidateRange(1000,100000)][int]$Frames=1000,
    [ValidateRange(300,100000)][int]$Warmup=300,
    [ValidateRange(5,20)][int]$Repeats=5,
    [switch]$SelfTest,
    [switch]$CompletionProfile,
    [switch]$WaitProfile,
    [switch]$PairedBackends,
    [switch]$SameBackendControls,
    [ValidateRange(10,20)][int]$Pairs=10
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$Validation=$Validation.ToLowerInvariant()
if($WaitProfile -and -not $CompletionProfile){throw 'WaitProfile requires CompletionProfile'}
if($SameBackendControls -and -not $PairedBackends){throw 'SameBackendControls requires PairedBackends'}
if($PairedBackends){
    if(-not $CompletionProfile){throw 'PairedBackends requires CompletionProfile'}
    if($PSBoundParameters.ContainsKey('Validation') -and $Validation -ne 'off'){throw 'PairedBackends requires validation off'}
    if($PSBoundParameters.ContainsKey('Repeats')){throw 'PairedBackends uses Pairs, not Repeats'}
    if($Pairs%2){throw 'Pairs must be even for equal first-position exposure'}
    $Validation='off'
}
if(-not $PairedBackends -and $PSBoundParameters.ContainsKey('Pairs')){throw 'Pairs requires PairedBackends'}
. "$PSScriptRoot/paired-completion-plan.ps1"
$metricNames=@('wall_ms','cpu_ms','wait_calls','wait_ms','fence_objects','semaphore_objects','gpu_clear_ms','in_flight')
$culture=[Globalization.CultureInfo]::InvariantCulture

function Import-PresentProfileJudge {
    if($WaitProfile){
        . "$PSScriptRoot/wait-phase-judge.ps1"
        . "$PSScriptRoot/wait-phase-judge-selftest.ps1"
        foreach($name in @('Convert-WaitPhaseUInt','Convert-WaitPhaseNumber','Test-WaitPhaseSidecar','Invoke-WaitPhaseJudgeSelfTest')){
            Set-Item "Function:script:$name" (Get-Item "Function:$name").ScriptBlock
        }
    }
    if($CompletionProfile){. "$PSScriptRoot/completion-profile-judge.ps1"
        # Dot-sourced functions need script scope after this importer returns.
        foreach($name in @('Convert-CompletionUInt','Convert-CompletionNumber','Test-CompletionSidecar')){
            Set-Item "Function:script:$name" (Get-Item "Function:$name").ScriptBlock
        }
    }
    # Created blocks have no source-file PSScriptRoot. Resolve both source paths here.
    $sources=@(
        @{File='verify-present-profile.ps1';Names=@('Convert-ProfileUInt','Convert-ProfileNumber','Test-ProfileMarker','Test-PresentProfileEvidence')},
        @{File='verify-completion-benchmark.ps1';Names=@('Test-BenchmarkResult','Test-BenchmarkPhases')}
    )
    foreach($source in $sources){
        $tokens=$null;$errors=$null
        $ast=[System.Management.Automation.Language.Parser]::ParseFile(
            (Join-Path $PSScriptRoot $source.File),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw 'Shared verifier parse errors'}
        foreach($name in $source.Names){
            $nodes=@($ast.FindAll({param($n)
                $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name
            },$true))
            if($nodes.Count -ne 1){throw "Missing/duplicate shared function: $name"}
            $body=$nodes[0].Body.Extent.Text
            Set-Item -Path "Function:script:$name" -Value ([scriptblock]::Create($body.Substring(1,$body.Length-2)))
        }
    }
}
function New-PresentExecutionPlan {
    param([string]$Mode,[int]$Count)
    for($repeat=1;$repeat -le $Count;$repeat++){
        $vals=@('on','off');$backends=@('fence','timeline');$profiles=@('off','on')
        if($repeat%2 -eq 0){[array]::Reverse($vals);[array]::Reverse($backends);[array]::Reverse($profiles)}
        foreach($v in $vals){
            if($Mode -ne 'both' -and $Mode -ne $v){continue}
            foreach($b in $backends){foreach($p in $profiles){
                [pscustomobject]@{Case="$v-$b-$p-rep$repeat";Validation=$v;Backend=$b;Profile=$p;Repeat=$repeat}
            }}
        }
    }
}
function Test-PresentRunComplete {
    param([object[]]$Plan,[object[]]$Manifest,[object[]]$Judgments,[object[]]$Builds)
    if(-not $Plan -or $Manifest.Count -ne $Plan.Count -or $Judgments.Count -ne $Plan.Count){return $false}
    $seen=@{}
    for($i=0;$i -lt $Plan.Count;$i++){
        $m=$Manifest[$i];$j=$Judgments[$i]
        if($m.Case -cne $Plan[$i].Case -or $j.Case -cne $Plan[$i].Case -or
           $seen.ContainsKey($m.Case) -or ($m.Exit -isnot [int] -and $m.Exit -isnot [long]) -or
           $m.Exit -ne 0 -or $j.Pass -ne $true){return $false}
        $seen[$m.Case]=$true
    }
    $needed=@($Plan|Select-Object -ExpandProperty Validation -Unique)
    if($Builds.Count -ne $needed.Count){return $false}
    foreach($v in $needed){
        $found=@($Builds|Where-Object{$_.Validation -ceq $v})
        if($found.Count -ne 1 -or ($found[0].Exit -isnot [int] -and $found[0].Exit -isnot [long]) -or $found[0].Exit -ne 0){return $false}
    }
    return $true
}
function Invoke-PresentProcess {
    param([string]$File,[string[]]$Arguments,[string]$Directory,[string]$Stem)
    # Generated Windows paths/enums/integers only; no embedded quote or trailing slash.
    $quoted=@();foreach($arg in $Arguments){
        if($arg.Contains('"') -or $arg.EndsWith('\')){throw 'Unsupported process argument'}
        $quoted+='"'+$arg+'"'
    }
    $stdout="$Stem.stdout.txt";$stderr="$Stem.stderr.txt";$log="$Stem.log"
    $process=$null
    try{
        $startedUtc=[DateTimeOffset]::UtcNow.ToString('O')
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $process=Start-Process -FilePath $File -ArgumentList ($quoted -join ' ') -WorkingDirectory $Directory -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru
        $timer.Stop()
        $endedUtc=[DateTimeOffset]::UtcNow.ToString('O')
        $code=[int]$process.ExitCode
        # Preserve both original streams. Errors are checked regardless of order.
        $lines=@(Get-Content -LiteralPath $stdout -Encoding UTF8)+@(Get-Content -LiteralPath $stderr -Encoding UTF8)
        Set-Content -LiteralPath $log -Value $lines -Encoding UTF8
        [pscustomobject]@{Code=$code;Log=$log;StartedUtc=$startedUtc;EndedUtc=$endedUtc;ProcessWaitMs=$timer.Elapsed.TotalMilliseconds}
    }finally{if($process){$process.Dispose()}}
}
function Get-PresentCaseJudge {
    param([object]$Case,[int]$ExpectedFrames,[int]$ExpectedWarmup)
    foreach($file in @($Case.Log,$Case.Csv,($Case.Csv+'.phases.csv'),($Case.Csv+'.present.csv'))){
        if(-not (Test-Path -LiteralPath $file -PathType Leaf)){
            return [pscustomobject]@{Pass=$false;Reasons=@("missing file: $file")}
        }
    }
    $old=Test-PresentProfileEvidence -LogLines @(Get-Content -LiteralPath $Case.Log) -MainLines @(Get-Content -LiteralPath $Case.Csv) -PhaseLines @(Get-Content -LiteralPath ($Case.Csv+'.phases.csv')) -PresentLines @(Get-Content -LiteralPath ($Case.Csv+'.present.csv')) -Code $Case.Exit -Validation $Case.Validation -Backend $Case.Backend -Profile $Case.Profile -Frames $ExpectedFrames -Warmup $ExpectedWarmup -WaitProfile:$WaitProfile
    if(-not $CompletionProfile){return $old}
    if(-not (Test-Path -LiteralPath ($Case.Csv+'.completion.csv') -PathType Leaf)){
        return [pscustomobject]@{Pass=$false;Reasons=@($old.Reasons)+@('missing completion sidecar')}
    }
    $extra=Test-CompletionSidecar -CompletionLines @(Get-Content -LiteralPath ($Case.Csv+'.completion.csv')) -PresentLines @(Get-Content -LiteralPath ($Case.Csv+'.present.csv')) -LogLines @(Get-Content -LiteralPath $Case.Log) -Frames $ExpectedFrames -Profile $Case.Profile
    $pass=$old.Pass -and $extra.Pass
    $why=@($old.Reasons)+@($extra.Reasons)
    if($WaitProfile){
        if(-not (Test-Path -LiteralPath ($Case.Csv+'.wait-phases.csv') -PathType Leaf)){
            return [pscustomobject]@{Pass=$false;Reasons=$why+@('missing wait-phase sidecar')}
        }
        $wait=Test-WaitPhaseSidecar -WaitLines @(Get-Content -LiteralPath ($Case.Csv+'.wait-phases.csv')) -MainLines @(Get-Content -LiteralPath $Case.Csv) -PresentLines @(Get-Content -LiteralPath ($Case.Csv+'.present.csv')) -LogLines @(Get-Content -LiteralPath $Case.Log) -Frames $ExpectedFrames -Profile $Case.Profile
        $pass=$pass -and $wait.Pass;$why+=@($wait.Reasons)
    }
    [pscustomobject]@{Pass=$pass;Reasons=$why}
}
function Invoke-PresentRunnerSelfTest {
    $plan=@(New-PresentExecutionPlan both 5)
    $manifest=@($plan|ForEach-Object{[pscustomobject]@{Case=$_.Case;Exit=0}})
    $judges=@($plan|ForEach-Object{[pscustomobject]@{Case=$_.Case;Pass=$true}})
    $builds=@([pscustomobject]@{Validation='on';Exit=0},[pscustomobject]@{Validation='off';Exit=0})
    $ok=@()
    $ok+=($plan.Count -eq 40 -and @($plan.Case|Select-Object -Unique).Count -eq 40)
    $ok+=($plan[0].Backend -eq 'fence' -and $plan[0].Profile -eq 'off' -and $plan[8].Backend -eq 'timeline' -and $plan[8].Profile -eq 'on')
    $ok+=(@(New-PresentExecutionPlan on 5).Count -eq 20 -and @(New-PresentExecutionPlan off 5).Count -eq 20)
    $ok+=(Test-PresentRunComplete $plan $manifest $judges $builds)
    $ok+=(-not (Test-PresentRunComplete $plan @($manifest[0..38]) $judges $builds))
    $ok+=(-not (Test-PresentRunComplete $plan $manifest @($judges[0..38]) $builds))
    $manifest[0].Exit=-1073741819
    $ok+=(-not (Test-PresentRunComplete $plan $manifest $judges $builds));$manifest[0].Exit=0
    $judges[0].Pass=$false
    $ok+=(-not (Test-PresentRunComplete $plan $manifest $judges $builds));$judges[0].Pass=$true
    $manifest[1].Case=$manifest[0].Case
    $ok+=(-not (Test-PresentRunComplete $plan $manifest $judges $builds));$manifest[1].Case=$plan[1].Case
    $builds[0].Exit=1
    $ok+=(-not (Test-PresentRunComplete $plan $manifest $judges $builds));$builds[0].Exit=0
    $ok+=(-not (Test-PresentRunComplete $plan $manifest $judges @($builds[0])))
    if($CompletionProfile){
        # Isolate the new opt-in boundary with an already-passing old verdict.
        # The full old judge is independently exercised by offline replay.
        $oldBody=(Get-Item Function:Test-PresentProfileEvidence).ScriptBlock
        $savedWaitProfile=$WaitProfile;$WaitProfile=$false
        $probeRoot=Join-Path $repo ('native/build/vulkan-modernization/completion-runner-selftest-'+(Get-Date -Format 'yyyyMMdd-HHmmssfff')+'-'+$PID)
        New-Item -ItemType Directory -Path $probeRoot|Out-Null
        try{
            Set-Item Function:script:Test-PresentProfileEvidence { [pscustomobject]@{Pass=$true;Reasons=@()} }
            $csv=Join-Path $probeRoot 'frames.csv';$log=Join-Path $probeRoot 'log.txt'
            'placeholder'|Set-Content $csv
            'placeholder'|Set-Content ($csv+'.phases.csv')
            @('frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms','0,off,1,1,0,0,0,NA,NA,NA')|Set-Content ($csv+'.present.csv')
            'completion-profile-dispatch: whole-run fenceStatus=0 counter=0 resetFence=0'|Set-Content $log
            $case=[pscustomobject]@{Log=$log;Csv=$csv;Exit=0;Validation='off';Backend='fence';Profile='off'}
            $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
            $ch='frame,profile,fence_status_calls,fence_not_ready,counter_calls,reset_fence_calls,query_failures,completion_api_ms,host_other_ms'
            @($ch,'0,off,0,0,0,0,0,NA,NA')|Set-Content ($csv+'.completion.csv')
            $ok+=(Get-PresentCaseJudge $case 1 0).Pass
            @($ch,'0,off,0,0,0,0,1,NA,NA')|Set-Content ($csv+'.completion.csv')
            $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
            $CompletionProfile=$false
            $ok+=(Get-PresentCaseJudge $case 1 0).Pass
        }finally{
            $CompletionProfile=$true
            $WaitProfile=$savedWaitProfile
            Set-Item Function:script:Test-PresentProfileEvidence $oldBody
        }
    }
    $bad=@($ok|Where-Object{$_ -ne $true})
    Write-Host "Runner SelfTest executed=$($ok.Count) passed=$($ok.Count-$bad.Count); no build/GPU"
    if($bad.Count){throw 'Runner control-flow SelfTest failed'}
}
function Invoke-WaitRunnerBoundarySelfTest {
    # Control-flow probes isolate old verdicts; the actual new judge remains installed.
    $oldBody=(Get-Item Function:Test-PresentProfileEvidence).ScriptBlock
    $completionBody=(Get-Item Function:Test-CompletionSidecar).ScriptBlock
    $root=Join-Path $repo ('native/build/vulkan-modernization/wait-runner-selftest-'+(Get-Date -Format 'yyyyMMdd-HHmmssfff')+'-'+$PID+'-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root|Out-Null
    $ok=@()
    try{
        Set-Item Function:script:Test-PresentProfileEvidence {[pscustomobject]@{Pass=$true;Reasons=@()}}
        Set-Item Function:script:Test-CompletionSidecar {[pscustomobject]@{Pass=$true;Reasons=@()}}
        $csv=Join-Path $root 'frames.csv';$log=Join-Path $root 'native.log'
        @('frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight','0,1,0,0,0,1,1,0.01,1')|Set-Content $csv
        'old verdict fixture'|Set-Content ($csv+'.phases.csv')
        'old verdict fixture'|Set-Content ($csv+'.completion.csv')
        @('frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms','0,off,1,1,0,0,0,NA,NA,NA')|Set-Content ($csv+'.present.csv')
        'benchmark wait-profile: mode=on fault=none capacity=32'|Set-Content $log
        $case=[pscustomobject]@{Log=$log;Csv=$csv;Exit=0;Validation='off';Backend='fence';Profile='off'}
        $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
        $names=@('frame')
        foreach($phase in @('unknown','acquire','record','submit','observe','present')){
            foreach($suffix in @('fence_calls','semaphore_calls','failures','wait_ms')){$names+=$phase+'_'+$suffix}
        }
        $names+=@('acquire_calls','acquire_successes','acquire_present_fence_calls','acquire_other_fence_calls','acquire_present_fence_ms','identity_failures','invalid','total_wait_calls','total_wait_ms')
        $row=New-Object 'string[]' 34;for($i=0;$i -lt 34;$i++){$row[$i]='0'};$row[25]='1';$row[26]='1'
        @(($names -join ','),($row -join ','))|Set-Content ($csv+'.wait-phases.csv')
        $ok+=(Get-PresentCaseJudge $case 1 0).Pass
        Set-Item Function:script:Test-PresentProfileEvidence {[pscustomobject]@{Pass=$false;Reasons=@('old failure sentinel')}}
        $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
        Set-Item Function:script:Test-PresentProfileEvidence {[pscustomobject]@{Pass=$true;Reasons=@()}}
        Set-Item Function:script:Test-CompletionSidecar {[pscustomobject]@{Pass=$false;Reasons=@('completion failure sentinel')}}
        $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
        Set-Item Function:script:Test-CompletionSidecar {[pscustomobject]@{Pass=$true;Reasons=@()}}
        $row[30]='1';@(($names -join ','),($row -join ','))|Set-Content ($csv+'.wait-phases.csv')
        $ok+=(-not (Get-PresentCaseJudge $case 1 0).Pass)
        $WaitProfile=$false
        $ok+=(Get-PresentCaseJudge $case 1 0).Pass
    }finally{
        Set-Item Function:script:Test-PresentProfileEvidence $oldBody
        Set-Item Function:script:Test-CompletionSidecar $completionBody
    }
    $bad=@($ok|Where-Object{$_ -ne $true})
    Write-Host "Wait runner boundary SelfTest executed=$($ok.Count) passed=$($ok.Count-$bad.Count); no build/GPU"
    if($bad.Count){throw 'Wait runner boundary SelfTest failed'}
}
$runRoot=$null;$saved=@{};$oldCwd=(Get-Location).Path
$envNames=@('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_FORCE_API','CC_VULKAN_COMPLETION','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER','CC_VULKAN_RENDERING','CC_VULKAN_DISABLE_FEATURES')
$manifest=New-Object 'System.Collections.Generic.List[object]'
$judgments=New-Object 'System.Collections.Generic.List[object]'
$builds=New-Object 'System.Collections.Generic.List[object]'
$reasons=New-Object 'System.Collections.Generic.List[string]'
$restoreOk=$true;$complete=$false;$sourceHash=$null;$recorderHash=$null
try{
    Import-PresentProfileJudge
    if($SelfTest){
        Invoke-PresentRunnerSelfTest
        if($PairedBackends){Invoke-BackendPairSelfTest}
        if($WaitProfile){Invoke-WaitPhaseJudgeSelfTest;Invoke-WaitRunnerBoundarySelfTest}
        exit 0
    }
    $sourceHash=(Get-FileHash -LiteralPath "$PSScriptRoot/completion_benchmark.h" -Algorithm SHA256).Hash
    if($WaitProfile){$recorderHash=(Get-FileHash -LiteralPath "$PSScriptRoot/benchmark_wait_phase.h" -Algorithm SHA256).Hash}
    $plan=if($PairedBackends){@(New-BackendPairPlan $Pairs -SameBackendControls:$SameBackendControls)}else{@(New-PresentExecutionPlan $Validation $Repeats)}
    foreach($name in $envNames){
        $item=Get-Item ("Env:"+$name) -ErrorAction SilentlyContinue
        $saved[$name]=[pscustomobject]@{Exists=($null -ne $item);Value=if($null -ne $item){$item.Value}else{$null}}
    }
    $family=if($SameBackendControls){'same-backend-control-matrix'}elseif($PairedBackends){'paired-completion-matrix'}elseif($CompletionProfile){'completion-profile-matrix'}else{'present-profile-matrix'}
    if($WaitProfile){$family=if($SameBackendControls){'same-backend-wait-phase-matrix'}elseif($PairedBackends){'paired-wait-phase-matrix'}else{'wait-phase-matrix'}}
    $base=Join-Path $repo ('native/build/vulkan-modernization/'+$family)
    if(-not (Test-Path -LiteralPath $base)){New-Item -ItemType Directory -Path $base|Out-Null}
    $runRoot=Join-Path $base ('run-'+(Get-Date -Format 'yyyyMMdd-HHmmssfff')+'-'+$PID+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $runRoot|Out-Null
    $plan|ConvertTo-Json -Depth 4|Set-Content "$runRoot/plan.json" -Encoding UTF8
    Write-Host "run root: $runRoot"
    $cmake=(Get-Command cmake -ErrorAction Stop).Source
    foreach($val in @($plan|Select-Object -ExpandProperty Validation -Unique)){
        $tree=Join-Path $repo ('native/build/vulkan-modernization/'+$(if($val -eq 'on'){'win64-simulator'}else{'win64-benchmark-off'}))
        $built=Invoke-PresentProcess -File $cmake -Arguments @('--build',$tree,'--config','Release','--target','vulkan_wsi_smoke','--parallel','4') -Directory $repo -Stem "$runRoot/build-$val"
        $builds.Add([pscustomobject]@{Validation=$val;Tree=$tree;Exit=$built.Code;Log=$built.Log})
        Write-Host "build $val exit=$($built.Code)"
        if($built.Code -ne 0){throw "Current-source $val build failed; no old executable accepted"}
    }
    $implicit=Join-Path $runRoot 'empty-implicit';New-Item -ItemType Directory -Path $implicit|Out-Null
    $env:VK_LAYER_PATH='D:/Software/VulkanSDK/1.4.341.1/Bin'
    $env:VK_IMPLICIT_LAYER_PATH=$implicit
    $env:PATH=(Join-Path $repo 'native/external/win64/libs')+';'+(Join-Path $repo 'native/external/win64/libs/v8/Release')+';'+$env:PATH
    $env:CC_VULKAN_FORCE_API='1.3';$env:CC_VULKAN_SUBMIT='legacy';$env:CC_VULKAN_BARRIER='legacy'
    Remove-Item Env:CC_VULKAN_RENDERING -ErrorAction SilentlyContinue
    Remove-Item Env:CC_VULKAN_DISABLE_FEATURES -ErrorAction SilentlyContinue
    foreach($case in $plan){
        $cd=Join-Path $runRoot $case.Case;New-Item -ItemType Directory -Path $cd|Out-Null
        $csv=Join-Path $cd 'frames.csv'
        $env:CC_VULKAN_COMPLETION=$case.Backend
        $tree=Join-Path $repo ('native/build/vulkan-modernization/'+$(if($case.Validation -eq 'on'){'win64-simulator'}else{'win64-benchmark-off'}))
        $exe=Join-Path $tree 'vulkan-engine-smoke/Release/vulkan_wsi_smoke.exe'
        $entry=[pscustomobject]@{Case=$case.Case;Validation=$case.Validation;Backend=$case.Backend;Profile=$case.Profile;Repeat=$case.Repeat;Frames=$Frames;Warmup=$Warmup;Exit=$null;Log="$cd/native.log";Csv=$csv}
        if($PairedBackends){
            foreach($field in @('Pair','OrderIndex','Position','Direction')){$entry|Add-Member $field $case.$field}
            $entry|Add-Member BuildTree 'win64-benchmark-off'
            $entry|Add-Member StartedUtc $null
            $entry|Add-Member EndedUtc $null
            $entry|Add-Member ProcessWaitMs $null
        }
        try{
            $nativeArgs=@('--benchmark','--csv',$csv,'--validation',$case.Validation,'--frames',[string]$Frames,'--warmup',[string]$Warmup,'--present-profile',$case.Profile)
            if($WaitProfile){$nativeArgs+=@('--wait-profile','on')}
            $native=Invoke-PresentProcess -File $exe -Arguments $nativeArgs -Directory $cd -Stem "$cd/native"
            $entry.Exit=$native.Code;$entry.Log=$native.Log
            if($PairedBackends){
                $entry.StartedUtc=$native.StartedUtc;$entry.EndedUtc=$native.EndedUtc;$entry.ProcessWaitMs=$native.ProcessWaitMs
            }
            $verdict=Get-PresentCaseJudge $entry $Frames $Warmup
        }catch{$verdict=[pscustomobject]@{Pass=$false;Reasons=@($_.Exception.Message)}}
        $manifest.Add($entry)
        $judgments.Add([pscustomobject]@{Case=$case.Case;Pass=$verdict.Pass;Reasons=@($verdict.Reasons)})
        if(-not $verdict.Pass){$reasons.Add("case failed: $($case.Case)")}
        $manifest.ToArray()|ConvertTo-Json -Depth 5|Set-Content "$runRoot/manifest.json" -Encoding UTF8
        $judgments.ToArray()|ConvertTo-Json -Depth 5|Set-Content "$runRoot/judge.json" -Encoding UTF8
        Write-Host "[$($manifest.Count)/$($plan.Count)] $($case.Case) native=$($entry.Exit) judge=$($verdict.Pass)"
        if(-not $verdict.Pass){foreach($why in $verdict.Reasons){Write-Host "  FAIL: $why"}}
    }
    $complete=Test-PresentRunComplete $plan $manifest.ToArray() $judgments.ToArray() $builds.ToArray()
    if($PairedBackends){$complete=$complete -and (Test-BackendPairRun $plan $manifest.ToArray() $Pairs $Frames $Warmup -SameBackendControls:$SameBackendControls)}
    if(-not $complete){$reasons.Add('Matrix count/config/build/native/judge completeness failed')}
}catch{$reasons.Add($_.Exception.Message);Write-Host "FAIL: $($_.Exception.Message)"}
finally{
    foreach($name in $saved.Keys){
        try{
            if($saved[$name].Exists){Set-Item ("Env:"+$name) $saved[$name].Value}
            else{Remove-Item ("Env:"+$name) -ErrorAction SilentlyContinue}
            $actual=Get-Item ("Env:"+$name) -ErrorAction SilentlyContinue
            if(($null -ne $actual) -ne $saved[$name].Exists -or
               ($saved[$name].Exists -and $actual.Value -cne $saved[$name].Value)){$restoreOk=$false}
        }catch{$restoreOk=$false}
    }
    try{Set-Location -LiteralPath $oldCwd}catch{$restoreOk=$false}
    if(-not $restoreOk){$reasons.Add('environment/cwd restore failed')}
    if($runRoot){
        $sourceUnchanged=(Get-FileHash -LiteralPath "$PSScriptRoot/completion_benchmark.h" -Algorithm SHA256).Hash -ceq $sourceHash
        if($WaitProfile){$sourceUnchanged=$sourceUnchanged -and ((Get-FileHash -LiteralPath "$PSScriptRoot/benchmark_wait_phase.h" -Algorithm SHA256).Hash -ceq $recorderHash)}
        if(-not $sourceUnchanged){$reasons.Add('benchmark source changed during matrix')}
        $status=[pscustomobject]@{Complete=$complete;Pass=($complete -and $reasons.Count -eq 0);Expected=$plan.Count;Attempted=$manifest.Count;NativeOk=@($manifest|Where-Object{$null -ne $_.Exit -and $_.Exit -eq 0}).Count;JudgePassed=@($judgments|Where-Object{$_.Pass}).Count;EnvRestored=$restoreOk;Frames=$Frames;Warmup=$Warmup;Repeats=$Repeats;CompletionProfile=[bool]$CompletionProfile;SourceSha256=$sourceHash;SourceUnchanged=$sourceUnchanged;Builds=@($builds.ToArray());Reasons=@($reasons.ToArray())}
        $status|Add-Member PairedBackends ([bool]$PairedBackends)
        $status|Add-Member WaitProfile ([bool]$WaitProfile)
        if($WaitProfile){$status|Add-Member RecorderSha256 $recorderHash}
        $status|Add-Member SameBackendControls ([bool]$SameBackendControls)
        $status|Add-Member Pairs $(if($PairedBackends){$Pairs}else{$null})
        if($PairedBackends){$status.Repeats=$Pairs}
        $status|ConvertTo-Json -Depth 6|Set-Content "$runRoot/run-status.json" -Encoding UTF8
        Write-Host "Matrix attempted=$($status.Attempted) nativeOk=$($status.NativeOk) judgePassed=$($status.JudgePassed) complete=$complete envRestored=$restoreOk"
    }
}
if(-not $complete -or $reasons.Count){exit 1}
exit 0
