# Pure control helpers for the adjacent, counterbalanced backend experiment.
function New-BackendPairPlan {
    param([int]$Count,[switch]$SameBackendControls)
    if($Count -lt 10 -or $Count -gt 20 -or $Count%2 -ne 0){throw 'Pairs must be even and between 10 and 20'}
    for($pair=1;$pair -le $Count;$pair++){
        $backends=if($SameBackendControls){
            if($pair%2){@('fence','fence')}else{@('timeline','timeline')}
        }elseif($pair%2){@('fence','timeline')}else{@('timeline','fence')}
        for($position=0;$position -lt 2;$position++){
            $backend=$backends[$position]
            [pscustomobject]@{
                Case=if($SameBackendControls){"off-$backend-on-rep$pair-member$position"}else{"off-$backend-on-rep$pair"}
                Validation='off';Backend=$backend;Profile='on';Repeat=$pair
                Pair=$pair;OrderIndex=(($pair-1)*2+$position);Position=$position
                Direction=if($SameBackendControls){"$backend-control"}elseif($pair%2){'fence-first'}else{'timeline-first'}
            }
        }
    }
}
function Test-BackendPairRun {
    param([object[]]$Plan,[object[]]$Manifest,[int]$Count,[int]$Frames,[int]$Warmup,[switch]$SameBackendControls)
    try{
        $expected=@(New-BackendPairPlan $Count -SameBackendControls:$SameBackendControls)
        if($Frames -lt 1000 -or $Warmup -lt 300 -or $Plan.Count -ne $expected.Count -or $Manifest.Count -ne $expected.Count){return $false}
        $previousEnd=$null
        for($i=0;$i -lt $expected.Count;$i++){
            foreach($field in @('Case','Validation','Backend','Profile','Repeat','Pair','OrderIndex','Position','Direction')){
                if($Plan[$i].$field -cne $expected[$i].$field -or $Manifest[$i].$field -cne $expected[$i].$field){return $false}
            }
            $entry=$Manifest[$i]
            if($entry.Frames -ne $Frames -or $entry.Warmup -ne $Warmup -or $entry.BuildTree -cne 'win64-benchmark-off'){return $false}
            foreach($field in @('StartedUtc','EndedUtc')){
                $parsed=[DateTimeOffset]::MinValue
                if($entry.$field -isnot [string] -or -not [DateTimeOffset]::TryParse($entry.$field,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$parsed) -or $parsed.Offset -ne [TimeSpan]::Zero -or $parsed.ToString('O') -cne $entry.$field){return $false}
            }
            $start=[DateTimeOffset]::Parse($entry.StartedUtc)
            $end=[DateTimeOffset]::Parse($entry.EndedUtc)
            if($end -lt $start -or ($null -ne $previousEnd -and $start -lt $previousEnd)){return $false}
            $previousEnd=$end
            if(($entry.ProcessWaitMs -isnot [double] -and $entry.ProcessWaitMs -isnot [int] -and $entry.ProcessWaitMs -isnot [long]) -or [double]::IsNaN($entry.ProcessWaitMs) -or [double]::IsInfinity($entry.ProcessWaitMs) -or $entry.ProcessWaitMs -lt 0){return $false}
        }
        return $true
    }catch{return $false}
}
function Invoke-BackendPairSelfTest {
    $plan=@(New-BackendPairPlan 10)
    $manifest=@($plan|ForEach-Object{
        $item=$_|Select-Object *
        $stamp=[DateTimeOffset]::Parse('2026-10-04T00:00:00.0000000+00:00').AddSeconds($_.OrderIndex*2)
        $item|Add-Member Frames 1000
        $item|Add-Member Warmup 300
        $item|Add-Member BuildTree 'win64-benchmark-off'
        $item|Add-Member StartedUtc $stamp.ToString('O')
        $item|Add-Member EndedUtc $stamp.AddSeconds(1).ToString('O')
        $item|Add-Member ProcessWaitMs 1000.0
        $item
    })
    $results=New-Object 'System.Collections.Generic.List[bool]'
    $results.Add((Test-BackendPairRun $plan $manifest 10 1000 300))
    $results.Add(($plan.Count -eq 20 -and @($plan|Where-Object{$_.Position -eq 0 -and $_.Backend -eq 'fence'}).Count -eq 5 -and @($plan|Where-Object{$_.Position -eq 0 -and $_.Backend -eq 'timeline'}).Count -eq 5))
    foreach($badCount in @(0,5,9,11,21)){
        $rejected=$false;try{$null=@(New-BackendPairPlan $badCount)}catch{$rejected=$true}
        $results.Add($rejected)
    }
    foreach($mutation in @(
        @{Field='Backend';Value='timeline'},@{Field='OrderIndex';Value=1},@{Field='Pair';Value=2},
        @{Field='Position';Value=1},@{Field='Direction';Value='timeline-first'},
        @{Field='Validation';Value='on'},@{Field='Profile';Value='off'},
        @{Field='Frames';Value=40},@{Field='Warmup';Value=10},
        @{Field='BuildTree';Value='win64-simulator'},@{Field='StartedUtc';Value='garbage'},
        @{Field='EndedUtc';Value='2026-10-03T00:00:00.0000000+00:00'},
        @{Field='ProcessWaitMs';Value=-1},@{Field='ProcessWaitMs';Value=[double]::NaN}
    )){
        $copy=@($manifest|ForEach-Object{$_|Select-Object *})
        $copy[0].($mutation.Field)=$mutation.Value
        $results.Add((-not (Test-BackendPairRun $plan $copy 10 1000 300)))
    }
    $results.Add((-not (Test-BackendPairRun $plan @($manifest[0..18]) 10 1000 300)))
    $wrongPlan=@($plan|ForEach-Object{$_|Select-Object *});$wrongPlan[10].Backend='fence'
    $results.Add((-not (Test-BackendPairRun $wrongPlan $manifest 10 1000 300)))
    $overlap=@($manifest|ForEach-Object{$_|Select-Object *});$overlap[1].StartedUtc=$overlap[0].StartedUtc
    $results.Add((-not (Test-BackendPairRun $plan $overlap 10 1000 300)))
    $controls=@(New-BackendPairPlan 10 -SameBackendControls)
    $results.Add(($controls.Count -eq 20 -and @($controls.Case|Select-Object -Unique).Count -eq 20))
    $results.Add((@($controls|Where-Object{$_.Position -eq 0 -and $_.Backend -eq 'fence'}).Count -eq 5 -and @($controls|Where-Object{$_.Position -eq 0 -and $_.Backend -eq 'timeline'}).Count -eq 5))
    $controlManifest=@($manifest|ForEach-Object{$_|Select-Object *})
    for($i=0;$i -lt $controls.Count;$i++){
        foreach($field in @('Case','Validation','Backend','Profile','Repeat','Pair','OrderIndex','Position','Direction')){$controlManifest[$i].$field=$controls[$i].$field}
    }
    $results.Add((Test-BackendPairRun $controls $controlManifest 10 1000 300 -SameBackendControls))
    $results.Add((-not (Test-BackendPairRun $controls $controlManifest 10 1000 300)))
    $wrongControl=@($controlManifest|ForEach-Object{$_|Select-Object *});$wrongControl[1].Backend='timeline'
    $results.Add((-not (Test-BackendPairRun $controls $wrongControl 10 1000 300 -SameBackendControls)))
    $wrongControl=@($controlManifest|ForEach-Object{$_|Select-Object *});$wrongControl[1].Case=$wrongControl[0].Case
    $results.Add((-not (Test-BackendPairRun $controls $wrongControl 10 1000 300 -SameBackendControls)))
    $failed=@($results|Where-Object{$_ -ne $true}).Count
    Write-Host "Backend pair control SelfTest executed=$($results.Count) passed=$($results.Count-$failed); no build/GPU"
    if($failed){throw 'Backend pair control SelfTest failed'}
}
