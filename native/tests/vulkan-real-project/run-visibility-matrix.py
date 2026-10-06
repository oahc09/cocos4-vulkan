"""Fresh build + bounded actual-project buffer-visibility correctness matrix."""
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
CMAKE = "D:/Creator/3.8.8/resources/tools/cmake/bin/cmake.exe"


def dump(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    if root.exists():
        raise ValueError("new evidence root required")
    root.mkdir(parents=True)
    tree = PROJECT / "build/windows/proj"
    with (root / "build.log").open("w", encoding="utf-8") as log:
        code = subprocess.run([CMAKE, "--build", str(tree), "--config", "Debug", "--target", "test-cases", "--parallel", "4"], stdout=log, stderr=log).returncode
    if code:
        raise RuntimeError("current build failed; refusing old executable")
    exe = tree / "Debug/test-cases.exe"
    sources = [REPO / "native/cocos/renderer/gfx-vulkan/VKCommands.cpp", HERE / "RealProjectVisibility.cpp", HERE / "RealProjectVisibility.h", HERE / "verify-visibility.py", HERE / "collect-performance-runtime.py"]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {str(p): sha(p) for p in sources}
    exe_sha = sha(exe)
    dump(root / "source-before.json", before)
    state = {"Executor": "Lead", "Complete": False, "Pass": False, "Attempted": 0, "Passed": 0, "ExeSha256": exe_sha}
    spec = importlib.util.spec_from_file_location("visibility_judge", HERE / "verify-visibility.py")
    judge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(judge)
    manifest = []
    cases = [("legacy", "content", "1.3", 0, 0, 1), ("sync2", "content", "1.3", 1, 1, 1),
             ("fence11", "content", "1.1", 0, 0, 0), ("fallback", "fallback", "1.3", 1, 1, 1),
             ("wrong-input", "wrong-input", "1.3", 1, 1, 1), ("bad-mode", "bogus", "1.3", 0, 0, 1)]
    for name, mode, api, sync2, submit2, timeline in cases:
        d = root / name
        d.mkdir()
        expected = int(mode in ("wrong-input", "bogus"))
        env = {"CC_VULKAN_FORCE_API": api, "CC_VULKAN_COMPLETION": "auto" if timeline else "fence",
               "CC_VULKAN_BARRIER": "sync2" if sync2 else "legacy", "CC_VULKAN_SUBMIT": "submit2" if submit2 else "legacy",
               "CC_VULKAN_RENDERING": "classic", "CC_VULKAN_REAL_PROJECT_VISIBILITY": mode}
        dump(d / "config.json", {"Actor": "Lead", "exe": str(exe), "env": env, "closeAfterSeconds": 5, "expectedExit": expected})
        state["Attempted"] += 1
        dump(root / "run-status.json", state)
        with (d / "collector.log").open("w", encoding="utf-8") as log:
            code = subprocess.run([sys.executable, str(HERE / "collect-performance-runtime.py"), str(d)], stdout=log, stderr=log, timeout=120).returncode
        native = json.loads((d / "runtime-summary.json").read_text())
        text = (d / "debug-output.log").read_text(encoding="utf-8")
        if mode == "bogus":
            result = {"Pass": native["exitCode"] == 1 and "FAIL: visibility known mode" in text and "project-visibility phase:" not in text and not any(not v["firstChance"] for v in native["exceptions"])}
        else:
            result = judge.judge(native, text, mode, api, sync2, submit2, timeline)
        good = code == 0 and result["Pass"] and native["exeSHA256"] == exe_sha
        manifest.append({"Case": name, "NativeExit": native["exitCode"], "WrapperExit": code, "Pass": good})
        state["Passed"] += int(good)
        dump(d / "judgment.json", result)
        dump(root / "manifest.json", manifest)
        print(name, good, flush=True)
        if not good:
            raise RuntimeError(name + " failed " + str(result))
    after = {str(p): sha(p) for p in sources}
    state.update(Complete=True, SourceUnchanged=before == after, Pass=before == after and state["Passed"] == 6)
    dump(root / "source-after.json", after)
    dump(root / "run-status.json", state)
    if not state["Pass"]:
        raise RuntimeError("source changed or incomplete matrix")
    subprocess.run([sys.executable, str(HERE / "verify-visibility.py"), str(root), "--self-test"], check=True)


if __name__ == "__main__":
    main()
