"""Build isolated manager before/after binaries, sample the same actual project path."""
import argparse
import csv
import datetime
import hashlib
import importlib.util
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PROJECT = Path("E:/AIProMax/github/cocos4-vulkan-test-project")
CMAKE = "D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe"
SHA = lambda b: hashlib.sha256(b).hexdigest()
def dump(p,v): p.write_text(json.dumps(v,indent=2),encoding="utf-8")
def function_range(data):
    begin = data.index(b"void CCVKGPUBarrierManager::update(")
    end = data.index(b"void CCVKGPUBufferHub::flush(", begin)
    return begin,end

def main():
    p=argparse.ArgumentParser(); p.add_argument("--root",type=Path,required=True); p.add_argument("--baseline-source",type=Path,required=True); a=p.parse_args()
    root=a.root.resolve()
    if root.exists(): raise ValueError("new evidence root required")
    root.mkdir(parents=True)
    core=REPO/"native/cocos/renderer/gfx-vulkan/VKCommands.cpp"; original=core.read_bytes()
    old=a.baseline_source.read_bytes(); i,k=function_range(original); oi,ok=function_range(old)
    baseline=original[:i]+old[oi:ok]+original[k:]
    if baseline==original: raise ValueError("baseline identical")
    (root/"VKCommands-current.cpp").write_bytes(original); (root/"VKCommands-baseline.cpp").write_bytes(baseline)
    tree=PROJECT/"build/windows/proj-perf-off"; exe=tree/"Debug/test-cases.exe"
    state={"Actor":"Lead","Complete":False,"Pass":False,"Attempted":0,"Passed":0,"SourceRestored":False}
    dump(root/"run-status.json",state)
    build_failed=False
    try:
        for name,data in (("baseline",baseline),("current",original)):
            core.write_bytes(data)
            with (root/(name+"-build.log")).open("w",encoding="utf-8") as log:
                code=subprocess.run([CMAKE,"--build",str(tree),"--config","Debug","--target","test-cases","--parallel","4"],stdout=log,stderr=log).returncode
            state[name+"BuildExit"]=code; dump(root/"run-status.json",state)
            if code: build_failed=True; raise RuntimeError(name+" build failed; old executable refused")
            (root/(name+".exe")).write_bytes(exe.read_bytes())
    finally:
        core.write_bytes(original); state["SourceRestored"]=core.read_bytes()==original; dump(root/"run-status.json",state)
    if build_failed: raise RuntimeError("build failed")
    current=exe.read_bytes(); current_stat=exe.stat()
    sources=[core,HERE/"RealProjectVisibility.cpp",HERE/"verify-upload-performance.py",HERE/"collect-performance-runtime.py",HERE/"run-upload-performance.py"]
    before={str(f):SHA(f.read_bytes()) for f in sources}; dump(root/"source-before.json",before)
    spec=importlib.util.spec_from_file_location("judge",HERE/"verify-upload-performance.py"); judge=importlib.util.module_from_spec(spec); spec.loader.exec_module(judge)
    manifest=[]; samples=[]; failed=[]
    try:
        for rep in range(1,6):
            for name in (("baseline","current") if rep%2 else ("current","baseline")):
                image=(root/(name+".exe")).read_bytes(); exe.write_bytes(image)
                case=f"{name}-rep{rep}"; d=root/case; d.mkdir(); mode="upload-perf-baseline" if name=="baseline" else "upload-perf"
                env={"CC_VULKAN_FORCE_API":"1.3","CC_VULKAN_COMPLETION":"auto","CC_VULKAN_BARRIER":"legacy","CC_VULKAN_SUBMIT":"legacy","CC_VULKAN_RENDERING":"classic","CC_VULKAN_REAL_PROJECT_VISIBILITY":mode}
                dump(d/"config.json",{"Actor":"Lead","exe":str(exe),"env":env,"closeAfterSeconds":5,"expectedExit":0})
                item={"Case":case,"Version":name,"Repeat":rep,"ExeSha256":SHA(image),"StartedUTC":datetime.datetime.now(datetime.timezone.utc).isoformat()}; manifest.append(item)
                state["Attempted"]+=1; dump(root/"manifest.json",manifest); dump(root/"run-status.json",state); print("RUN",case,flush=True)
                with (d/"collector.log").open("w",encoding="utf-8") as log:
                    code=subprocess.run([sys.executable,str(HERE/"collect-performance-runtime.py"),str(d)],stdout=log,stderr=log,timeout=300).returncode
                summary=json.loads((d/"runtime-summary.json").read_text()); text=(d/"debug-output.log").read_text(encoding="utf-8"); csv_text=(d/"upload-performance.csv").read_text() if (d/"upload-performance.csv").exists() else ""
                result=judge.judge(summary,text,csv_text,mode,"off")
                good=code==0 and result["Pass"] and summary["exeSHA256"]==SHA(image)
                item.update(NativeExit=summary["exitCode"],Pass=good,FinishedUTC=datetime.datetime.now(datetime.timezone.utc).isoformat())
                dump(d/"judgment.json",{key:v for key,v in result.items() if key!="Rows"}); dump(root/"manifest.json",manifest)
                state["Passed"]+=int(good); dump(root/"run-status.json",state)
                if not good: failed.append(case); print(result,flush=True); raise RuntimeError(case+" failed")
                for row in result["Rows"]: samples.append({"version":name,"repeat":rep,**row})
                if len(manifest)==1:
                    mutations=[("resources=0","resources=1"),("submits=1","submits=0"),("managerSubmits=1","managerSubmits=0"),("mismatches=0","mismatches=1"),("destroyed=4","destroyed=3"),("count=1000","count=999"),("effective=1.3","effective=1.30")]
                    fixtures=[result["Pass"]]
                    for x,y in mutations:
                        assert x in text; fixtures.append(not judge.judge(summary,text.replace(x,y,1),csv_text,mode,"off")["Pass"])
                    for extra in ("foo completed failures=1","project-visibilitybatch: junk","FAIL: unexpected","SYNC-HAZARD-WAW"):
                        fixtures.append(not judge.judge(summary,text+"\n"+extra,csv_text,mode,"off")["Pass"])
                    fixtures.append(not judge.judge(summary,text,"",mode,"off")["Pass"])
                    fixtures.append(not judge.judge(summary,text,csv_text.replace("131072", "131071",1),mode,"off")["Pass"])
                    dump(root/"fixtures.json",{"Declared":len(fixtures),"Executed":len(fixtures),"Passed":sum(fixtures)})
                    if not all(fixtures): raise RuntimeError("parser fixtures failed")
                print("PASS",case,flush=True)
    finally:
        import os
        exe.write_bytes(current); os.utime(exe,ns=(current_stat.st_atime_ns,current_stat.st_mtime_ns))
        state["CurrentExeRestored"]=SHA(exe.read_bytes())==SHA(current)
        state["SourceUnchanged"]={str(f):SHA(f.read_bytes()) for f in sources}==before
        state.update(Complete=True,Pass=not failed and state["Passed"]==10 and state["CurrentExeRestored"] and state["SourceUnchanged"])
        dump(root/"run-status.json",state)
    if not state["Pass"]: raise RuntimeError("formal upload matrix incomplete")
    with (root/"samples.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(samples[0]));writer.writeheader();writer.writerows(samples)
    def stats(values):
        values=sorted(values); return {"P50":statistics.median(values),"P95":values[math.ceil(.95*len(values))-1],"P99":values[math.ceil(.99*len(values))-1]}
    pairs=[]
    for rep in range(1,6):
        metrics={}
        for metric in ("batch_ns","upload_ns","manager_ns","compute_cb_gpu_ns","record_ns","submit_ns","completion_ns","wait_api_ns"):
            b=stats([v[metric] for v in samples if v["repeat"]==rep and v["version"]=="baseline"])
            c=stats([v[metric] for v in samples if v["repeat"]==rep and v["version"]=="current"])
            metrics[metric]={"baseline":b,"current":c,"change_percent":{q:100*(c[q]/b[q]-1) if b[q] else None for q in b}}
        pairs.append({"Repeat":rep,"Metrics":metrics})
    dump(root/"paired-analysis.json",{"Pairs":pairs,"MedianPairedChangePercent":{m:{q:statistics.median([v["Metrics"][m]["change_percent"][q] for v in pairs if v["Metrics"][m]["change_percent"][q] is not None]) if any(v["Metrics"][m]["change_percent"][q] is not None for v in pairs) else None for q in ("P50","P95","P99")} for m in ("batch_ns","upload_ns","manager_ns","compute_cb_gpu_ns","record_ns","submit_ns","completion_ns","wait_api_ns")},"Scope":"synchronized upload+compute+copy batches, not display/game frames; GPU timestamp covers user compute/readback CB only"})
if __name__=="__main__": main()
