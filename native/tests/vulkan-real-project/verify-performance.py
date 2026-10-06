"""Offline actual-project performance evidence judge. No GPU/build side effects."""
import argparse, copy, csv, io, json, math, re, statistics
from pathlib import Path
HEADER='frame,main_period_ns,main_active_ns,main_thread_cpu_ns,present_ns,driver_present_ns,present_wait_ns,present_residual_ns,presents,successes,wait_calls,draws,submits,gpu_submitted_cb_ns,gpu_intervals'.split(',')
PREFIX=r'(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?'
NUM=r'(?:0|[1-9][0-9]*)'
def need(value, message):
    if not value: raise ValueError(message)
def uint(value, positive=False):
    need(isinstance(value,str) and re.fullmatch(NUM,value) is not None,'canonical integer')
    n=int(value); need(n<=2**64-1 and (n>0 if positive else True),'integer range'); return n

def judge(log, native, text, phase, policy, frames, warmup, validation, expected=None, launch=None):
    try:
        need(type(native) is int and native==0,'native exit')
        need(phase in ('cold','restored') and policy in ('legacy','modern','encoder') and validation in ('on','off'),'expected configuration')
        need(bool(log) and bool(text),'mandatory log/CSV')
        need(not re.search(r'VUID-[^\s:]+.*(?:Error|ERROR)|SYNC-HAZARD|\[ERROR\]|FAIL:|INCOMPLETE|UNSUPPORTED|SKIPPED',log),'error output')
        lines=log.splitlines()
        def marker(label, pattern):
            family=[l for l in lines if re.search('project-performance'+re.escape(' '+label),l,re.I)]
            need(len(family)==1,'unique '+label)
            match=re.fullmatch(PREFIX+pattern,family[0]);need(match is not None,'exact '+label);return match
        need(len([l for l in lines if re.search('project-performance',l,re.I)])==8,'complete performance marker family')
        marker('scene',rf'project-performance scene: name=atlas-compress ready=1 warmup={warmup} frames={frames}')
        cache=marker('cache',rf'project-performance cache: phase={phase} loaded={0 if phase=="cold" else 1} initialBytes=({NUM}) inputExact=1')
        initial=uint(cache[1]);need(initial==0 if phase=='cold' else initial>0 and expected==initial,'cold/warm input association')
        metadata=marker('metadata',rf'project-performance metadata: width=([1-9][0-9]*) height=([1-9][0-9]*) requestedImages=([1-9][0-9]*) presentMode=({NUM}) fps=1000 displayHz=([1-9][0-9]*) timestampPeriod=([0-9.eE+-]+)')
        need(math.isfinite(float(metadata[6])) and float(metadata[6])>0,'timestamp period')
        first=marker('first-use',rf'project-performance first-use: nativeEntryToPresentNs=({NUM}) firstGraphicsFactoryNs=({NUM}) firstShaderCompileNs=({NUM}) firstGraphicsDriverNs=({NUM})')
        first_values=[uint(v,True) for v in first.groups()];need(first_values[1]>=first_values[2] and first_values[1]>=first_values[3],'first-use interval bounds')
        startup_names=['backendNs','instanceNs','deviceNs','cacheConfigureNs','firstMainFrameNs','totalShaderCompileNs','totalGraphicsFactoryNs','totalGraphicsDriverNs','maxGraphicsFactoryNs','maxShaderCompileNs','factories','shaderCompiles','pipelineCalls','firstPresentQpc','qpcFrequency']
        startup=marker('startup',r'project-performance startup: '+' '.join(n+'=('+NUM+')' for n in startup_names))
        startup_values={k:uint(v,True) for k,v in zip(startup_names,startup.groups())}
        need(startup_values['backendNs']>=startup_values['instanceNs']+startup_values['deviceNs'],'backend interval bounds')
        need(startup_values['totalShaderCompileNs']>=startup_values['maxShaderCompileNs']>=first_values[2] and startup_values['totalGraphicsFactoryNs']>=startup_values['maxGraphicsFactoryNs']>=first_values[1] and startup_values['totalGraphicsDriverNs']>=first_values[3],'compilation interval bounds')
        need(isinstance(launch,dict) and type(launch.get('LaunchQpc')) is int and type(launch.get('QpcFrequency')) is int,'mandatory launch clock')
        need(0<launch['LaunchQpc']<startup_values['firstPresentQpc'] and launch['QpcFrequency']==startup_values['qpcFrequency'],'same Windows QPC epoch/frequency')
        launch_ns=(startup_values['firstPresentQpc']-launch['LaunchQpc'])*1000000000//launch['QpcFrequency']
        need(launch_ns>=first_values[0],'launch includes diagnostic entry')
        saved=uint(marker('saved',rf'project-performance saved: bytes=({NUM})')[1],True)
        marker('completed',rf'project-performance completed: frames={frames} failures=0 liveErrors=0 postErrors=0 outcome=CLEAN')
        footer=[l for l in lines if re.search(r'completed',l,re.I)];need(len(footer)==1,'foreign footer')
        leaks=[l for l in lines if re.search('Total device memory leaked',l,re.I)]
        need(len(leaks)==1 and re.fullmatch(PREFIX+r'Total device memory leaked: 0 bytes\.',leaks[0]),'actual leak zero')
        apis=[l for l in lines if re.search(r'Vulkan\s*API',l,re.I)];need(len(apis)==1 and re.fullmatch(PREFIX+r'Vulkan API: loader=[0-9]+\.[0-9]+ requested=1\.3 XR=1\.3 instance=1\.3 physical=[0-9]+\.[0-9]+ effective=1\.3',apis[0]),'actual effective API')
        renderers=[l for l in lines if re.search(r'RENDERER:',l,re.I)];need(len(renderers)==1,'unique physical renderer');renderer=re.fullmatch(PREFIX+r'RENDERER: (.+)',renderers[0]);need(renderer is not None,'actual renderer')
        paths=[l for l in lines if re.search(r'Vulkan\s*paths',l,re.I)];need(len(paths)==1,'unique runtime paths')
        bits='0' if policy=='legacy' else '1'
        dynamic='1' if policy=='modern' else '0'
        need(re.fullmatch(PREFIX+rf'Vulkan paths: timeline=1 sync2={bits} submit2={bits} dynamicRendering={dynamic} classicOverride={0 if policy=="modern" else 1} maintenance4=0',paths[0]),'actual policy paths')
        marker('validation',r'project-performance validation: '+('ENABLED' if validation=='on' else 'DISABLED'))
        reader=csv.DictReader(io.StringIO(text));need(reader.fieldnames==HEADER,'exact CSV header');rows=list(reader);need(len(rows)==frames,'exact frame count')
        parsed=[]
        for i,row in enumerate(rows):
            need(set(row)==set(HEADER) and None not in row.values(),'complete row')
            values={k:uint(row[k]) for k in HEADER};need(row['frame']==str(i),'frame order')
            need(all(values[k]>0 for k in ('main_period_ns','main_active_ns','present_ns','draws','submits','gpu_intervals','gpu_submitted_cb_ns')),'real frame work')
            need(values['presents']==1 and values['successes']==1,'one successful present')
            need(values['draws']==3 and values['submits']==1 and values['gpu_intervals']==3,'same actual atlas submit/draw topology')
            need(values['present_ns']==sum(values[k] for k in ('driver_present_ns','present_wait_ns','present_residual_ns')),'present partition closure')
            need(values['present_wait_ns']==0 or values['wait_calls']>0,'wait classification')
            parsed.append(values)
        return {'Pass':True,'SavedBytes':saved,'InitialBytes':initial,'Metadata':list(metadata.groups()),'Renderer':renderer[1],'FirstUse':first_values,'Startup':startup_values,'LaunchToPresentNs':launch_ns,'Rows':parsed}
    except (ValueError,TypeError,IndexError,KeyError,OverflowError) as error:
        return {'Pass':False,'SavedBytes':0,'Reasons':[str(error)]}

def self_test(sample, frames=40, warmup=10):
    args=[sample['log'],0,sample['csv'],'cold','modern',frames,warmup,'on',None,sample['launch']]
    need(judge(*args)['Pass'],'real canonical fixture')
    fixtures=[]
    for label,old,new in [
        ('zero frames',f'frames={frames} failures=0','frames=0 failures=0'),('live','liveErrors=0','liveErrors=1'),('post','postErrors=0','postErrors=1'),('outcome','outcome=CLEAN','outcome=FAILED'),
        ('runtime','sync2=1','sync2=0'),('native phase','phase=cold loaded=0','phase=restored loaded=0'),('initial','initialBytes=0','initialBytes=1'),('saved','saved: bytes=65694','saved: bytes=0'),('API','effective=1.3','effective=1.30'),('payload','inputExact=1','inputExact=0')]:
        b=args.copy();need(old in b[0],'mutation exists '+label);b[0]=b[0].replace(old,new,1);fixtures.append((label,b))
    b=args.copy();b[-1]=None;fixtures.append(('missing launch clock',b))
    for label,line in [('foreign','foo completed failures=1'),('malformed','project-performance_xxx bad'),('VUID','VUID-fake: Error'),('hazard','SYNC-HAZARD-WRITE-AFTER-WRITE'),('FAIL','12:00:00 [ERROR]: FAIL: evidence'),('truncated foreign footer','foo completed bad')]:
        b=args.copy();b[0]+='\n'+line;fixtures.append((label,b))
    for label,index,value in [('boolean exit',1,False),('native',1,1),('missing CSV',2,''),('missing log',0,''),('unknown policy',4,'bad')]:
        b=args.copy();b[index]=value;fixtures.append((label,b))
    rows=list(csv.reader(io.StringIO(args[2])))
    for col,val in [('frame','01'),('draws','0'),('submits','0'),('gpu_intervals','0'),('draws','4'),('submits','2'),('gpu_intervals','4'),('gpu_submitted_cb_ns','0'),('presents','2'),('successes','0'),('present_residual_ns','18446744073709551616'),('main_period_ns','+1')]:
        b=args.copy();r=copy.deepcopy(rows);r[1][HEADER.index(col)]=val;stream=io.StringIO();csv.writer(stream,lineterminator='\n').writerows(r);b[2]=stream.getvalue();fixtures.append((col,b))
    for label,b in fixtures:need(not judge(*b)['Pass'],'reject '+label)
    return len(fixtures)+1

def main():
    parser=argparse.ArgumentParser();parser.add_argument('root',type=Path);parser.add_argument('--self-test',action='store_true');a=parser.parse_args();r=a.root
    if a.self_test and not (r/'manifest.json').exists():
        sample={'log':(r/'debug-output.log').read_text(),'csv':(r/'performance.csv').read_text(),'launch':json.loads((r/'launch-clock.json').read_text())};print('fixtures passed='+str(self_test(sample)));return
    manifest=json.loads((r/'manifest.json').read_text());need(isinstance(manifest,list) and len(manifest)>0,'manifest')
    if a.self_test:
        canonical=next((v for v in manifest if v['Phase']=='cold' and v['Policy']=='modern' and v['Validation']=='on'),None)
        need(canonical is not None,'canonical actual modern ON fixture')
        d=r/canonical['Case'];sample={'log':(d/'debug-output.log').read_text(),'csv':(d/'performance.csv').read_text(),'launch':json.loads((d/'launch-clock.json').read_text())}
        print('fixtures passed='+str(self_test(sample,canonical['Frames'],canonical['Warmup'])));return
    status=json.loads((r/'run-status.json').read_text())
    need(status.get('Complete') is True and status.get('Pass') is True and status.get('SourceUnchanged') is True,'terminal frozen matrix')
    need(type(status.get('Repeats')) is int and status['Repeats']>=5 and status['Frames']>=1000 and status['Warmup']>=300,'formal sampling bounds')
    expected_cases={(v,p,n,f) for v in ('on','off') for p in ('legacy','modern') for n in range(1,status['Repeats']+1) for f in ('cold','restored')}
    need(len(manifest)==len(expected_cases) and {(v['Validation'],v['Policy'],v['Repeat'],v['Phase']) for v in manifest}==expected_cases,'complete formal case matrix')
    need(json.loads((r/'source-before.json').read_text())==json.loads((r/'source-after.json').read_text()),'frozen source identity')
    results=[];groups={};cases=set();cold={}
    for item in manifest:
        name=item['Case'];need(name not in cases,'unique case');cases.add(name);d=r/name
        need(item['Pass'] is True and item['NativeExit']==0 and item['WrapperExit']==0 and item['Frames']==status['Frames'] and item['Warmup']==status['Warmup'],'successful actual manifest case')
        log=(d/'debug-output.log').read_text(encoding='utf-8');summary=json.loads((d/'runtime-summary.json').read_text());native=summary['exitCode'];need(summary['exeSHA256']==item['ExeSha256'],'actual exe identity');text=(d/'performance.csv').read_text()
        pair=(item['Validation'],item['Policy'],item['Repeat']);expected=cold.get(pair)
        j=judge(log,native,text,item['Phase'],item['Policy'],item['Frames'],item['Warmup'],item['Validation'],expected,json.loads((d/'launch-clock.json').read_text()));need(j['Pass'],name+': '+str(j.get('Reasons')))
        if item['Phase']=='cold':cold[pair]=j['SavedBytes']
        for metric in HEADER[1:8]+['gpu_submitted_cb_ns']:
            values=[row[metric]/1000 for row in j['Rows']];ordered=sorted(values)
            groups.setdefault((item['Validation'],item['Policy'],item['Phase'],metric),[]).append({'Repeat':item['Repeat'],'MeanUs':statistics.mean(values),'MedianUs':statistics.median(values),'P95Us':ordered[math.ceil(.95*len(values))-1],'P99Us':ordered[math.ceil(.99*len(values))-1],'StddevUs':statistics.pstdev(values),'MADUs':statistics.median(abs(v-statistics.median(values)) for v in values)})
        results.append({'Case':name,**{k:v for k,v in j.items() if k!='Rows'}})
    output={'Executor':'Lead','Complete':True,'Cases':results,'Groups':[{'Validation':k[0],'Policy':k[1],'Phase':k[2],'Metric':k[3],'Samples':v} for k,v in groups.items()],'GateGranted':False}
    (r/'judgment.json').write_text(json.dumps(output,indent=2))
    print('actual project judge PASS cases='+str(len(results)))
if __name__=='__main__':main()
