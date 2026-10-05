# Fixtures exercise the same pure function used by the formal matrix.
function Invoke-WaitPhaseJudgeSelfTest {
    $baseWait=New-Object 'string[]' 34
    for($i=0;$i -lt 34;$i++){$baseWait[$i]='0'}
    $baseWait[5]='1';$baseWait[8]='0.1';$baseWait[21]='1';$baseWait[24]='0.2'
    $baseWait[25]='1';$baseWait[26]='1';$baseWait[27]='1';$baseWait[29]='0.1'
    $baseWait[32]='2';$baseWait[33]='0.3'
    $wh='frame,unknown_fence_calls,unknown_semaphore_calls,unknown_failures,unknown_wait_ms,acquire_fence_calls,acquire_semaphore_calls,acquire_failures,acquire_wait_ms,record_fence_calls,record_semaphore_calls,record_failures,record_wait_ms,submit_fence_calls,submit_semaphore_calls,submit_failures,submit_wait_ms,observe_fence_calls,observe_semaphore_calls,observe_failures,observe_wait_ms,present_fence_calls,present_semaphore_calls,present_failures,present_wait_ms,acquire_calls,acquire_successes,acquire_present_fence_calls,acquire_other_fence_calls,acquire_present_fence_ms,identity_failures,invalid,total_wait_calls,total_wait_ms'
    $mh='frame,wall_ms,cpu_ms,wait_calls,wait_ms,fence_objects,semaphore_objects,gpu_clear_ms,in_flight'
    $ph='frame,profile,present_calls,present_successes,fence_wait_calls,timeline_wait_calls,wait_failures,driver_ms,host_wait_ms,host_residual_ms'
    $marker='benchmark wait-profile: mode=on fault=none capacity=32'
    $fixtures=New-Object 'System.Collections.Generic.List[object]'
    $newBase={
        @{WaitLines=@($wh,($baseWait -join ','));MainLines=@($mh,'0,1,0,2,0.3,1,1,0.01,1');
            PresentLines=@($ph,'0,on,1,1,1,0,0,0.4,0.2,0.1');LogLines=@($marker);Frames=1;Profile='on'}
    }
    $add={param($name,$fixture,$expected)$fixtures.Add([pscustomobject]@{Name=$name;Args=$fixture;Expected=$expected})}
    $setWait={param($fixture,$col,$value)$r=$fixture.WaitLines[1].Split(',');$r[$col]=$value;$fixture.WaitLines[1]=$r -join ','}
    & $add 'valid-fence' (& $newBase) $true
    $a=& $newBase;$r=$a.WaitLines[1].Split(',');$r[21]='0';$r[22]='1';$a.WaitLines[1]=$r -join ','
    $a.PresentLines[1]='0,on,1,1,0,1,0,0.4,0.2,0.1'; & $add 'valid-timeline' $a $true
    $a=& $newBase;$a.Profile='off';$a.PresentLines[1]='0,off,1,1,1,0,0,NA,NA,NA'; & $add 'valid-present-profile-off' $a $true
    $a=& $newBase;$r=$a.WaitLines[1].Split(',');$r[5]='0';$r[8]='0';$r[27]='0';$r[29]='0';$r[32]='1';$r[33]='0.2'
    $a.WaitLines[1]=$r -join ',';$a.MainLines[1]='0,1,0,1,0.2,1,1,0.01,1'; & $add 'valid-maintenance-off' $a $true
    $a=& $newBase; & $setWait $a 27 '0'; & $setWait $a 28 '1'; & $setWait $a 29 '0'
    & $add 'valid-other-acquire-fence' $a $true
    $a=& $newBase;$r=$a.WaitLines[1].Split(',');$r[0]='1';$a.WaitLines+= $r -join ','
    $a.MainLines+='1,1,0,2,0.3,1,1,0.01,1';$a.PresentLines+='1,on,1,1,1,0,0,0.4,0.2,0.1';$a.Frames=2
    & $add 'valid-two-frames' $a $true
    $a=& $newBase; & $setWait $a 5 '18446744073709551615'; & $add 'sum-overflow' $a $false
    $a=& $newBase; & $setWait $a 28 '18446744073709551615'; & $add 'classification-sum-overflow' $a $false
    # Deliberate multi-field stress: finite inputs whose addition overflows double.
    $a=& $newBase; & $setWait $a 8 '1e308'; & $setWait $a 24 '1e308'; & $setWait $a 33 '1e308'
    & $add 'finite-duration-sum-overflow-multifield' $a $false
    # Legitimate non-acquire/non-present waits must not be hardcoded to zero.
    foreach($start in @(9,13,17)){
        $a=& $newBase;$r=$a.WaitLines[1].Split(',');$r[$start]='1';$r[$start+3]='0.4';$r[32]='3';$r[33]='0.7'
        $a.WaitLines[1]=$r -join ',';$a.MainLines[1]='0,1,0,3,0.7,1,1,0.01,1'; & $add "valid-phase-$start" $a $true
    }
    foreach($col in @(1,2,3,5,6,7,9,10,11,13,14,15,17,18,19,21,22,23,25,26,27,28,30,31,32)){
        $a=& $newBase; & $setWait $a $col '01'; & $add "noncanonical-count-$col" $a $false
    }
    foreach($value in @('+1','-1',' 1','1 ','18446744073709551616')){
        $a=& $newBase; & $setWait $a 32 $value; & $add "bad-count-$value" $a $false
    }
    foreach($value in @('NaN','Infinity','-0.1','NA','+0.1','01','1e309')){
        $a=& $newBase; & $setWait $a 8 $value; & $add "bad-duration-$value" $a $false
    }
    foreach($col in @(3,7,11,15,19,23,30,31)){
        $a=& $newBase; & $setWait $a $col '1'; & $add "failure-state-$col" $a $false
    }
    foreach($change in @(@(0,'1'),@(1,'1'),@(4,'0.1'),@(12,'0.1'),@(25,'0'),@(26,'0'),@(27,'0'),@(28,'1'),@(29,'0.2'),@(32,'1'),@(33,'0.4'),@(21,'0'))){
        $a=& $newBase; & $setWait $a $change[0] $change[1]; & $add "single-field-$($change[0])" $a $false
    }
    foreach($name in @('missing-wait','missing-main','missing-present','missing-log','wrong-header','extra-row','short-row','main-frame','present-frame','main-count','main-time','present-count','present-time','profile','frames','marker-junk','marker-nospace','marker-duplicate','marker-case','marker-fault')){
        $a=& $newBase
        switch($name){
            'missing-wait'{$a.WaitLines=@()}
            'missing-main'{$a.MainLines=@()}
            'missing-present'{$a.PresentLines=@()}
            'missing-log'{$a.LogLines=@()}
            'wrong-header'{$a.WaitLines[0]+=',junk'}
            'extra-row'{$a.WaitLines+=$a.WaitLines[1]}
            'short-row'{$a.WaitLines[1]='0,0'}
            'main-frame'{$a.MainLines[1]='1,1,0,2,0.3,1,1,0.01,1'}
            'present-frame'{$a.PresentLines[1]='1,on,1,1,1,0,0,0.4,0.2,0.1'}
            'main-count'{$a.MainLines[1]='0,1,0,1,0.3,1,1,0.01,1'}
            'main-time'{$a.MainLines[1]='0,1,0,2,0.4,1,1,0.01,1'}
            'present-count'{$a.PresentLines[1]='0,on,1,1,0,0,0,0.4,0.2,0.1'}
            'present-time'{$a.PresentLines[1]='0,on,1,1,1,0,0,0.4,0.3,0.1'}
            'profile'{$a.Profile='bogus'}
            'frames'{$a.Frames='1'}
            'marker-junk'{$a.LogLines=@('junk '+$marker)}
            'marker-nospace'{$a.LogLines+= 'benchmarkwait-profile: junk'}
            'marker-duplicate'{$a.LogLines+=$marker}
            'marker-case'{$a.LogLines+=$marker.ToUpperInvariant()}
            'marker-fault'{$a.LogLines=@($marker.Replace('fault=none','fault=identity'))}
        }
        & $add $name $a $false
    }
    $executed=0;$passed=0
    foreach($f in $fixtures){
        $fixtureArgs=$f.Args
        $result=Test-WaitPhaseSidecar @fixtureArgs
        $executed++
        if($result.Pass -eq $f.Expected){$passed++}else{Write-Host "FAIL: $($f.Name): $($result.Reasons -join '; ')"}
    }
    Write-Host "Wait-phase SelfTest declared=$($fixtures.Count) executed=$executed passed=$passed"
    if($passed -ne $fixtures.Count -or $executed -ne $fixtures.Count){throw 'Wait-phase fixture failure'}
}
