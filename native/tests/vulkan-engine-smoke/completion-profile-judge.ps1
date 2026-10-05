# Pure additional-sidecar checks. Existing main/phase/present checks run separately.
function Convert-CompletionUInt {
    param([string]$Text)
    $value=[uint64]0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)\z' -and [uint64]::TryParse(
        $Text,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$value)
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Convert-CompletionNumber {
    param([string]$Text)
    $value=0.0
    $ok=$Text -cmatch '^(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?\z' -and
        [double]::TryParse($Text,[Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,[ref]$value) -and
        -not [double]::IsNaN($value) -and -not [double]::IsInfinity($value) -and $value -ge 0
    [pscustomobject]@{Pass=$ok;Value=$value}
}
function Test-CompletionSidecar {
    param([string[]]$CompletionLines,[string[]]$PresentLines,[string[]]$LogLines,
          [object]$Frames,[string]$Profile)
    $why=New-Object 'System.Collections.Generic.List[string]'
    try {
        if(($Frames -isnot [int] -and $Frames -isnot [long]) -or $Frames -lt 1 -or
           $Frames -gt 100000 -or $Profile -cnotin @('on','off')){
            return [pscustomobject]@{Pass=$false;Reasons=@('invalid expected frames/profile')}
        }
        $headers=@(
            'frame,profile,fence_status_calls,fence_not_ready,counter_calls,reset_fence_calls,query_failures,completion_api_ms,host_other_ms',
            'frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms')
        $groups=@(@{Name='completion';Lines=$CompletionLines;Header=$headers[0]},
                  @{Name='present';Lines=$PresentLines;Header=$headers[1]})
        foreach($g in $groups){
            if(-not $g.Lines -or $g.Lines.Count -ne $Frames+1 -or $g.Lines[0] -cne $g.Header -or
               @($g.Lines|Where-Object{[string]::IsNullOrWhiteSpace($_)}).Count){
                $why.Add("invalid $($g.Name) header/count/empty rows")
            }
        }
        if(-not $LogLines){$why.Add('missing log')}
        if($why.Count){return [pscustomobject]@{Pass=$false;Reasons=@($why.ToArray())}}
        $whole=@($LogLines|Where-Object{$_ -imatch 'completion\s*-?\s*profile\s*-?\s*dispatch'})
        $dispatch=@()
        if($whole.Count -ne 1 -or $whole[0] -cnotmatch '^completion-profile-dispatch: whole-run fenceStatus=(0|[1-9][0-9]*) counter=(0|[1-9][0-9]*) resetFence=(0|[1-9][0-9]*)\z'){
            $why.Add('missing/duplicate/malformed completion dispatch')
        }else{
            $raw=@($Matches[1],$Matches[2],$Matches[3])
            foreach($text in $raw){$n=Convert-CompletionUInt $text;$dispatch+=$n
                if(-not $n.Pass){$why.Add('dispatch integer overflow')}
            }
        }
        # Decimal arithmetic is exact over all UInt64 sums; never let PowerShell
        # silently promote an overflowing integer addition to a rounded double.
        $sums=@([decimal]0,[decimal]0,[decimal]0)
        $max=[decimal][uint64]::MaxValue
        for($i=0;$i -lt $Frames;$i++){
            $c=$CompletionLines[$i+1].Split(',');$p=$PresentLines[$i+1].Split(',')
            if($c.Count -ne 9 -or $p.Count -ne 10){$why.Add("row $i column count");continue}
            if($c[0] -cne [string]$i -or $p[0] -cne [string]$i -or
               $c[1] -cne $Profile -or $p[1] -cne $Profile){$why.Add("row $i frame/profile mismatch")}
            $ints=@();$intsOk=$true
            foreach($n in 2..6){$v=Convert-CompletionUInt $c[$n];$ints+=$v
                if(-not $v.Pass){$intsOk=$false;$why.Add("row $i invalid completion integer")}}
            if($intsOk){
                if($ints[1].Value -gt $ints[0].Value){$why.Add("row $i NOT_READY exceeds status calls")}
                if($c[6] -cne '0'){$why.Add("row $i query failures")}
                $values=@($ints[0].Value,$ints[2].Value,$ints[3].Value)
                for($j=0;$j -lt 3;$j++){
                    $sum=$sums[$j]+[decimal]$values[$j]
                    if($sum -gt $max){$why.Add("row $i sample count overflow")}else{$sums[$j]=$sum}
                }
            }
            foreach($n in 2..6){if(-not (Convert-CompletionUInt $p[$n]).Pass){$why.Add("row $i invalid present integer")}}
            if($p[2] -cne '1' -or $p[3] -cne '1' -or $p[6] -cne '0'){$why.Add("row $i present result counters")}
            if($Profile -ceq 'off'){
                if($c[7] -cne 'NA' -or $c[8] -cne 'NA' -or
                   $p[7] -cne 'NA' -or $p[8] -cne 'NA' -or $p[9] -cne 'NA'){$why.Add("row $i off durations must be NA")}
            }else{
                $api=Convert-CompletionNumber $c[7];$other=Convert-CompletionNumber $c[8]
                $parent=Convert-CompletionNumber $p[9]
                foreach($n in 7..8){if(-not (Convert-CompletionNumber $p[$n]).Pass){$why.Add("row $i invalid present duration")}}
                if(-not ($api.Pass -and $other.Pass -and $parent.Pass)){$why.Add("row $i invalid completion/parent duration")}
                else {
                    $total=$api.Value+$other.Value
                    if([double]::IsInfinity($total) -or [math]::Abs($total-$parent.Value) -ge 1e-8){$why.Add("row $i completion partition mismatch")}
                    if($api.Value -gt 0 -and $intsOk -and $ints[0].Value -eq 0 -and
                       $ints[2].Value -eq 0 -and $ints[3].Value -eq 0){$why.Add("row $i API span without calls")}
                }
            }
        }
        if($dispatch.Count -eq 3){for($j=0;$j -lt 3;$j++){
            if($dispatch[$j].Pass -and [decimal]$dispatch[$j].Value -lt $sums[$j]){$why.Add('dispatch below sample total')}
        }}
    }catch{$why.Add("invalid sidecar evidence: $($_.Exception.Message)")}
    [pscustomobject]@{Pass=($why.Count -eq 0);Reasons=@($why.ToArray())}
}
