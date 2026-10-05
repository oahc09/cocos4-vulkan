# Offline replay only: this script never builds or runs a GPU executable.
[CmdletBinding()]
param([switch]$SelfTest,[string]$EvidenceRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$metricNames=@('wall_ms','cpu_ms','wait_calls','wait_ms','fence_objects','semaphore_objects','gpu_clear_ms','in_flight')
$culture=[Globalization.CultureInfo]::InvariantCulture

function Import-LegacyBenchJudges {
    # Install only the two declared function bodies. Never execute the old runner.
    $tokens=$null;$errors=$null
    $ast=[System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'verify-completion-benchmark.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'Legacy verifier parse failure'}
    foreach($name in @('Test-BenchmarkResult','Test-BenchmarkPhases')){
        $nodes=@($ast.FindAll({param($n)
            $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name
        },$true))
        if($nodes.Count -ne 1){throw "Missing/duplicate legacy function: $name"}
        $body=$nodes[0].Body.Extent.Text
        $block=[scriptblock]::Create($body.Substring(1,$body.Length-2))
        Set-Item -Path "Function:script:$name" -Value $block
    }
}
function Convert-ProfileUInt {
    param([string]$Text)
    $value=[long]0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)$' -and
        [long]::TryParse($Text,[Globalization.NumberStyles]::None,$culture,[ref]$value)
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Convert-ProfileNumber {
    param([string]$Text)
    $value=0.0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$' -and
        [double]::TryParse($Text,[Globalization.NumberStyles]::Float,$culture,[ref]$value) -and
        -not [double]::IsNaN($value) -and -not [double]::IsInfinity($value) -and $value -ge 0
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Test-ProfileMarker {
    param([string[]]$Lines,[string]$Wide,[string]$Exact,[int]$Count=1)
    $found=@($Lines|Where-Object{$_ -imatch $Wide})
    $good=$found.Count -eq $Count
    foreach($line in $found){if($line -cnotmatch $Exact){$good=$false}}
    [pscustomobject]@{Pass=$good;Lines=$found}
}
function Test-PresentProfileEvidence {
    param(
        [string[]]$LogLines,[string[]]$MainLines,[string[]]$PhaseLines,[string[]]$PresentLines,
        [object]$Code,[string]$Validation,[string]$Backend,[string]$Profile,
        [int]$Frames,[int]$Warmup,[switch]$WaitProfile
    )
    $why=New-Object 'System.Collections.Generic.List[string]'
    try {
        if(($Code -isnot [int] -and $Code -isnot [long]) -or $Code -ne 0){$why.Add('exit must be integer zero')}
        if($Validation -cnotin @('on','off') -or $Backend -cnotin @('fence','timeline') -or
           $Profile -cnotin @('on','off') -or $Frames -lt 1 -or $Frames -gt 100000 -or
           $Warmup -lt 0 -or $Warmup -gt 100000){$why.Add('invalid expected configuration')}
        if(-not $LogLines -or @($LogLines|Where-Object{$null -eq $_}).Count){$why.Add('missing log')}
        $headers=@(
            'frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight',
            'frame,acquire_ms,record_ms,submit_ms,observe_ms,present_ms',
            'frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms'
        )
        $groups=@(
            @{Name='main';Lines=$MainLines;Header=$headers[0]},
            @{Name='phase';Lines=$PhaseLines;Header=$headers[1]},
            @{Name='present';Lines=$PresentLines;Header=$headers[2]}
        )
        foreach($g in $groups){
            if($null -eq $g.Lines -or $g.Lines.Count -ne $Frames+1 -or
               $g.Lines[0] -cne $g.Header -or @($g.Lines|Where-Object{[string]::IsNullOrWhiteSpace($_)}).Count){
                $why.Add("invalid $($g.Name) CSV header/count/empty rows")
            }
        }
        # No indexing, casts or legacy calls before shape validation succeeds.
        if($why.Count){return [pscustomobject]@{Pass=$false;Reasons=@($why.ToArray())}}

        $prefix='^(?:[0-9]{2}:[0-9]{2}:[0-9]{2} \[INFO\]: )?'
        $timeline=if($Backend -ceq 'timeline'){'1'}else{'0'}
        $triad=if($Validation -ceq 'on'){'ENABLED'}else{'NOT VERIFIED'}
        $markers=@(
            @{N='API';W='vulkan\s*api\b';E=$prefix+'Vulkan API: loader=[0-9]+\.[0-9]+ requested=[0-9]+\.[0-9]+ XR=[0-9]+\.[0-9]+ instance=[0-9]+\.[0-9]+ physical=[0-9]+\.[0-9]+ effective=1\.3$';C=1},
            @{N='paths';W='vulkan\s*paths\b';E=$prefix+"Vulkan paths: timeline=$timeline sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0$";C=1},
            @{N='validation';W='validation\s*\+\s*best\s*practices';E='^'+[regex]::Escape("validation + best practices + sync: $triad")+'$';C=1},
            @{N='config';W='benchmark\s*config\b';E='^'+[regex]::Escape("benchmark config: backend=$Backend validation=$Validation warmup=$Warmup frames=$Frames effective=1.3")+'$';C=1},
            @{N='encoders';W='benchmark\s*encoders\b';E='^benchmark encoders: submit=legacy barrier=legacy$';C=1},
            @{N='dispatch';W='benchmark\s*dispatch\b';E='^benchmark dispatch: frames=(0|[1-9][0-9]*) legacySubmits=(0|[1-9][0-9]*) modernSubmits=(0|[1-9][0-9]*) legacyBarriers=(0|[1-9][0-9]*) modernBarriers=(0|[1-9][0-9]*)$';C=1},
            @{N='device';W='benchmark\s*device\b';E='^benchmark device: name=.+ driver=[0-9]+ timestampBits=([1-9]|[1-5][0-9]|6[0-4]) timestampPeriod=[0-9]+\.[0-9]+$';C=1},
            @{N='surface';W='benchmark\s*surface\b';E='^benchmark surface: width=[1-9][0-9]* height=[1-9][0-9]* images=[1-9][0-9]* slots=[1-9][0-9]* presentMode=[0-9]+ maintenance1=[01]$';C=1},
            @{N='profile';W='benchmark\s*present-profile\b';E='^'+[regex]::Escape("benchmark present-profile: mode=$Profile fault=none")+'$';C=1},
            @{N='scope';W='benchmark\s*scope\b';E='^benchmark scope: clear-CB GPU timestamps; frame-flow-thread GetThreadTimes; API waits; point polling included$';C=1},
            @{N='metrics';W='benchmark\s*metric\b';E='^benchmark metric: (wall_ms|cpu_ms|wait_calls|wait_ms|fence_objects|semaphore_objects|gpu_clear_ms|in_flight) mean=[0-9]+\.[0-9]+ p50=[0-9]+\.[0-9]+ p95=[0-9]+\.[0-9]+ p99=[0-9]+\.[0-9]+ max=[0-9]+\.[0-9]+$';C=8},
            @{N='phases';W='benchmark\s*phase\b';E='^benchmark phase: (acquire_ms|record_ms|submit_ms|observe_ms|present_ms) mean=[0-9]+\.[0-9]+ p50=[0-9]+\.[0-9]+ p95=[0-9]+\.[0-9]+ p99=[0-9]+\.[0-9]+ max=[0-9]+\.[0-9]+$';C=5},
            @{N='footer';W='completed';E='^'+[regex]::Escape("completion benchmark completed: frames=$Frames failures=0 errors=0")+'$';C=1},
            @{N='leak';W='total\s*device\s*memory\s*leaked';E=$prefix+'Total device memory leaked: 0 bytes\.$';C=1}
        )
        if($WaitProfile){
            $markers+=@{N='wait-profile';W='benchmark\s*wait\s*-?\s*profile';E='^benchmark wait-profile: mode=on fault=none capacity=32$';C=1}
        }
        foreach($m in $markers){
            $check=Test-ProfileMarker -Lines $LogLines -Wide $m.W -Exact $m.E -Count $m.C
            if(-not $check.Pass){$why.Add("reserved $($m.N) missing/duplicate/malformed")}
        }
        foreach($line in $LogLines){
            if($line -imatch 'VUID-|HAZARD|FAIL:|SKIPPED|INCOMPLETE|UNSUPPORTED'){$why.Add('error/incomplete log')}
            if($line -imatch 'benchmark' -and
               $line -cnotmatch '^benchmark (config|encoders|dispatch|device|surface|present-profile|scope|metric|phase): ' -and
               $line -cnotmatch '^completion benchmark completed: ' -and
               -not ($WaitProfile -and $line -ceq 'benchmark wait-profile: mode=on fault=none capacity=32')){
                $why.Add('unknown/malformed benchmark family')
            }
        }

        for($i=0;$i -lt $Frames;$i++){
            $main=$MainLines[$i+1].Split(',')
            $phase=$PhaseLines[$i+1].Split(',')
            $pr=$PresentLines[$i+1].Split(',')
            if($main.Count -ne 9 -or $phase.Count -ne 6 -or $pr.Count -ne 10){
                $why.Add("row $i column count");continue
            }
            if($main[0] -cne [string]$i -or $phase[0] -cne [string]$i -or $pr[0] -cne [string]$i){$why.Add("row $i frame mismatch")}
            foreach($n in 1..8){
                $parsed=Convert-ProfileNumber $main[$n]
                if(-not $parsed.Pass){$why.Add("row $i invalid main number")}
                if($n -in @(3,5,6,8) -and -not (Convert-ProfileUInt $main[$n]).Pass){$why.Add("row $i invalid main integer")}
            }
            foreach($n in 1..5){if(-not (Convert-ProfileNumber $phase[$n]).Pass){$why.Add("row $i invalid phase number")}}
            if($pr[1] -cne $Profile -or $pr[2] -cne '1' -or $pr[3] -cne '1' -or $pr[6] -cne '0'){$why.Add("row $i profile/calls/success/failures")}
            $fw=Convert-ProfileUInt $pr[4];$tw=Convert-ProfileUInt $pr[5]
            if(-not $fw.Pass -or -not $tw.Pass){$why.Add("row $i invalid scoped wait integer")}
            if($Profile -ceq 'off'){
                if($pr[7] -cne 'NA' -or $pr[8] -cne 'NA' -or $pr[9] -cne 'NA'){$why.Add("row $i off durations must be NA")}
            }else{
                $d=Convert-ProfileNumber $pr[7];$w=Convert-ProfileNumber $pr[8];$h=Convert-ProfileNumber $pr[9]
                $parent=Convert-ProfileNumber $phase[5]
                if(-not ($d.Pass -and $w.Pass -and $h.Pass -and $parent.Pass)){$why.Add("row $i invalid profile number")}
                else {
                    $sum=$d.Value+$w.Value+$h.Value
                    if([double]::IsInfinity($sum) -or [double]::IsNaN($sum) -or [math]::Abs($sum-$parent.Value) -ge 1e-8){$why.Add("row $i present partition mismatch")}
                    if($w.Value -gt 0 -and $fw.Pass -and $tw.Pass -and $fw.Value -eq 0 -and $tw.Value -eq 0){$why.Add("row $i host span without waits")}
                }
            }
        }
        if(-not $why.Count){
            $mainRows=@($MainLines|ConvertFrom-Csv);$phaseRows=@($PhaseLines|ConvertFrom-Csv)
            $legacy=Test-BenchmarkResult -Lines $LogLines -Rows $mainRows -Code 0 -Backend $Backend -Validation $Validation -Frames $Frames -Warmup $Warmup
            $phases=Test-BenchmarkPhases -Rows $phaseRows -MainRows $mainRows -Lines $LogLines
            foreach($reason in @($legacy.Reasons)+@($phases.Reasons)){$why.Add("legacy: $reason")}
            if(-not $legacy.Pass -and -not $legacy.Reasons.Count){$why.Add('legacy main rejected')}
            if(-not $phases.Pass -and -not $phases.Reasons.Count){$why.Add('legacy phases rejected')}
        }
    }catch{$why.Add("invalid evidence: $($_.Exception.Message)")}
    [pscustomobject]@{Pass=($why.Count -eq 0);Reasons=@($why.ToArray())}
}

function Read-ProfileEvidence {
    param([string]$Csv,[string]$Log,[object]$Code,[string]$Validation,[string]$Backend,[string]$Profile)
    foreach($path in @($Csv,$Log,($Csv+'.phases.csv'),($Csv+'.present.csv'))){
        if(-not (Test-Path -LiteralPath $path -PathType Leaf)){
            return [pscustomobject]@{Pass=$false;Reasons=@("missing file: $path")}
        }
    }
    Test-PresentProfileEvidence -LogLines @(Get-Content -LiteralPath $Log) -MainLines @(Get-Content -LiteralPath $Csv) -PhaseLines @(Get-Content -LiteralPath ($Csv+'.phases.csv')) -PresentLines @(Get-Content -LiteralPath ($Csv+'.present.csv')) -Code $Code -Validation $Validation -Backend $Backend -Profile $Profile -Frames 40 -Warmup 10
}
function Test-ProfileManifest {
    param([string]$Root)
    $why=New-Object 'System.Collections.Generic.List[string]';$results=New-Object 'System.Collections.Generic.List[object]'
    try{
        $path=Join-Path $Root 'manifest.json'
        if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw 'manifest missing'}
        $manifest=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
        $manifest=@($manifest)
        $required=@();foreach($v in @('on','off')){foreach($b in @('fence','timeline')){foreach($p in @('on','off')){$required+="$v-$b-$p"}}}
        if($manifest.Count -ne 8){$why.Add('manifest must contain exactly eight cases')}
        $seen=@{}
        foreach($m in $manifest){
            $name=[string]$m.case
            if($name -cnotmatch '^(on|off)-(fence|timeline)-(on|off)$'){
                $why.Add("unknown case: $name");continue
            }
            if($seen.ContainsKey($name)){$why.Add("duplicate case: $name");continue}
            $seen[$name]=$true;$parts=$name.Split('-')
            if($m.PSObject.Properties.Name -cnotcontains 'exit' -or
               [string]::IsNullOrEmpty([string]$m.csv) -or [string]::IsNullOrEmpty([string]$m.log)){
                $r=[pscustomobject]@{Pass=$false;Reasons=@('missing manifest fields')}
            }else{
                $r=Read-ProfileEvidence -Csv $m.csv -Log $m.log -Code $m.exit -Validation $parts[0] -Backend $parts[1] -Profile $parts[2]
            }
            if(-not $r.Pass){$why.Add("case failed: $name")}
            $results.Add([pscustomobject]@{Case=$name;Code=$m.exit;Pass=$r.Pass;Reasons=@($r.Reasons)})
        }
        foreach($name in $required){if(-not $seen.ContainsKey($name)){$why.Add("missing case: $name")}}
    }catch{$why.Add("invalid manifest: $($_.Exception.Message)")}
    [pscustomobject]@{Pass=($why.Count -eq 0);Reasons=@($why.ToArray());Cases=@($results.ToArray())}
}
function New-ProfileReportDirectory {
    $base=Join-Path $repo 'native/build/vulkan-modernization/present-profile-checks'
    if(-not (Test-Path -LiteralPath $base)){New-Item -ItemType Directory -Path $base|Out-Null}
    $path=Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmssfff')+'-'+$PID+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $path -ErrorAction Stop|Out-Null
    $path
}
function Invoke-ProfileSelfTest {
    $root=Join-Path $repo 'native/build/vulkan-modernization/perf-b1a-lead-20261004-074907724-17608'
    $fixtures=New-Object 'System.Collections.Generic.List[object]'
    function Get-Base([string]$Name){
        $parts=$Name.Split('-');$dir=Join-Path $root $Name
        @{LogLines=@(Get-Content "$dir/log.txt");MainLines=@(Get-Content "$dir/frames.csv");PhaseLines=@(Get-Content "$dir/frames.csv.phases.csv");PresentLines=@(Get-Content "$dir/frames.csv.present.csv");Code=0;Validation=$parts[0];Backend=$parts[1];Profile=$parts[2];Frames=40;Warmup=10}
    }
    function Add-Case([string]$Name,[scriptblock]$Change,[bool]$Want=$false,[string]$Base='on-fence-on'){
        $x=Get-Base $Base
        & $Change $x
        $fixtures.Add([pscustomobject]@{Name=$Name;Args=$x;Want=$Want})
    }
    function Set-Cell($x,[string]$Group,[int]$Column,[string]$Value){
        $c=$x[$Group][1].Split(',');$c[$Column]=$Value;$x[$Group][1]=$c -join ','
    }
    function Replace-Log($x,[string]$Pattern,[string]$Value){
        $found=@();for($i=0;$i -lt $x.LogLines.Count;$i++){if($x.LogLines[$i] -cmatch $Pattern){$found+=$i}}
        if($found.Count -ne 1){throw "fixture source marker not unique: $Pattern"}
        $x.LogLines[$found[0]]=$Value
    }
    foreach($base in @('on-fence-on','on-fence-off','on-timeline-on','on-timeline-off','off-fence-on','off-fence-off','off-timeline-on','off-timeline-off')){
        Add-Case "valid-$base" {} $true $base
    }
    Add-Case 'timeline-nonzero-wait-valid' {param($x) Set-Cell $x PresentLines 5 '1'} $true 'off-timeline-on'
    Add-Case 'exit-ntstatus' {param($x) $x.Code=-1073741819}
    Add-Case 'exit-fraction' {param($x) $x.Code=0.1}
    foreach($bad in @('Validation','Backend','Profile')){
        Add-Case "expected-$bad-invalid" {param($x) $x[$bad]='unknown'}
    }
    Add-Case 'missing-log' {param($x) $x.LogLines=@()}
    foreach($group in @('MainLines','PhaseLines','PresentLines')){
        Add-Case "$group-missing" {param($x) $x[$group]=$null}
        Add-Case "$group-header-only" {param($x) $x[$group]=@($x[$group][0])}
        Add-Case "$group-schema-garbage" {param($x) $x[$group][0]='garbage'}
        Add-Case "$group-count-short" {param($x) $x[$group]=@($x[$group][0..39])}
        Add-Case "$group-row-empty" {param($x) $x[$group][1]=''}
        Add-Case "$group-columns-short" {param($x) $x[$group][1]='0,1'}
        Add-Case "$group-frame-wrong" {param($x) Set-Cell $x $group 0 '1'}
        Add-Case "$group-frame-leading-zero" {param($x) Set-Cell $x $group 0 '00'}
    }
    Add-Case 'main-negative-cpu' {param($x) Set-Cell $x MainLines 2 '-1'}
    Add-Case 'phase-negative-record' {param($x) Set-Cell $x PhaseLines 2 '-1'}
    Add-Case 'main-NaN' {param($x) Set-Cell $x MainLines 2 'NaN'}
    Add-Case 'phase-Infinity' {param($x) Set-Cell $x PhaseLines 2 'Infinity'}
    Add-Case 'main-whole-partition-wrong' {param($x) Set-Cell $x MainLines 1 '9'}
    Add-Case 'metric-value-wrong' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -clike 'benchmark metric: wall_ms *'){$_ -replace 'mean=[0-9.]+','mean=9.000000000'}else{$_}})}
    Add-Case 'phase-stat-wrong' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -clike 'benchmark phase: acquire_ms *'){$_ -replace 'mean=[0-9.]+','mean=9.000000000'}else{$_}})}
    Add-Case 'missing-wall-metric' {param($x) $x.LogLines=@($x.LogLines|Where-Object{$_ -cnotlike 'benchmark metric: wall_ms *'})}
    Add-Case 'wrong-config-frames' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -clike 'benchmark config:*'){$_ -replace 'frames=40','frames=999'}else{$_}})}
    Add-Case 'wrong-config-warmup' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -clike 'benchmark config:*'){$_ -replace 'warmup=10','warmup=999'}else{$_}})}
    Add-Case 'wrong-dispatch-count' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -clike 'benchmark dispatch:*'){$_ -replace 'frames=50','frames=999'}else{$_}})}
    Add-Case 'unknown-benchmark-marker' {param($x) $x.LogLines+='benchmark_unknown malformed'}
    Add-Case 'foreign-footer' {param($x) $x.LogLines+='foo completed failures=1'}
    Add-Case 'truncated-footer' {param($x) $x.LogLines+='completionbenchmark completed'}
    Add-Case 'leak-garbage-prefix' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -imatch 'Total device memory leaked'){'garbage '+$_}else{$_}})}
    foreach($bad in @('VUID-123','HAZARD','[INFO]: FAIL: bad','SKIPPED','INCOMPLETE','UNSUPPORTED')){
        Add-Case "forbidden-$bad" {param($x) $x.LogLines+=$bad}
    }
    foreach($family in @('config','encoders','dispatch','device','surface','present-profile','scope','metric','phase')){
        $pattern='^benchmark '+[regex]::Escape($family)+':'
        Add-Case "marker-$family-missing" {param($x) $removed=$false;$x.LogLines=@(foreach($line in $x.LogLines){if(-not $removed -and $line -cmatch $pattern){$removed=$true}else{$line}})}
        Add-Case "marker-$family-duplicate" {param($x) $x.LogLines+=@($x.LogLines|Where-Object{$_ -cmatch $pattern}|Select-Object -First 1)}
        Add-Case "marker-$family-malformed" {param($x) $index=-1;for($i=0;$i -lt $x.LogLines.Count;$i++){if($x.LogLines[$i] -cmatch $pattern){$index=$i;break}};$x.LogLines[$index]='garbage '+$x.LogLines[$index]}
    }
    foreach($family in @('Vulkan API','Vulkan paths','validation + best practices + sync','Total device memory leaked','completion benchmark completed')){
        $pattern=[regex]::Escape($family)
        Add-Case "reserved-$family-missing" {param($x) $x.LogLines=@($x.LogLines|Where-Object{$_ -cnotmatch $pattern})}
        Add-Case "reserved-$family-duplicate" {param($x) $x.LogLines+=@($x.LogLines|Where-Object{$_ -cmatch $pattern})}
        Add-Case "reserved-$family-junk-replacement" {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -cmatch $pattern){'junk '+$_}else{$_}})}
        Add-Case "reserved-$family-nospace-extra" {param($x) $x.LogLines+=($family -replace ' ','')}
    }
    Add-Case 'api-effective-1.30' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -cmatch 'Vulkan API:'){$_ -replace 'effective=1.3','effective=1.30'}else{$_}})}
    Add-Case 'paths-wrong-backend' {param($x) $x.LogLines=@($x.LogLines|ForEach-Object{if($_ -cmatch 'Vulkan paths:'){$_ -replace 'timeline=0','timeline=1'}else{$_}})}
    foreach($column in @(4,5)){
        foreach($bad in @('01','+1','-1',' 1','9223372036854775808')){
            Add-Case "wait-$column-invalid-$bad" {param($x) Set-Cell $x PresentLines $column $bad}
        }
    }
    foreach($column in @(7,8,9)){
        foreach($bad in @('NaN','Infinity','-1','1e999','NA')){
            Add-Case "profile-$column-invalid-$bad" {param($x) Set-Cell $x PresentLines $column $bad}
        }
    }
    Add-Case 'present-partition-wrong' {param($x) Set-Cell $x PresentLines 9 '9'}
    Add-Case 'host-span-without-waits' {param($x) Set-Cell $x PresentLines 4 '0'}
    Add-Case 'calls-zero' {param($x) Set-Cell $x PresentLines 2 '0'}
    Add-Case 'successes-zero' {param($x) Set-Cell $x PresentLines 3 '0'}
    Add-Case 'wait-failure-one' {param($x) Set-Cell $x PresentLines 6 '1'}
    Add-Case 'off-numeric-not-NA' {param($x) Set-Cell $x PresentLines 7 '0'} $false 'off-timeline-off'
    Add-Case 'present-mode-mismatch' {param($x) Set-Cell $x PresentLines 1 'off'}

    $out=New-ProfileReportDirectory
    $checks=New-Object 'System.Collections.Generic.List[object]'
    foreach($f in $fixtures){
        $fixtureArgs=$f.Args
        $r=Test-PresentProfileEvidence @fixtureArgs
        $ok=$r.Pass -eq $f.Want
        $checks.Add([pscustomobject]@{Name=$f.Name;Want=$f.Want;Actual=$r.Pass;Pass=$ok;Reasons=@($r.Reasons)})
        if(-not $ok){Write-Host "SelfTest FAIL: $($f.Name) $($r.Reasons -join '; ')"}
    }
    # Manifest control-flow fixtures reference unchanged real sample files.
    $original=Get-Content "$root/manifest.json" -Raw|ConvertFrom-Json
    $original=@($original)
    $manifestFixtures=@(
        @{Name='valid-eight';Rows=$original;Want=$true},
        @{Name='missing-case';Rows=@($original[0..6]);Want=$false},
        @{Name='duplicate-case';Rows=@($original[0..6])+$original[0];Want=$false},
        @{Name='empty-manifest';Rows=@();Want=$false}
    )
    $unknown=$original|ConvertTo-Json -Depth 5|ConvertFrom-Json;$unknown[0].case='on-fence-on-extra'
    $manifestFixtures+=@{Name='unknown-case';Rows=$unknown;Want=$false}
    $missing=$original|ConvertTo-Json -Depth 5|ConvertFrom-Json;$missing[0].csv=Join-Path $out 'absent.csv'
    $manifestFixtures+=@{Name='missing-file';Rows=$missing;Want=$false}
    $missingExit=$original|ConvertTo-Json -Depth 5|ConvertFrom-Json;$missingExit[0].PSObject.Properties.Remove('exit')
    $manifestFixtures+=@{Name='missing-exit';Rows=$missingExit;Want=$false}
    foreach($f in $manifestFixtures){
        $dir=Join-Path $out $f.Name;New-Item -ItemType Directory -Path $dir|Out-Null
        ConvertTo-Json -InputObject @($f.Rows) -Depth 5|Set-Content -LiteralPath "$dir/manifest.json" -Encoding UTF8
        $r=Test-ProfileManifest $dir
        $checks.Add([pscustomobject]@{Name="manifest-$($f.Name)";Want=$f.Want;Actual=$r.Pass;Pass=($r.Pass -eq $f.Want);Reasons=@($r.Reasons)})
    }
    $bad=@($checks|Where-Object{-not $_.Pass})
    $checks.ToArray()|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$out/selftest.json" -Encoding UTF8
    Write-Host "SelfTest declared=$($fixtures.Count+$manifestFixtures.Count) executed=$($checks.Count) passed=$($checks.Count-$bad.Count)"
    Write-Host "SelfTest evidence: $out"
    [pscustomobject]@{Pass=($bad.Count -eq 0);Report=$out}
}
try{
    if($SelfTest.IsPresent -eq [bool]$EvidenceRoot){throw 'Specify exactly one of -SelfTest or -EvidenceRoot'}
    Import-LegacyBenchJudges
    if($SelfTest){
        $r=Invoke-ProfileSelfTest
    }else{
        $r=Test-ProfileManifest $EvidenceRoot
        $out=New-ProfileReportDirectory
        $r|ConvertTo-Json -Depth 7|Set-Content -LiteralPath "$out/verdict.json" -Encoding UTF8
        foreach($case in $r.Cases){Write-Host "$($case.Case): Pass=$($case.Pass) $($case.Reasons -join '; ')"}
        Write-Host "Offline evidence verdict: $out"
        foreach($why in $r.Reasons){Write-Host "FAIL: $why"}
    }
    if(-not $r.Pass){exit 1}
    exit 0
}catch{Write-Host "FAIL: $($_.Exception.Message)";exit 1}
