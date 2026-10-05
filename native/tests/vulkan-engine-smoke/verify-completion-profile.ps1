# Offline replay; never builds or runs GPU code.
[CmdletBinding()]
param([switch]$SelfTest,[string]$EvidenceRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$metricNames=@('wall_ms','cpu_ms','wait_calls','wait_ms','fence_objects','semaphore_objects','gpu_clear_ms','in_flight')
$culture=[Globalization.CultureInfo]::InvariantCulture
. "$PSScriptRoot/completion-profile-judge.ps1"

function Import-CompletionProfileDependencies {
    foreach($source in @(
        @{File='verify-present-profile.ps1';Names=@('Convert-ProfileUInt','Convert-ProfileNumber','Test-ProfileMarker','Test-PresentProfileEvidence')},
        @{File='verify-completion-benchmark.ps1';Names=@('Test-BenchmarkResult','Test-BenchmarkPhases')}
    )){
        $tokens=$null;$errors=$null
        $ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $source.File),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw 'Shared verifier parse errors'}
        foreach($name in $source.Names){
            $nodes=@($ast.FindAll({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
            if($nodes.Count -ne 1){throw "Missing/duplicate shared function: $name"}
            $body=$nodes[0].Body.Extent.Text
            Set-Item "Function:script:$name" ([scriptblock]::Create($body.Substring(1,$body.Length-2)))
        }
    }
}
function New-CompletionReportRoot {
    $base=Join-Path $repo 'native/build/vulkan-modernization/completion-profile-checks'
    if(-not (Test-Path -LiteralPath $base)){New-Item -ItemType Directory -Path $base|Out-Null}
    $path=Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmssfff')+'-'+$PID+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $path -ErrorAction Stop|Out-Null
    $path
}
function Test-CompletionEvidenceCase {
    param([object]$Case)
    $why=New-Object 'System.Collections.Generic.List[string]'
    try{
        foreach($file in @($Case.Log,$Case.Csv,($Case.Csv+'.phases.csv'),($Case.Csv+'.present.csv'),($Case.Csv+'.completion.csv'))){
            if([string]::IsNullOrWhiteSpace($file) -or -not (Test-Path -LiteralPath $file -PathType Leaf)){$why.Add("missing evidence: $file")}
        }
        if($why.Count){return [pscustomobject]@{Pass=$false;Reasons=@($why.ToArray())}}
        $logs=@(Get-Content -LiteralPath $Case.Log -Encoding UTF8)
        $present=@(Get-Content -LiteralPath ($Case.Csv+'.present.csv') -Encoding UTF8)
        $old=Test-PresentProfileEvidence -LogLines $logs -MainLines @(Get-Content -LiteralPath $Case.Csv -Encoding UTF8) -PhaseLines @(Get-Content -LiteralPath ($Case.Csv+'.phases.csv') -Encoding UTF8) -PresentLines $present -Code $Case.Exit -Validation $Case.Validation -Backend $Case.Backend -Profile $Case.Profile -Frames $Case.Frames -Warmup $Case.Warmup
        $extra=Test-CompletionSidecar -CompletionLines @(Get-Content -LiteralPath ($Case.Csv+'.completion.csv') -Encoding UTF8) -PresentLines $present -LogLines $logs -Frames $Case.Frames -Profile $Case.Profile
        foreach($r in @($old.Reasons)+@($extra.Reasons)){$why.Add($r)}
        if(-not $old.Pass -or -not $extra.Pass){$why.Add('shared evidence judge rejected')}
    }catch{$why.Add("invalid evidence case: $($_.Exception.Message)")}
    [pscustomobject]@{Pass=($why.Count -eq 0);Reasons=@($why.ToArray())}
}
function Invoke-CompletionSelfTest {
    param([string]$Report)
    # Literal first two rows from Lead's current-source short run (2026-10-04).
    $ch='frame,profile,fence_status_calls,fence_not_ready,counter_calls,reset_fence_calls,query_failures,completion_api_ms,host_other_ms'
    $ph='frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms'
    $on=@{C=@($ch,'0,on,1,0,0,1,0,0.195600000,0.015200000','1,on,1,0,0,1,0,0.178500000,0.018500000');
        P=@($ph,'0,on,1,1,1,0,0,0.386700000,0.009400000,0.210800000','1,on,1,1,1,0,0,0.982700000,0.024500000,0.197000000');
        L=@('completion-profile-dispatch: whole-run fenceStatus=100 counter=0 resetFence=98');Frames=2;Profile='on'}
    $off=@{C=@($ch,'0,off,1,0,0,1,0,NA,NA','1,off,1,0,0,1,0,NA,NA');
        P=@($ph,'0,off,1,1,1,0,0,NA,NA,NA','1,off,1,1,1,0,0,NA,NA,NA');
        L=@('completion-profile-dispatch: whole-run fenceStatus=104 counter=0 resetFence=98');Frames=2;Profile='off'}
    $timeline=@{C=@($ch,'0,on,0,0,1,0,0,0.076000000,0.006100000','1,on,0,0,1,0,0,0.076300000,0.008400000');
        P=@($ph,'0,on,1,1,0,0,0,0.360700000,0.000000000,0.082100000','1,on,1,1,0,0,0,0.334400000,0.000000000,0.084700000');
        L=@('completion-profile-dispatch: whole-run fenceStatus=0 counter=100 resetFence=47');Frames=2;Profile='on'}
    $fixtures=New-Object 'System.Collections.Generic.List[object]'
    function Add-CompletionFixture {
        param([string]$Name,[hashtable]$Base,[scriptblock]$Change,[bool]$Want=$false)
        $copy=@{C=@($Base.C.Clone());P=@($Base.P.Clone());L=@($Base.L.Clone());Frames=$Base.Frames;Profile=$Base.Profile}
        if($Change){& $Change $copy}
        $fixtures.Add([pscustomobject]@{Name=$Name;Input=$copy;Want=$Want})
    }
    function Set-CompletionField {
        param([hashtable]$Data,[string]$Group,[int]$Column,[string]$Value,[int]$Row=1)
        $f=$Data[$Group][$Row].Split(',');$f[$Column]=$Value;$Data[$Group][$Row]=$f -join ','
    }
    Add-CompletionFixture valid-on $on $null $true
    Add-CompletionFixture valid-off $off $null $true
    Add-CompletionFixture valid-timeline $timeline $null $true
    Add-CompletionFixture legal-notready $off {param($x) Set-CompletionField $x C 3 '1'} $true
    Add-CompletionFixture empty-log $on {param($x) $x.L=@()}
    Add-CompletionFixture empty-completion $on {param($x) $x.C=@()}
    Add-CompletionFixture empty-present $on {param($x) $x.P=@()}
    Add-CompletionFixture frames-zero $on {param($x) $x.Frames=0}
    Add-CompletionFixture frames-over-limit $on {param($x) $x.Frames=100001}
    Add-CompletionFixture frames-string $on {param($x) $x.Frames='2'}
    Add-CompletionFixture unknown-profile $on {param($x) $x.Profile='bogus'}
    Add-CompletionFixture profile-case $on {param($x) $x.Profile='ON'}
    foreach($group in @('C','P')){
        Add-CompletionFixture "$group-missing-header" $on ([scriptblock]::Create("param(`$x) `$x.$group=@(`$x.${group}[1..2])"))
        Add-CompletionFixture "$group-header-only" $on ([scriptblock]::Create("param(`$x) `$x.$group=@(`$x.${group}[0])"))
        Add-CompletionFixture "$group-blank-row" $on ([scriptblock]::Create("param(`$x) `$x.${group}[1]=''"))
        Add-CompletionFixture "$group-extra-column" $on ([scriptblock]::Create("param(`$x) `$x.${group}[1]+=',junk'"))
        Add-CompletionFixture "$group-missing-column" $on ([scriptblock]::Create("param(`$x) `$f=`$x.${group}[1].Split(','); `$x.${group}[1]=`$f[0..(`$f.Count-2)] -join ','"))
        Add-CompletionFixture "$group-frame-wrong" $on ([scriptblock]::Create("param(`$x) Set-CompletionField `$x $group 0 '1'"))
        Add-CompletionFixture "$group-profile-wrong" $on ([scriptblock]::Create("param(`$x) Set-CompletionField `$x $group 1 'off'"))
    }
    foreach($value in @('01','+1','-1',' 1','1 ','garbage','18446744073709551616')){
        Add-CompletionFixture ('integer-'+$fixtures.Count) $on ([scriptblock]::Create("param(`$x) Set-CompletionField `$x C 2 '$value'"))
    }
    Add-CompletionFixture notready-over-count $on {param($x) Set-CompletionField $x C 3 '2'}
    Add-CompletionFixture query-failures $on {param($x) Set-CompletionField $x C 6 '1'}
    Add-CompletionFixture failure-leading-zero $on {param($x) Set-CompletionField $x C 6 '00'}
    Add-CompletionFixture span-without-call $timeline {param($x) Set-CompletionField $x C 4 '0'}
    foreach($value in @('NaN','Infinity','-1','junk','1e999',' 0.195600000','0.195600000 ')){
        Add-CompletionFixture ('number-'+$fixtures.Count) $on ([scriptblock]::Create("param(`$x) Set-CompletionField `$x C 7 '$value'"))
    }
    Add-CompletionFixture parent-nan $on {param($x) Set-CompletionField $x P 9 'NaN'}
    Add-CompletionFixture parent-junk $on {param($x) Set-CompletionField $x P 9 'junk'}
    Add-CompletionFixture closure-wrong $on {param($x) Set-CompletionField $x C 8 '0.5'}
    Add-CompletionFixture total-infinity $on {param($x) Set-CompletionField $x C 7 '1e308';Set-CompletionField $x C 8 '1e308'}
    foreach($column in @(7,8)){Add-CompletionFixture "off-numeric-$column" $off ([scriptblock]::Create("param(`$x) Set-CompletionField `$x C $column '0'"))}
    Add-CompletionFixture off-parent-numeric $off {param($x) Set-CompletionField $x P 9 '0'}
    Add-CompletionFixture present-calls-zero $on {param($x) Set-CompletionField $x P 2 '0'}
    Add-CompletionFixture present-wait-overflow $on {param($x) Set-CompletionField $x P 4 '18446744073709551616'}
    Add-CompletionFixture dispatch-missing $on {param($x) $x.L=@('unrelated')}
    Add-CompletionFixture dispatch-duplicate $on {param($x) $x.L+=@($x.L[0])}
    Add-CompletionFixture dispatch-prefix $on {param($x) $x.L[0]='junk '+$x.L[0]}
    Add-CompletionFixture dispatch-tail $on {param($x) $x.L[0]+=' garbage'}
    Add-CompletionFixture dispatch-nospace $on {param($x) $x.L[0]=$x.L[0].Replace(': ',':')}
    Add-CompletionFixture dispatch-case $on {param($x) $x.L[0]=$x.L[0].ToUpperInvariant()}
    Add-CompletionFixture dispatch-below-sum $on {param($x) $x.L[0]=$x.L[0].Replace('fenceStatus=100','fenceStatus=1')}
    Add-CompletionFixture dispatch-leading-zero $on {param($x) $x.L[0]=$x.L[0].Replace('counter=0','counter=00')}
    Add-CompletionFixture dispatch-overflow $on {param($x) $x.L[0]=$x.L[0].Replace('counter=0','counter=18446744073709551616')}
    Add-CompletionFixture dispatch-extra-malformed $on {param($x) $x.L+='COMPLETIONPROFILEDISPATCHjunk'}
    # Exact UInt64 boundary: this legal base sums to MaxValue, then only row 2 changes.
    $large=@{C=$on.C.Clone();P=$on.P.Clone();L=@('completion-profile-dispatch: whole-run fenceStatus=18446744073709551615 counter=0 resetFence=98');Frames=2;Profile='on'}
    Set-CompletionField $large C 2 '18446744073709551614'
    Add-CompletionFixture uint64-boundary $large $null $true
    Add-CompletionFixture sum-overflow $large {param($x) Set-CompletionField $x C 2 '2' 2}
    $results=New-Object 'System.Collections.Generic.List[object]';$passed=0
    foreach($f in $fixtures){
        $dir=Join-Path $Report $f.Name;New-Item -ItemType Directory -Path $dir|Out-Null
        $f.Input.C|Set-Content "$dir/completion.csv" -Encoding UTF8
        $f.Input.P|Set-Content "$dir/present.csv" -Encoding UTF8
        $f.Input.L|Set-Content "$dir/log.txt" -Encoding UTF8
        # Empty arrays produce an empty file; all fixtures replay their file contents.
        foreach($file in @('completion.csv','present.csv','log.txt')){if(-not (Test-Path -LiteralPath "$dir/$file")){New-Item -ItemType File -Path "$dir/$file"|Out-Null}}
        $f.Input|ConvertTo-Json -Depth 4|Set-Content "$dir/input.json" -Encoding UTF8
        $result=Test-CompletionSidecar -CompletionLines @(Get-Content -LiteralPath "$dir/completion.csv") -PresentLines @(Get-Content -LiteralPath "$dir/present.csv") -LogLines @(Get-Content -LiteralPath "$dir/log.txt") -Frames $f.Input.Frames -Profile $f.Input.Profile
        $ok=$result.Pass -eq $f.Want;if($ok){$passed++}else{Write-Host "FAIL: $($f.Name) actual=$($result.Pass) expected=$($f.Want)"}
        $results.Add([pscustomobject]@{Name=$f.Name;Expected=$f.Want;Actual=$result.Pass;Pass=$ok;Reasons=$result.Reasons})
    }
    $results.ToArray()|ConvertTo-Json -Depth 5|Set-Content "$Report/selftest.json" -Encoding UTF8
    Write-Host "Completion SelfTest declared=$($fixtures.Count) executed=$($results.Count) passed=$passed"
    if($passed -ne $fixtures.Count){throw 'Completion SelfTest failed'}
}
function Invoke-CompletionReplay {
    param([string]$Root,[string]$Report)
    $decoded=Get-Content -LiteralPath (Join-Path $Root 'manifest.json') -Raw|ConvertFrom-Json
    $manifest=@($decoded)
    $seen=@{};$results=New-Object 'System.Collections.Generic.List[object]'
    if($manifest.Count -ne 8 -and $manifest.Count -ne 40){throw 'Replay requires eight short or forty formal cases'}
    $formal=$manifest.Count -eq 40
    $required=@();foreach($v in @('on','off')){foreach($b in @('fence','timeline')){foreach($p in @('on','off')){
        if($formal){foreach($r in 1..5){$required+="$v-$b-$p-rep$r"}}else{$required+="$v-$b-$p"}
    }}}
    foreach($case in $manifest){
        $name=[string]$case.Case
        if($name -cnotin $required -or $seen.ContainsKey($name)){throw "unknown/duplicate manifest case: $name"}
        $seen[$name]=$true;$parts=$name.Split('-')
        if($case.Validation -cne $parts[0] -or $case.Backend -cne $parts[1] -or $case.Profile -cne $parts[2] -or
           ($case.Frames -isnot [int] -and $case.Frames -isnot [long]) -or
           ($case.Warmup -isnot [int] -and $case.Warmup -isnot [long]) -or
           ($formal -and ($case.Frames -lt 1000 -or $case.Warmup -lt 300)) -or
           (-not $formal -and ($case.Frames -ne 40 -or $case.Warmup -ne 10))){throw "manifest configuration mismatch: $name"}
        $result=Test-CompletionEvidenceCase $case
        $results.Add([pscustomobject]@{Case=$name;Pass=$result.Pass;Reasons=$result.Reasons})
        Write-Host "$name judge=$($result.Pass)"
    }
    $results.ToArray()|ConvertTo-Json -Depth 5|Set-Content "$Report/replay.json" -Encoding UTF8
    $missing=@($required|Where-Object{-not $seen.ContainsKey($_)})
    if($missing.Count -or @($results|Where-Object{-not $_.Pass}).Count){throw 'Completion replay failed'}
}
try{
    if($SelfTest.IsPresent -eq [bool]$EvidenceRoot){throw 'Specify exactly one of -SelfTest or -EvidenceRoot'}
    $report=New-CompletionReportRoot;Write-Host "report root: $report"
    if($SelfTest){Invoke-CompletionSelfTest $report}
    else{Import-CompletionProfileDependencies;Invoke-CompletionReplay $EvidenceRoot $report}
    exit 0
}catch{Write-Host "FAIL: $($_.Exception.Message)";exit 1}
