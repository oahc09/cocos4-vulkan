# Pure wait-phase sidecar checks. Main/phases/present/completion verdicts remain separate.
function Convert-WaitPhaseUInt {
    param([string]$Text)
    $value=[uint64]0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)\z' -and [uint64]::TryParse(
        $Text,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$value)
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Convert-WaitPhaseNumber {
    param([string]$Text)
    $value=0.0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?\z' -and
        [double]::TryParse($Text,[Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,[ref]$value) -and
        -not [double]::IsNaN($value) -and -not [double]::IsInfinity($value) -and $value -ge 0
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Test-WaitPhaseSidecar {
    param([string[]]$WaitLines,[string[]]$MainLines,[string[]]$PresentLines,[string[]]$LogLines,
          [object]$Frames,[string]$Profile)
    $why=New-Object 'System.Collections.Generic.List[string]'
    try {
        if(($Frames -isnot [int] -and $Frames -isnot [long]) -or $Frames -lt 1 -or
           $Frames -gt 100000 -or $Profile -cnotin @('on','off')){
            return [pscustomobject]@{Pass=$false;Reasons=@('invalid expected frames/profile')}
        }
        $wh='frame,unknown_fence_calls,unknown_semaphore_calls,unknown_failures,unknown_wait_ms,acquire_fence_calls,acquire_semaphore_calls,acquire_failures,acquire_wait_ms,record_fence_calls,record_semaphore_calls,record_failures,record_wait_ms,submit_fence_calls,submit_semaphore_calls,submit_failures,submit_wait_ms,observe_fence_calls,observe_semaphore_calls,observe_failures,observe_wait_ms,present_fence_calls,present_semaphore_calls,present_failures,present_wait_ms,acquire_calls,acquire_successes,acquire_present_fence_calls,acquire_other_fence_calls,acquire_present_fence_ms,identity_failures,invalid,total_wait_calls,total_wait_ms'
        $mh='frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight'
        $ph='frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms'
        foreach($g in @(@{Name='wait';Lines=$WaitLines;Header=$wh},
                         @{Name='main';Lines=$MainLines;Header=$mh},
                         @{Name='present';Lines=$PresentLines;Header=$ph})){
            if(-not $g.Lines -or $g.Lines.Count -ne $Frames+1 -or $g.Lines[0] -cne $g.Header -or
               @($g.Lines|Where-Object{[string]::IsNullOrWhiteSpace($_)}).Count){
                $why.Add("invalid $($g.Name) header/count/empty rows")
            }
        }
        $markers=@($LogLines|Where-Object{$_ -imatch 'benchmark\s*wait\s*-?\s*profile'})
        if($markers.Count -ne 1 -or $markers[0] -cne 'benchmark wait-profile: mode=on fault=none capacity=32'){
            $why.Add('missing/duplicate/malformed/faulted wait-profile marker')
        }
        if($why.Count){return [pscustomobject]@{Pass=$false;Reasons=@($why.ToArray())}}
        $max=[decimal][uint64]::MaxValue
        $tolerance=1e-8
        for($i=0;$i -lt $Frames;$i++){
            $w=$WaitLines[$i+1].Split(',');$m=$MainLines[$i+1].Split(',');$p=$PresentLines[$i+1].Split(',')
            if($w.Count -ne 34 -or $m.Count -ne 9 -or $p.Count -ne 10){$why.Add("row $i column count");continue}
            if($w[0] -cne [string]$i -or $m[0] -cne [string]$i -or $p[0] -cne [string]$i -or
               $p[1] -cne $Profile){$why.Add("row $i frame/profile mismatch")}
            $counts=New-Object 'uint64[]' 34
            $times=New-Object 'double[]' 34
            $shapeOk=$true
            for($j=1;$j -lt 34;$j++){
                if($j -in @(4,8,12,16,20,24,29,33)){
                    $v=Convert-WaitPhaseNumber $w[$j];$times[$j]=$v.Value
                }else{
                    $v=Convert-WaitPhaseUInt $w[$j];$counts[$j]=$v.Value
                }
                if(-not $v.Pass){$shapeOk=$false;$why.Add("row $i invalid wait value at column $j")}
            }
            $mainCount=Convert-WaitPhaseUInt $m[3];$mainTime=Convert-WaitPhaseNumber $m[4]
            if(-not $mainCount.Pass -or -not $mainTime.Pass){$shapeOk=$false;$why.Add("row $i invalid main wait values")}
            $presentCounts=@()
            foreach($j in 2..6){$v=Convert-WaitPhaseUInt $p[$j];$presentCounts+=$v.Value
                if(-not $v.Pass){$shapeOk=$false;$why.Add("row $i invalid present counts")}}
            if(-not $shapeOk){continue}
            $total=[decimal]0;$totalMs=0.0
            foreach($start in @(1,5,9,13,17,21)){
                $calls=[decimal]$counts[$start]+[decimal]$counts[$start+1]
                $total+=$calls;$totalMs+=$times[$start+3]
                if($counts[$start+2] -ne 0){$why.Add("row $i phase failure")}
                if($calls -eq 0 -and $times[$start+3] -ne 0){$why.Add("row $i duration without phase calls")}
            }
            if($counts[1] -ne 0 -or $counts[2] -ne 0){$why.Add("row $i unknown-phase waits")}
            if($total -gt $max -or $total -ne [decimal]$counts[32] -or
               $total -ne [decimal]$mainCount.Value){$why.Add("row $i total call closure/overflow")}
            if([double]::IsInfinity($totalMs) -or [Math]::Abs($totalMs-$times[33]) -ge $tolerance -or
               [Math]::Abs($times[33]-$mainTime.Value) -ge $tolerance){$why.Add("row $i total duration closure")}
            if($counts[25] -ne 1 -or $counts[26] -ne 1 -or $counts[30] -ne 0 -or $counts[31] -ne 0){
                $why.Add("row $i acquire/identity/invalid state")
            }
            $acquireTotal=[decimal]$counts[27]+[decimal]$counts[28]
            if($acquireTotal -gt $max -or $acquireTotal -ne [decimal]$counts[5]){
                $why.Add("row $i acquire fence classification closure/overflow")
            }
            if($times[29]-$times[8] -gt $tolerance -or ($counts[27] -eq 0 -and $times[29] -ne 0)){
                $why.Add("row $i acquire present-fence duration")
            }
            if($presentCounts[0] -ne 1 -or $presentCounts[1] -ne 1 -or $presentCounts[4] -ne 0 -or
               $counts[21] -ne $presentCounts[2] -or $counts[22] -ne $presentCounts[3] -or
               $counts[23] -ne $presentCounts[4]){$why.Add("row $i present counter closure")}
            if($Profile -ceq 'on'){
                $presentMs=Convert-WaitPhaseNumber $p[8]
                if(-not $presentMs.Pass -or [Math]::Abs($times[24]-$presentMs.Value) -ge $tolerance){
                    $why.Add("row $i present wait duration closure")
                }
            }elseif($p[7] -cne 'NA' -or $p[8] -cne 'NA' -or $p[9] -cne 'NA'){
                $why.Add("row $i present-profile off durations must be NA")
            }
        }
    }catch{$why.Add('wait sidecar parse/check failed: '+$_.Exception.Message)}
    [pscustomobject]@{Pass=($why.Count -eq 0);Reasons=@($why.ToArray())}
}
