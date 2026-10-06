"""Same actual-project executable controls for upload-batch variance."""
import argparse,datetime,hashlib,importlib.util,json,subprocess,sys,csv,statistics,math,shutil
from pathlib import Path
HERE=Path(__file__).resolve().parent
SHA=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def dump(p,v):p.write_text(json.dumps(v,indent=2),encoding="utf-8")
def main():
 p=argparse.ArgumentParser();p.add_argument("--root",type=Path,required=True)
 g=p.add_mutually_exclusive_group(required=True);g.add_argument("--build-evidence",type=Path);g.add_argument("--fresh-build",action="store_true");a=p.parse_args()
 root=a.root.resolve()
 if root.exists():raise ValueError("new evidence root required")
 exe=Path("E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj-perf-off/Debug/test-cases.exe")
 if a.fresh_build:
  root.mkdir(parents=True)
  files=[HERE/"RealProjectVisibility.cpp",HERE/"verify-upload-performance.py",HERE/"collect-performance-runtime.py",HERE/"run-upload-controls.py",HERE.parents[2]/"native/cocos/renderer/gfx-vulkan/VKCommands.cpp"]
  frozen={str(f):SHA(f) for f in files};dump(root/"source-before.json",frozen)
  with (root/"build.log").open("w",encoding="utf-8") as log:
   code=subprocess.run(["D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe","--build",str(exe.parent.parent),"--config","Debug","--target","test-cases","--parallel","4"],stdout=log,stderr=log).returncode
  dump(root/"build-result.json",{"Exit":code})
  if code or any(SHA(Path(f))!=sha for f,sha in frozen.items()):raise RuntimeError("fresh build failed or source changed; old executable refused")
  evidence=root; (root/"current.exe").write_bytes(exe.read_bytes())
 else:
  evidence=a.build_evidence.resolve()
  old=json.loads((evidence/"run-status.json").read_text())
  if not(old["Complete"] and old["Pass"] and old["SourceRestored"] and old["CurrentExeRestored"] and old["SourceUnchanged"]):raise ValueError("valid current build evidence required")
  frozen=json.loads((evidence/"source-before.json").read_text())
  if any(SHA(Path(f))!=sha for f,sha in frozen.items()):raise ValueError("source differs from fresh built binary")
  if SHA(exe)!=SHA(evidence/"current.exe"):raise ValueError("executable differs from built current artifact")
 root.mkdir(parents=True,exist_ok=a.fresh_build);exe_sha=SHA(exe)
 spec=importlib.util.spec_from_file_location("judge",HERE/"verify-upload-performance.py");judge=importlib.util.module_from_spec(spec);spec.loader.exec_module(judge)
 state={"Actor":"Lead","BuildEvidence":str(evidence),"ExeSha256":exe_sha,"Complete":False,"Pass":False,"Attempted":0,"Passed":0};manifest=[];samples=[]
 smi=shutil.which("nvidia-smi")
 def sensor(d,name):
  if smi:
   q=subprocess.run([smi,"--query-gpu=index,name,temperature.gpu,pstate,clocks.current.graphics,power.draw,utilization.gpu","--format=csv,noheader,nounits"],capture_output=True,text=True)
   dump(d/name,{"Exit":q.returncode,"Output":q.stdout,"Error":q.stderr,"UTC":datetime.datetime.now(datetime.timezone.utc).isoformat()})
 for rep in range(1,6):
  for member in (("first","second") if rep%2 else ("second","first")):
   name=f"rep{rep}-{member}";d=root/name;d.mkdir()
   env={"CC_VULKAN_FORCE_API":"1.3","CC_VULKAN_COMPLETION":"auto","CC_VULKAN_BARRIER":"legacy","CC_VULKAN_SUBMIT":"legacy","CC_VULKAN_RENDERING":"classic","CC_VULKAN_REAL_PROJECT_VISIBILITY":"upload-perf"}
   dump(d/"config.json",{"Actor":"Lead","exe":str(exe),"env":env,"closeAfterSeconds":5,"expectedExit":0})
   item={"Case":name,"Repeat":rep,"Member":member,"ExeSha256":exe_sha,"StartedUTC":datetime.datetime.now(datetime.timezone.utc).isoformat()};manifest.append(item)
   state["Attempted"]+=1;dump(root/"run-status.json",state);dump(root/"manifest.json",manifest);print("RUN",name,flush=True);sensor(d,"gpu-before.json")
   with (d/"collector.log").open("w",encoding="utf-8") as f:code=subprocess.run([sys.executable,str(HERE/"collect-performance-runtime.py"),str(d)],stdout=f,stderr=f,timeout=300).returncode
   sensor(d,"gpu-after.json")
   summary=json.loads((d/"runtime-summary.json").read_text());text=(d/"debug-output.log").read_text(encoding="utf-8");data=(d/"upload-performance.csv").read_text() if (d/"upload-performance.csv").exists() else ""
   result=judge.judge(summary,text,data,"upload-perf","off"); good=code==0 and result["Pass"] and summary["exeSHA256"]==exe_sha
   item.update(Pass=good,Exit=summary["exitCode"],FinishedUTC=datetime.datetime.now(datetime.timezone.utc).isoformat());dump(d/"judgment.json",{k:v for k,v in result.items() if k!="Rows"});dump(root/"manifest.json",manifest)
   state["Passed"]+=int(good);dump(root/"run-status.json",state)
   if not good:raise RuntimeError(name+" failed "+str(result))
   for row in result["Rows"]:samples.append({"repeat":rep,"member":member,**row})
   print("PASS",name,flush=True)
 state.update(Complete=True,SourceUnchanged=all(SHA(Path(f))==sha for f,sha in frozen.items()),ExeUnchanged=SHA(exe)==exe_sha)
 state["Pass"]=state["Passed"]==10 and state["SourceUnchanged"] and state["ExeUnchanged"];dump(root/"run-status.json",state)
 if not state["Pass"]:raise RuntimeError("control matrix incomplete")
 with (root/"samples.csv").open("w",newline="") as f:w=csv.DictWriter(f,fieldnames=list(samples[0]));w.writeheader();w.writerows(samples)
 def stats(a):
  a=sorted(a);return {"P50":statistics.median(a),"P95":a[math.ceil(.95*len(a))-1],"P99":a[math.ceil(.99*len(a))-1]}
 pairs=[]
 for rep in range(1,6):
  metrics={}
  for metric in ("batch_ns","upload_ns","manager_ns","compute_cb_gpu_ns","record_ns","submit_ns","completion_ns","wait_api_ns"):
   x=stats([r[metric] for r in samples if r["repeat"]==rep and r["member"]=="first"]);y=stats([r[metric] for r in samples if r["repeat"]==rep and r["member"]=="second"])
   metrics[metric]={"first":x,"second":y,"change_percent":{k:100*(y[k]/x[k]-1) if x[k] else None for k in x}}
  pairs.append({"Repeat":rep,"Metrics":metrics})
 dump(root/"control-analysis.json",{"Pairs":pairs,"Scope":"same binary, second minus first; endpoint sensors do not prove thermal stability or root cause"})
if __name__=="__main__":main()
