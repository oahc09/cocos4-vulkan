"""Build and verify actual-project Dynamic post scopes; no performance claims."""
import argparse,hashlib,importlib.util,json,re,subprocess,sys,time
from pathlib import Path
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[2]
PROJECT=Path('E:/AIProMax/github/cocos4-vulkan-test-project');CMAKE='D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe'
def dump(p,v):p.write_text(json.dumps(v,indent=2))
def main():
 p=argparse.ArgumentParser();p.add_argument('--root',required=True,type=Path);a=p.parse_args();r=a.root.resolve();r.mkdir()
 spec=importlib.util.spec_from_file_location('j',HERE/'verify-rendering-features.py');j=importlib.util.module_from_spec(spec);spec.loader.exec_module(j)
 sources=[HERE/n for n in ['RealProjectScopeObservation.h','RealProjectResolve.cpp','RealProjectColorResolve.cpp','RealProjectDepthResolve.cpp','collect-performance-runtime.py','verify-rendering-features.py','run-scope-matrix.py']]+[REPO/'native/cocos/renderer/gfx-vulkan/VKCommandBuffer.cpp']
 sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();before={str(p):sha(p) for p in sources};dump(r/'source-before.json',before)
 tree=PROJECT/'build/windows/proj';exe=tree/'Debug/test-cases.exe'
 with (r/'build.log').open('w') as f:code=subprocess.run([CMAKE,'--build',str(tree),'--config','Debug','--target','test-cases','--parallel','4'],stdout=f,stderr=f).returncode
 if code:raise RuntimeError('fresh build failed; no old exe execution')
 exe_sha=sha(exe);state={'Executor':'Lead','Complete':False,'Pass':False,'BuildExit':code,'ExeSha256':exe_sha,'Attempted':0,'Passed':0};manifest=[];fixtures=[]
 for barrier in ['legacy','sync2']:
  for mode in ['color2','color4','depth2','depth4']:
   d=r/(barrier+'-'+mode);d.mkdir();env={'CC_VULKAN_FORCE_API':'1.3','CC_VULKAN_COMPLETION':'auto','CC_VULKAN_BARRIER':barrier,'CC_VULKAN_SUBMIT':'legacy','CC_VULKAN_RENDERING':'dynamic','CC_VULKAN_REAL_PROJECT_RESOLVE':mode,'CC_VULKAN_REAL_PROJECT_SCOPES':'post'}
   dump(d/'config.json',{'Actor':'Lead','exe':str(exe),'env':env,'closeAfterSeconds':90,'expectedExit':0});state['Attempted']+=1;dump(r/'run-status.json',state)
   print('RUN '+d.name,flush=True)
   with (d/'collector.log').open('w') as f:
    proc=subprocess.Popen([sys.executable,str(HERE/'collect-performance-runtime.py'),str(d)],stdout=f,stderr=f);deadline=time.monotonic()+180
    while proc.poll() is None:
     logpath=d/'debug-output.log'
     if logpath.exists() and 'project-resolve scopes:' in logpath.read_text(encoding='utf-8'):(d/'close-request').touch()
     if time.monotonic()>deadline:raise RuntimeError('collector timeout; PID evidence retained')
     time.sleep(.1)
   native=json.loads((d/'runtime-summary.json').read_text());text=(d/'debug-output.log').read_text();kind=mode[:-1];samples=int(mode[-1]);result={'Pass':False}
   try:
    j.require(proc.returncode==0 and native['exeSHA256']==exe_sha,'current executable')
    j.only(j.lines(text),'Vulkan API:',r'Vulkan API: loader=[0-9]+\.[0-9]+ requested=1.3 XR=1.3 instance=1.3 physical=[0-9]+\.[0-9]+ effective=1.3')
    j.only(j.lines(text),'Vulkan paths:',rf'Vulkan paths: timeline=1 sync2={int(barrier=="sync2")} submit2=0 dynamicRendering=1 classicOverride=0 maintenance4=[01]')
    result={'Pass':j.resolve_native(native,text,kind,samples,scopes=True)}
   except (ValueError,KeyError,TypeError) as error:result={'Pass':False,'Reasons':[str(error)]}
   dump(d/'judgment.json',result);manifest.append({'Case':d.name,'NativeExit':native['exitCode'],'Pass':result['Pass']});dump(r/'manifest.json',manifest);state['Passed']+=int(result['Pass']);dump(r/'run-status.json',state);print('RESULT '+d.name+' '+str(result),flush=True)
   if not result['Pass']:raise RuntimeError('scope case failed')
   if len(manifest)==1:
    mutations=[('posts=4','posts=0'),('precise=4','precise=3'),('broad=0','broad=1'),('failures=0','failures=1')]
    scope='project-resolve scopes: posts=4 precise=4 broad=0 failures=0'
    cases=[text.replace(scope,scope.replace(x,y),1) for x,y in mutations]+[text.replace(scope,scope+' junk',1),text+'\n'+scope,text.replace(scope,'project-resolvescopes: malformed',1),text.replace(scope,'')]
    for log in cases:
     try:j.resolve_native(native,log,kind,samples,scopes=True);fixtures.append(False)
     except (ValueError,KeyError,TypeError):fixtures.append(True)
    j.require(all(fixtures),'eight scope evidence mutations must be rejected')
 after={str(p):sha(p) for p in sources};state.update(Complete=True,Pass=state['Passed']==8 and before==after,SourceUnchanged=before==after,Fixtures={'Declared':len(fixtures),'Executed':len(fixtures),'Passed':sum(fixtures)})
 dump(r/'source-after.json',after);dump(r/'run-status.json',state);j.require(state['Pass'],'complete frozen scope matrix')
if __name__=='__main__':main()
