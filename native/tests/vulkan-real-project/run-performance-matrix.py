"""Build and sample the user's actual Windows project; opt-in diagnostics only."""
import argparse, importlib.util, json, subprocess, sys, hashlib, datetime, time, shutil
from pathlib import Path
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('judge',HERE/'verify-performance.py');j=importlib.util.module_from_spec(spec);spec.loader.exec_module(j)
PROJECT=Path('E:/AIProMax/github/cocos4-vulkan-test-project')
CMAKE='D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe'
TREE={'on':PROJECT/'build/windows/proj','off':PROJECT/'build/windows/proj-perf-off'}
def dump(path,data):path.write_text(json.dumps(data,indent=2),encoding='utf-8')
def main():
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--repeats',type=int,default=5);p.add_argument('--frames',type=int,default=1000);p.add_argument('--warmup',type=int,default=300);a=p.parse_args()
 j.need(a.repeats>=5 and a.frames>=1000 and a.warmup>=300 and a.frames+a.warmup<8192,'formal minimum bounds')
 r=a.root.resolve();j.need(not r.exists(),'unique new evidence root');r.mkdir(parents=True)
 sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 sources=list(HERE.glob('RealProject*.cpp'))+list(HERE.glob('RealProject*.h'))+[HERE/'verify-performance.py',HERE/'run-performance-matrix.py',HERE/'collect-performance-runtime.py']
 repo=HERE.parents[1];sources += [repo/'cocos/renderer/gfx-vulkan'/n for n in ['VKDevice.cpp','VKGPUContext.cpp','VKPipelineState.cpp','VKShader.cpp','VKMeasurementProbe.h','VKCommands.cpp']]
 sources += [PROJECT/'native/engine'/n for n in ['win64/main.cpp','common/Classes/Game.cpp','win64/CMakeLists.txt']]
 before={str(f):sha(f) for f in sources};dump(r/'source-before.json',before)
 status={'Executor':'Lead','Complete':False,'Pass':False,'Frames':a.frames,'Warmup':a.warmup,'Repeats':a.repeats,'Attempted':0,'Passed':0};dump(r/'run-status.json',status)
 for val,tree in TREE.items():
  with (r/(val+'-build.log')).open('w',encoding='utf-8') as f:code=subprocess.run([CMAKE,'--build',str(tree),'--config','Debug','--target','test-cases','--parallel','4'],stdout=f,stderr=f).returncode
  status[val+'BuildExit']=code;dump(r/'run-status.json',status);j.need(code==0,'fresh target build '+val)
 exes={v:sha(t/'Debug/test-cases.exe') for v,t in TREE.items()};dump(r/'exe-sha.json',exes)
 manifest=[];failures=[]
 for rep in range(1,a.repeats+1):
  vals=['on','off'] if rep%2 else ['off','on'];policies=['legacy','modern'] if rep%2 else ['modern','legacy']
  for val in vals:
   for policy in policies:
    pair=r/f'{val}-{policy}-rep{rep}';pair.mkdir();cache=pair/'cache.bin';saved=None
    for phase in ['cold','restored']:
     if phase=='restored' and not saved:failures.append({'Pair':pair.name,'Reason':'cold link unavailable'});continue
     name=f'{val}-{policy}-rep{rep}-{phase}';d=r/name;d.mkdir();exe=TREE[val]/'Debug/test-cases.exe'
     env={'CC_VULKAN_FORCE_API':'1.3','CC_VULKAN_COMPLETION':'auto','CC_VULKAN_BARRIER':'sync2' if policy=='modern' else 'legacy','CC_VULKAN_SUBMIT':'submit2' if policy=='modern' else 'legacy','CC_VULKAN_RENDERING':'dynamic' if policy=='modern' else 'classic','CC_VULKAN_REAL_PROJECT_PERFORMANCE':phase,'CC_VULKAN_REAL_PROJECT_PERF_CACHE':str(cache),'CC_VULKAN_REAL_PROJECT_PERF_FRAMES':str(a.frames),'CC_VULKAN_REAL_PROJECT_PERF_WARMUP':str(a.warmup),'CC_VULKAN_REAL_PROJECT_PERF_FPS':'1000'}
     dump(d/'config.json',{'Actor':'Lead','exe':str(exe),'env':env,'closeAfterSeconds':180,'expectedExit':0})
     item={'Case':name,'Validation':val,'Policy':policy,'Repeat':rep,'Phase':phase,'Frames':a.frames,'Warmup':a.warmup,'StartedUTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'ExeSha256':exes[val]};manifest.append(item);dump(r/'manifest.json',manifest)
     smi=shutil.which('nvidia-smi');
     if smi:
      sensor=subprocess.run([smi,'--query-gpu=index,name,driver_version,temperature.gpu,pstate,clocks.current.graphics,power.draw,utilization.gpu','--format=csv,noheader,nounits'],capture_output=True,text=True);dump(d/'gpu-before.json',{'Exit':sensor.returncode,'Output':sensor.stdout,'Error':sensor.stderr,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
     print('RUN '+name,flush=True);status['Attempted']+=1;dump(r/'run-status.json',status)
     with (d/'collector.log').open('w',encoding='utf-8') as f:code=subprocess.run([sys.executable,str(HERE/'collect-performance-runtime.py'),str(d)],stdout=f,stderr=f,timeout=300).returncode
     if smi:
      sensor=subprocess.run([smi,'--query-gpu=index,name,driver_version,temperature.gpu,pstate,clocks.current.graphics,power.draw,utilization.gpu','--format=csv,noheader,nounits'],capture_output=True,text=True);dump(d/'gpu-after.json',{'Exit':sensor.returncode,'Output':sensor.stdout,'Error':sensor.stderr,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
     native=json.loads((d/'runtime-summary.json').read_text());text=(d/'performance.csv').read_text() if (d/'performance.csv').exists() else '';log=(d/'debug-output.log').read_text(encoding='utf-8')
     judged=j.judge(log,native['exitCode'],text,phase,policy,a.frames,a.warmup,val,saved,json.loads((d/'launch-clock.json').read_text()));dump(d/'judgment.json',judged if not judged['Pass'] else {k:v for k,v in judged.items() if k!='Rows'})
     item['NativeExit']=native['exitCode'];item['WrapperExit']=code;item['FinishedUTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();item['Pass']=code==0 and judged['Pass'] and native['exeSHA256']==exes[val] and native['errorLogMessages']==0 and all(x['firstChance'] for x in native['exceptions']);dump(r/'manifest.json',manifest)
     if item['Pass']:status['Passed']+=1;saved=judged['SavedBytes'] if phase=='cold' else saved
     else:failures.append({'Case':name,'Reason':judged.get('Reasons')});saved=None
     dump(r/'run-status.json',status);print('RESULT '+name+' '+str(item['Pass']),flush=True)
 after={str(f):sha(f) for f in sources};status.update({'Complete':True,'SourceUnchanged':before==after,'Pass':not failures and before==after and status['Passed']==a.repeats*8,'Failures':failures});dump(r/'source-after.json',after);dump(r/'run-status.json',status)
 j.need(status['Pass'],'formal matrix failed');subprocess.run([sys.executable,str(HERE/'verify-performance.py'),str(r)],check=True)
if __name__=='__main__':main()
