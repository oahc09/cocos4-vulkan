"""Actual Windows project: fresh build and full-content upload stress evidence."""
import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PROJECT = Path("E:/AIProMax/github/cocos4-vulkan-test-project")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True, type=Path)
    a = p.parse_args()
    root = a.root.resolve()
    if root.exists(): raise ValueError("fresh evidence root required")
    root.mkdir(parents=True)
    tree = PROJECT / "build/windows/proj"
    with (root / "build.log").open("w", encoding="utf-8") as log:
        code = subprocess.run(["D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe", "--build", str(tree), "--config", "Debug", "--target", "test-cases", "--parallel", "4"], stdout=log, stderr=log).returncode
    if code: raise RuntimeError("build failed; old executable refused")
    exe = tree / "Debug/test-cases.exe"
    sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    sources = [HERE / "RealProjectVisibility.cpp", HERE / "verify-visibility.py", HERE / "collect-performance-runtime.py", REPO / "native/cocos/renderer/gfx-vulkan/VKCommands.cpp"]
    before = {str(f): sha(f) for f in sources}
    exe_sha = sha(exe)
    spec = importlib.util.spec_from_file_location("judge", HERE / "verify-visibility.py")
    judge = importlib.util.module_from_spec(spec); spec.loader.exec_module(judge)
    dump = lambda f, v: f.write_text(json.dumps(v, indent=2), encoding="utf-8")
    dump(root / "source-before.json", before)
    state = {"Actor":"Lead", "Complete":False, "Pass":False, "Attempted":0, "Passed":0, "ExeSha256":exe_sha}
    manifest = []
    cases = [("legacy", "stress", "1.3", 0, 0, 1), ("modern", "stress", "1.3", 1, 1, 1), ("fence11", "stress", "1.1", 0, 0, 0), ("wrong-content", "stress-wrong", "1.3", 1, 1, 1)]
    for name, mode, api, sync2, submit2, timeline in cases:
        d = root / name; d.mkdir()
        env = {"CC_VULKAN_FORCE_API":api, "CC_VULKAN_COMPLETION":"auto" if timeline else "fence", "CC_VULKAN_BARRIER":"sync2" if sync2 else "legacy", "CC_VULKAN_SUBMIT":"submit2" if submit2 else "legacy", "CC_VULKAN_RENDERING":"classic", "CC_VULKAN_REAL_PROJECT_VISIBILITY":mode}
        dump(d / "config.json", {"Actor":"Lead", "exe":str(exe), "env":env, "closeAfterSeconds":10, "expectedExit":int(mode == "stress-wrong")})
        state["Attempted"] += 1; dump(root / "run-status.json", state)
        with (d / "collector.log").open("w", encoding="utf-8") as log:
            code = subprocess.run([sys.executable, str(HERE / "collect-performance-runtime.py"), str(d)], stdout=log, stderr=log, timeout=150).returncode
        summary = json.loads((d / "runtime-summary.json").read_text())
        text = (d / "debug-output.log").read_text(encoding="utf-8")
        result = judge.judge(summary, text, mode, api, sync2, submit2, timeline)
        good = code == 0 and result["Pass"] and summary["exeSHA256"] == exe_sha
        dump(d / "judgment.json", result)
        manifest.append({"Case":name, "Exit":summary["exitCode"], "Pass":good}); dump(root / "manifest.json", manifest)
        state["Passed"] += int(good)
        print(name, result, flush=True)
        if not good: raise RuntimeError("stress failed " + name)
        if name == "legacy":
            mutations = [("uploads=4", "uploads=3"), ("copies=1", "copies=0"), ("submits=1", "submits=0"), ("managerSubmits=1", "managerSubmits=0"), ("mismatches=0", "mismatches=1"), ("checkedBytes=65536", "checkedBytes=0"), ("destroyed=96", "destroyed=95"), ("liveErrors=0", "liveErrors=1"), ("effective=1.3", "effective=1.30")]
            fixtures = [result["Pass"]]
            for old, new in mutations:
                assert old in text
                fixtures.append(not judge.judge(summary, text.replace(old,new,1), mode,api,sync2,submit2,timeline)["Pass"])
            for extra in ("project-visibilitytexture: garbage", "foo completed failures=1", "SYNC-HAZARD-WAW", "FAIL: unexpected"):
                fixtures.append(not judge.judge(summary,text+"\n"+extra,mode,api,sync2,submit2,timeline)["Pass"])
            dump(root / "fixtures.json", {"Declared":len(fixtures),"Executed":len(fixtures),"Passed":sum(fixtures)})
            if not all(fixtures): raise RuntimeError("stress parser fixtures failed")
    after = {str(f):sha(f) for f in sources}
    state.update(Complete=True, SourceUnchanged=before==after, Pass=before==after and state["Passed"]==4)
    dump(root / "run-status.json", state)
    if not state["Pass"]: raise RuntimeError("source changed")
if __name__ == "__main__": main()
