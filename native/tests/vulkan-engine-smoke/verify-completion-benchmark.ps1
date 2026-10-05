# Finite single-window clear microbenchmark. Same API, alternating backend order.
# Validation OFF requires a separate build; this script never changes CMake policy.
[CmdletBinding()]
param(
    [string]$BuildDir='',
    [ValidateSet('on','off')][string]$Validation='on',
    [ValidateRange(1,100000)][int]$Frames=1000,
    [ValidateRange(1,100000)][int]$Warmup=300,
    [ValidateRange(1,20)][int]$Repeats=5,
    [ValidateSet('legacy','submit2')][string]$Submit='legacy',
    [ValidateSet('legacy','sync2')][string]$Barrier='legacy',
    [switch]$EncoderMatrix,
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $BuildDir) {$BuildDir=Join-Path $repo 'native\build\vulkan-modernization\win64-simulator'}
$metricNames=@('wall_ms','cpu_ms','wait_calls','wait_ms','fence_objects','semaphore_objects','gpu_clear_ms','in_flight')
$culture=[Globalization.CultureInfo]::InvariantCulture
function Test-BenchmarkResult {
    param([string[]]$Lines,[object[]]$Rows,[int]$Code,[string]$Backend,[string]$Validation,[int]$Frames,[int]$Warmup,[string]$Submit='legacy',[string]$Barrier='legacy')
    $reasons=@(); $metrics=@()
    if ($Code -ne 0) {$reasons+='nonzero native exit'}
    $config=@($Lines | Where-Object {$_ -match '^benchmark config:'})
    if ($config.Count -ne 1 -or $config[0] -cne "benchmark config: backend=$Backend validation=$Validation warmup=$Warmup frames=$Frames effective=1.3") {$reasons+='configuration mismatch'}
    $encoders=@($Lines | Where-Object {$_ -match '^benchmark encoders:'})
    if ($encoders.Count -ne 1 -or $encoders[0] -cne "benchmark encoders: submit=$Submit barrier=$Barrier") {$reasons+='encoder configuration mismatch'}
    $dispatch=@($Lines | Where-Object {$_ -match '^benchmark dispatch:'})
    if ($dispatch.Count -ne 1 -or $dispatch[0] -notmatch '^benchmark dispatch: frames=([0-9]+) legacySubmits=([0-9]+) modernSubmits=([0-9]+) legacyBarriers=([0-9]+) modernBarriers=([0-9]+)$') {$reasons+='missing/invalid/duplicate dispatch proof'}
    else {
        $n=[uint64]$Matches[1];$ls=[uint64]$Matches[2];$ms=[uint64]$Matches[3];$lb=[uint64]$Matches[4];$mb=[uint64]$Matches[5]
        if ($n -ne $Frames+$Warmup -or ($Submit -eq 'legacy' -and ($ls -ne $n -or $ms -ne 0)) -or ($Submit -eq 'submit2' -and ($ms -ne $n -or $ls -ne 0))) {$reasons+='submit dispatch count/route mismatch'}
        if (($Barrier -eq 'legacy' -and ($lb -eq 0 -or $mb -ne 0)) -or ($Barrier -eq 'sync2' -and ($mb -eq 0 -or $lb -ne 0))) {$reasons+='barrier dispatch count/route mismatch'}
    }
    $footer=@($Lines | Where-Object {$_ -match '^completion benchmark completed:'})
    if ($footer.Count -ne 1 -or $footer[0] -cne "completion benchmark completed: frames=$Frames failures=0 errors=0") {$reasons+='missing/unclean/duplicate footer'}
    $layer=@($Lines | Where-Object {$_ -match '^validation \+ best practices \+ sync:'})
    $want=if ($Validation -eq 'on') {'ENABLED'} else {'NOT VERIFIED'}
    if ($layer.Count -ne 1 -or $layer[0] -cne "validation + best practices + sync: $want") {$reasons+='validation mismatch'}
    if ($Lines | Where-Object {$_ -match 'VUID-|SYNC-HAZARD|FAIL:|SKIPPED|INCOMPLETE'}) {$reasons+='error/incomplete lines'}
    $leaks=@($Lines | Where-Object {$_ -match 'Total device memory leaked:'})
    if ($leaks.Count -ne 1 -or $leaks[0] -notmatch 'Total device memory leaked: 0 bytes\.\s*$') {$reasons+='memory leak report'}
    $metadata=@($Lines | Where-Object {$_ -match '^benchmark device:|^benchmark surface:'})
    if ($metadata.Count -ne 2 -or @($metadata | Where-Object {$_ -match '^benchmark device: name=.+ driver=[0-9]+ timestampBits=([1-9]|[1-5][0-9]|6[0-4]) timestampPeriod=[0-9]+\.[0-9]+$'}).Count -ne 1 -or @($metadata | Where-Object {$_ -match '^benchmark surface: width=[1-9][0-9]* height=[1-9][0-9]* images=[1-9][0-9]* slots=[1-9][0-9]* presentMode=[0-9]+ maintenance1=[01]$'}).Count -ne 1) {$reasons+='device/surface metadata'}
    if ($Rows.Count -ne $Frames) {$reasons+='sample count mismatch'}
    $expectedColumns=@('frame')+$metricNames[0..3]+@('fence_objects','semaphore_objects','gpu_clear_ms','in_flight')
    $rowGood=$true
    for ($i=0;$i -lt $Rows.Count;$i++) {
        $row=$Rows[$i]
        if (($row.PSObject.Properties.Name -join ',') -cne ($expectedColumns -join ',')) {$rowGood=$false; break}
        if ([string]$row.frame -cne [string]$i) {$rowGood=$false; break}
        foreach ($name in $metricNames) {
            $value=0.0
            if (-not [double]::TryParse([string]$row.$name,[Globalization.NumberStyles]::Float,$culture,[ref]$value) -or [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0) {$rowGood=$false; break}
            if ($name -in @('wait_calls','fence_objects','semaphore_objects','in_flight') -and $value -ne [math]::Floor($value)) {$rowGood=$false; break}
        }
    }
    if (-not $rowGood) {$reasons+='invalid sample columns/indices/numbers'}
    $metricLines=@($Lines | Where-Object {$_ -match '^benchmark metric:'})
    if ($metricLines.Count -ne 8) {$reasons+='metric line count'}
    foreach ($name in $metricNames) {
        $matchesForName=@($metricLines | Where-Object {$_ -match ('^benchmark metric: '+[regex]::Escape($name)+' ')})
        if ($matchesForName.Count -ne 1) {$reasons+="metric $name missing/duplicate"; continue}
        $pattern='^benchmark metric: '+[regex]::Escape($name)+' mean=([0-9]+\.[0-9]+) p50=([0-9]+\.[0-9]+) p95=([0-9]+\.[0-9]+) p99=([0-9]+\.[0-9]+) max=([0-9]+\.[0-9]+)$'
        if ($matchesForName[0] -notmatch $pattern) {$reasons+="metric $name invalid";continue}
        $reported=@($Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5]) | ForEach-Object {[double]::Parse($_,$culture)}
        if ($rowGood -and $Rows.Count -eq $Frames) {
            $values=@($Rows | ForEach-Object {[double]::Parse([string]$_.$name,$culture)} | Sort-Object)
            $sum=0.0; foreach ($v in $values) {$sum+=$v}
            $expected=@(($sum/$Frames),$values[[int][math]::Ceiling(.50*$Frames)-1],$values[[int][math]::Ceiling(.95*$Frames)-1],$values[[int][math]::Ceiling(.99*$Frames)-1],$values[-1])
            for ($j=0;$j -lt 5;$j++) {if ([math]::Abs($reported[$j]-$expected[$j]) -gt 0.00000001) {$reasons+="metric $name disagrees with CSV";break}}
        }
        $metrics+= [pscustomobject]@{Backend=$Backend;Validation=$Validation;Submit=$Submit;Barrier=$Barrier;Metric=$name;Mean=$reported[0];P50=$reported[1];P95=$reported[2];P99=$reported[3];Max=$reported[4]}
    }
    return [pscustomobject]@{Pass=($reasons.Count -eq 0);Reasons=$reasons;Metrics=$metrics}
}
function Test-BenchmarkPhases {
    param([object[]]$Rows,[object[]]$MainRows,[string[]]$Lines)
    $names=@('acquire_ms','record_ms','submit_ms','observe_ms','present_ms')
    $reasons=@()
    if ($Rows.Count -eq 0 -or $Rows.Count -ne $MainRows.Count) {$reasons+='missing phase sample rows'}
    $values=@{};foreach ($name in $names) {$values[$name]=@()}
    for ($i=0;$i -lt $Rows.Count;$i++) {
        if (($Rows[$i].PSObject.Properties.Name -join ',') -cne (@('frame')+$names -join ',') -or [string]$Rows[$i].frame -cne [string]$i) {$reasons+='phase column/index mismatch';break}
        $sum=0.0
        foreach ($name in $names) {
            $n=0.0
            if (-not [double]::TryParse([string]$Rows[$i].$name,[Globalization.NumberStyles]::Float,$culture,[ref]$n) -or [double]::IsNaN($n) -or [double]::IsInfinity($n) -or $n -lt 0) {$reasons+='invalid phase number';break}
            $values[$name]+=$n;$sum+=$n
        }
        if ($i -ge $MainRows.Count -or [math]::Abs($sum - [double]::Parse([string]$MainRows[$i].wall_ms,$culture)) -gt .00000001) {$reasons+='phases do not partition frame wall';break}
    }
    $markers=@($Lines | Where-Object {$_ -match '^benchmark phase:'})
    if ($markers.Count -ne 5) {$reasons+='phase metric count'}
    foreach ($name in $names) {
        $match=@($markers | Where-Object {$_ -match ('^benchmark phase: '+[regex]::Escape($name)+' ')})
        if ($match.Count -ne 1 -or $match[0] -notmatch ('^benchmark phase: '+[regex]::Escape($name)+' mean=([0-9]+\.[0-9]+) p50=([0-9]+\.[0-9]+) p95=([0-9]+\.[0-9]+) p99=([0-9]+\.[0-9]+) max=([0-9]+\.[0-9]+)$')) {$reasons+='missing/invalid phase statistic';continue}
        $reported=@($Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5]) | ForEach-Object {[double]::Parse($_,$culture)}
        if ($values[$name].Count -eq $MainRows.Count -and $MainRows.Count -gt 0) {
            $v=@($values[$name] | Sort-Object);$sum=0.0;foreach ($n in $v) {$sum+=$n}
            $count=$v.Count
            $want=@(($sum/$count),$v[[int][math]::Ceiling(.50*$count)-1],$v[[int][math]::Ceiling(.95*$count)-1],$v[[int][math]::Ceiling(.99*$count)-1],$v[-1])
            for ($j=0;$j -lt 5;$j++) {if ([math]::Abs($want[$j]-$reported[$j]) -gt .00000001) {$reasons+='phase statistics disagree with samples';break}}
        }
    }
    return [pscustomobject]@{Pass=($reasons.Count -eq 0);Reasons=$reasons}
}
if ($SelfTest) {
    $rows=@('frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight','0,1,1,1,1,1,1,1,1','1,1,1,1,1,1,1,1,1') | ConvertFrom-Csv
    $base=@('benchmark config: backend=fence validation=on warmup=1 frames=2 effective=1.3','validation + best practices + sync: ENABLED','completion benchmark completed: frames=2 failures=0 errors=0','Total device memory leaked: 0 bytes.')
    foreach ($name in $metricNames) {$base+="benchmark metric: $name mean=1.000000000 p50=1.000000000 p95=1.000000000 p99=1.000000000 max=1.000000000"}
    $base+=@('benchmark device: name=test driver=1 timestampBits=64 timestampPeriod=1.000000000','benchmark surface: width=640 height=480 images=3 slots=3 presentMode=0 maintenance1=1')
    $base+=@('benchmark encoders: submit=legacy barrier=legacy','benchmark dispatch: frames=3 legacySubmits=3 modernSubmits=0 legacyBarriers=6 modernBarriers=0')
    $fixtures=@(
        @{Name='valid';Lines=$base;Pass=$true},
        @{Name='modern-valid';Lines=($base -replace 'submit=legacy','submit=submit2' -replace 'barrier=legacy','barrier=sync2' -replace 'legacySubmits=3 modernSubmits=0','legacySubmits=0 modernSubmits=3' -replace 'legacyBarriers=6 modernBarriers=0','legacyBarriers=0 modernBarriers=6');Submit='submit2';Barrier='sync2';Pass=$true},
        @{Name='modern-wrong-submit-route';Lines=($base -replace 'submit=legacy','submit=submit2');Submit='submit2';Pass=$false},
        @{Name='modern-wrong-barrier-route';Lines=($base -replace 'barrier=legacy','barrier=sync2');Barrier='sync2';Pass=$false},
        @{Name='no-encoder';Lines=@($base | Where-Object {$_ -notmatch '^benchmark encoders:'});Pass=$false},
        @{Name='wrong-submit-encoder';Lines=($base -replace 'submit=legacy','submit=submit2');Pass=$false},
        @{Name='wrong-barrier-encoder';Lines=($base -replace 'barrier=legacy','barrier=sync2');Pass=$false},
        @{Name='duplicate-encoder';Lines=($base+'benchmark encoders: submit=legacy barrier=legacy');Pass=$false},
        @{Name='missing-dispatch';Lines=@($base | Where-Object {$_ -notmatch '^benchmark dispatch:'});Pass=$false},
        @{Name='duplicate-dispatch';Lines=($base+'benchmark dispatch: frames=3 legacySubmits=3 modernSubmits=0 legacyBarriers=6 modernBarriers=0');Pass=$false},
        @{Name='extra-submit';Lines=($base -replace 'legacySubmits=3','legacySubmits=4');Pass=$false},
        @{Name='wrong-submit-route';Lines=($base -replace 'modernSubmits=0','modernSubmits=1');Pass=$false},
        @{Name='no-barriers';Lines=($base -replace 'legacyBarriers=6','legacyBarriers=0');Pass=$false},
        @{Name='wrong-barrier-route';Lines=($base -replace 'modernBarriers=0','modernBarriers=1');Pass=$false},
        @{Name='wrong-dispatch-frames';Lines=($base -replace 'dispatch: frames=3','dispatch: frames=2');Pass=$false},
        @{Name='no-footer';Lines=@($base | Where-Object {$_ -notmatch '^completion benchmark completed:'});Pass=$false},
        @{Name='duplicate-footer';Lines=($base+$base[2]);Pass=$false},
        @{Name='crash';Lines=$base;Code=-1073741819;Pass=$false},
        @{Name='failures';Lines=($base -replace 'failures=0','failures=1');Pass=$false},
        @{Name='errors';Lines=($base -replace 'errors=0','errors=1');Pass=$false},
        @{Name='wrong-backend';Lines=($base -replace 'backend=fence','backend=timeline');Pass=$false},
        @{Name='wrong-api';Lines=($base -replace 'effective=1.3','effective=1.2');Pass=$false},
        @{Name='wrong-validation';Lines=($base -replace 'sync: ENABLED','sync: NOT VERIFIED');Pass=$false},
        @{Name='vuid';Lines=($base+'VUID-example');Pass=$false},
        @{Name='incomplete';Lines=($base+'INCOMPLETE');Pass=$false},
        @{Name='leak';Lines=($base -replace '0 bytes','1 bytes');Pass=$false},
        @{Name='duplicate-metric';Lines=($base+$base[4]);Pass=$false},
        @{Name='stats-wrong';Lines=($base -replace 'mean=1.000000000','mean=2.000000000');Pass=$false},
        @{Name='missing-sample';Lines=$base;Rows=@($rows[0]);Pass=$false},
        @{Name='nan-sample';Lines=$base;Csv=@('frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight','0,NaN,1,1,1,1,1,1,1','1,1,1,1,1,1,1,1,1');Pass=$false},
        @{Name='negative-sample';Lines=$base;Csv=@('frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight','0,-1,1,1,1,1,1,1,1','1,1,1,1,1,1,1,1,1');Pass=$false},
        @{Name='missing-metadata';Lines=@($base | Where-Object {$_ -notmatch '^benchmark device:'});Pass=$false},
        @{Name='zero-timestamp-bits';Lines=($base -replace 'timestampBits=64','timestampBits=0');Pass=$false},
        @{Name='duplicate-frame';Lines=$base;Csv=@('frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight','0,1,1,1,1,1,1,1,1','0,1,1,1,1,1,1,1,1');Pass=$false}
    )
    $ok=$true
    foreach ($f in $fixtures) {
        $fixtureRows=if ($f.Csv) {@($f.Csv | ConvertFrom-Csv)} elseif ($f.ContainsKey('Rows')) {@($f.Rows)} else {@($rows)}
        $fixtureSubmit=if ($f.Submit) {$f.Submit} else {'legacy'};$fixtureBarrier=if ($f.Barrier) {$f.Barrier} else {'legacy'}
        $result=Test-BenchmarkResult -Submit $fixtureSubmit -Barrier $fixtureBarrier -Lines $f.Lines -Rows $fixtureRows -Code $f.Code -Backend fence -Validation on -Frames 2 -Warmup 1
        $match=$result.Pass -eq $f.Pass; Write-Host "SelfTest $($f.Name): $match"
        if (-not $match) {$ok=$false; $result.Reasons | ForEach-Object {Write-Host $_}}
    }
    $phaseCSV=@('frame,acquire_ms,record_ms,submit_ms,observe_ms,present_ms','0,.2,.2,.2,.2,.2','1,.2,.2,.2,.2,.2')
    $phaseLines=@();foreach ($name in @('acquire_ms','record_ms','submit_ms','observe_ms','present_ms')) {$phaseLines+="benchmark phase: $name mean=0.200000000 p50=0.200000000 p95=0.200000000 p99=0.200000000 max=0.200000000"}
    $phaseFixtures=@(
        @{Name='phases-valid';Csv=$phaseCSV;Lines=$phaseLines;Pass=$true},
        @{Name='phases-missing';Csv=@($phaseCSV[0]);Lines=$phaseLines;Pass=$false},
        @{Name='phases-partition';Csv=($phaseCSV -replace '^0,.2','0,.3');Lines=$phaseLines;Pass=$false},
        @{Name='phases-nan';Csv=($phaseCSV -replace '^0,.2','0,NaN');Lines=$phaseLines;Pass=$false},
        @{Name='phases-index';Csv=($phaseCSV -replace '^1,','0,');Lines=$phaseLines;Pass=$false},
        @{Name='phases-stats';Csv=$phaseCSV;Lines=($phaseLines -replace 'mean=0.200000000','mean=0.300000000');Pass=$false},
        @{Name='phases-no-marker';Csv=$phaseCSV;Lines=@($phaseLines[0..3]);Pass=$false},
        @{Name='phases-duplicate-marker';Csv=$phaseCSV;Lines=($phaseLines+$phaseLines[0]);Pass=$false})
    foreach ($f in $phaseFixtures) {
        $verdict=Test-BenchmarkPhases -Rows @($f.Csv | ConvertFrom-Csv) -MainRows $rows -Lines $f.Lines
        $match=$verdict.Pass -eq $f.Pass;Write-Host "SelfTest $($f.Name): $match"
        if (-not $match) {$ok=$false;$verdict.Reasons | ForEach-Object {Write-Host $_}}
    }
    exit $(if ($ok) {0} else {1})
}
$vars=@('PATH','VK_LAYER_PATH','VK_IMPLICIT_LAYER_PATH','CC_VULKAN_COMPLETION','CC_VULKAN_FORCE_API','CC_VULKAN_DISABLE_FEATURES','CC_VULKAN_SUBMIT','CC_VULKAN_BARRIER')
if ($EncoderMatrix -and ($PSBoundParameters.ContainsKey('Submit') -or $PSBoundParameters.ContainsKey('Barrier'))) {Write-Host 'FAIL: -EncoderMatrix cannot be combined with -Submit/-Barrier';exit 1}
$locationPushed=$false
$old=@{};foreach ($v in $vars) {$old[$v]=[Environment]::GetEnvironmentVariable($v,'Process')}
try {
    & cmake --build $BuildDir --config Release --target vulkan_wsi_smoke --parallel 4
    if ($LASTEXITCODE -ne 0) {throw 'benchmark build failed'}
    $root=Join-Path $repo ('native\build\vulkan-modernization\completion-bench-{0:yyyyMMdd-HHmmss-fff}-{1}' -f (Get-Date),$PID)
    New-Item -ItemType Directory -Force $root | Out-Null
    Push-Location $root; $locationPushed=$true
    $empty=Join-Path $root 'empty-implicit-layers';New-Item -ItemType Directory $empty | Out-Null
    $env:VK_LAYER_PATH='D:\Software\VulkanSDK\1.4.341.1\Bin';$env:VK_IMPLICIT_LAYER_PATH=$empty
    $env:PATH=(Join-Path $repo 'native\external\win64\libs')+';'+(Join-Path $repo 'native\external\win64\libs\v8\Release')+';'+$env:PATH
    $env:CC_VULKAN_FORCE_API='1.3';$env:CC_VULKAN_DISABLE_FEATURES=$null
    $exe=Join-Path $BuildDir 'vulkan-engine-smoke\Release\vulkan_wsi_smoke.exe'
    $allGood=$true;$allMetrics=@();$referenceMetadata=$null
    $cases=@()
    $pairs=if ($EncoderMatrix) {@('legacy/legacy','submit2/legacy','legacy/sync2','submit2/sync2')} else {@("$Submit/$Barrier")}
    foreach ($pair in $pairs) {$names=$pair.Split('/');foreach ($b in @('fence','timeline')) {$cases+=[pscustomobject]@{Backend=$b;Submit=$names[0];Barrier=$names[1]}}}
    for ($r=1;$r -le $Repeats;$r++) {
        # Rotate the first case by repeat and reverse alternate repeats. Each
        # combination gets the same samples; no concurrent GPU benchmarks.
        $order=@();for ($i=0;$i -lt $cases.Count;$i++) {$order+=$cases[($i+$r-1)%$cases.Count]}
        if ($r%2 -eq 0) {[array]::Reverse($order)}
        foreach ($case in $order) {
            $b=$case.Backend;$s=$case.Submit;$bar=$case.Barrier
            $env:CC_VULKAN_SUBMIT=$s;$env:CC_VULKAN_BARRIER=$bar
            $env:CC_VULKAN_COMPLETION=$b
            $log=Join-Path $root "$r-$b-$s-$bar.log";$csv=Join-Path $root "$r-$b-$s-$bar.csv"
            # PowerShell 5.1 native stderr must not turn successful logs into exceptions.
            cmd /c """$exe"" --benchmark --frames $Frames --warmup $Warmup --validation $Validation --csv ""$csv"" > ""$log"" 2>&1"
            $code=$LASTEXITCODE;$lines=@([IO.File]::ReadAllLines($log))
            $rows=if (Test-Path -LiteralPath $csv) {@(Import-Csv -LiteralPath $csv)} else {@()}
            $result=Test-BenchmarkResult -Lines $lines -Rows $rows -Code $code -Backend $b -Validation $Validation -Frames $Frames -Warmup $Warmup -Submit $s -Barrier $bar
            $phaseFile=$csv+'.phases.csv'
            $phaseRows=if (Test-Path -LiteralPath $phaseFile) {@(Import-Csv -LiteralPath $phaseFile)} else {@()}
            $phaseResult=Test-BenchmarkPhases -Rows $phaseRows -MainRows $rows -Lines $lines
            if (-not $phaseResult.Pass) {$result.Pass=$false;$result.Reasons+=@($phaseResult.Reasons)}
            $metadata=@($lines | Where-Object {$_ -match '^benchmark device:|^benchmark surface:'}) -join "`n"
            if ($null -eq $referenceMetadata) {$referenceMetadata=$metadata}
            elseif ($metadata -cne $referenceMetadata) {$result.Pass=$false;$result.Reasons+='device/surface changed across comparisons'}
            Write-Host "$(if ($result.Pass) {'PASS'} else {'FAIL'}) repeat=$r backend=$b submit=$s barrier=$bar exit=$code log=$log"
            if (-not $result.Pass) {$allGood=$false;$result.Reasons | ForEach-Object {Write-Host "  $_"}}
            else {foreach ($m in $result.Metrics) {$m | Add-Member -NotePropertyName Repeat -NotePropertyValue $r;$allMetrics+=$m}}
        }
    }
    $allMetrics | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $root 'summary.csv')
    Write-Host "Artifacts: $root (finite clear; instrumentation and WSI pacing included)"
    exit $(if ($allGood) {0} else {1})
} catch {Write-Host "FAIL: $($_.Exception.Message)";exit 1}
finally {if ($locationPushed) {Pop-Location};foreach ($v in $vars) {[Environment]::SetEnvironmentVariable($v,$old[$v],'Process')}}
