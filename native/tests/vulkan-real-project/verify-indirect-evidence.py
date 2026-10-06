"""Offline correctness judge for the actual Debug test-cases.exe. No GPU launch."""
import argparse
import copy
import json
import re
from pathlib import Path

PREFIX = r"(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?"
UINT = r"(?:0|[1-9][0-9]*)"


def judge(log, runtime, mode, api, paths):
    reasons = []

    def check(value, label):
        if not value:
            reasons.append(label)

    if mode not in ("multi", "loop", "skip-second") or api not in ("1.1", "1.3"):
        return {"pass": False, "reasons": ["unknown expected mode/API"]}
    if not isinstance(log, str) or not isinstance(runtime, dict):
        return {"pass": False, "reasons": ["log and native runtime required"]}
    fault = mode == "skip-second"
    check(type(runtime.get("exitCode")) is int and runtime["exitCode"] == int(fault), "native exit")
    check(runtime.get("executor") == "Lead", "execution channel")
    check(str(runtime.get("exe", "")).replace("\\", "/").lower().endswith(
        "/cocos4-vulkan-test-project/build/windows/proj/debug/test-cases.exe"), "real project exe")
    check(re.fullmatch(r"[0-9a-fA-F]{64}", str(runtime.get("exeSHA256", ""))), "exe SHA")
    exc = runtime.get("exceptions")
    check(isinstance(exc, list), "exception evidence")
    if isinstance(exc, list):
        for e in exc:
            check(isinstance(e, dict) and e.get("firstChance") is True and
                  e.get("exception") not in ("0xc0000005", "0xc0000409", "0xc000001d"), "native crash")
    check(not re.search(r"VUID-|HAZARD|Uncaught|SKIPPED|INCOMPLETE|UNSUPPORTED|FAIL:", log, re.I), "Vulkan/JS/failure")
    lines = log.splitlines()

    def exact(reserved, pattern, count=1):
        selected = [v for v in lines if re.search(reserved, v, re.I)]
        matches = [re.fullmatch(PREFIX + pattern, v) for v in selected]
        check(len(selected) == count and all(matches), "unique exact " + reserved)
        return [m for m in matches if m]

    check("INSTANCE_LAYERS: VK_LAYER_KHRONOS_validation" in log, "actual validation layer")
    ver = r"[0-9]+\.[0-9]+"
    exact(r"Vulkan\s*API", rf"Vulkan API: loader={ver} requested={ver} XR={ver} instance={ver} physical={ver} effective={re.escape(api)}")
    p = exact(r"Vulkan\s*paths", r"Vulkan paths: timeline=([01]) sync2=([01]) submit2=([01]) dynamicRendering=([01]) classicOverride=([01]) maintenance4=([01])")
    check(isinstance(paths, list) and len(paths) == 5 and all(type(v) is int and v in (0, 1) for v in paths)
          and bool(p) and list(map(int, p[0].groups()))[:5] == paths, "requested runtime paths")
    exact(r"Total\s*device\s*memory\s*leaked", r"Total device memory leaked: 0 bytes\.")
    exact(r"project-indirect\s*mode", rf"project-indirect mode: {mode} enabled=1")
    exact(r"project-indirect\s*validation", r"project-indirect validation: ENABLED")
    path = exact(r"project-indirect\s*path", rf"project-indirect path: multi=([01]) fault={int(fault)} firstInstance=0")
    multi = int(path[0][1]) if path else 0
    check(mode == "skip-second" or multi == int(mode == "multi"), "actual multi policy")
    phases = exact(r"project-indirect\s*phase", rf"project-indirect phase: indexed=([01]) calls={1 if multi else 2} commands=2 stride=({UINT}) packedBytes=({UINT}) packed=1 submits=1 chain=1 checkedBytes=2048 mismatches=({UINT})", 2)
    check(len(phases) == 2 and [int(m[1]) for m in phases] == [0, 1], "both independent phases")
    for m in phases:
        i, stride, packed, mismatch = map(int, m.groups())
        check(stride == (20 if i else 16) and packed == stride * 2, "actual packed shape")
        # A deliberately absent green draw leaves blue: G and B bytes differ.
        check(mismatch == (512 if fault else 0), "full golden result")
    exact(r"project-indirect\s*allocations", r"project-indirect allocations: bufferFacades=7 bufferDrops=7 imageFacades=2 imageDrops=2 allocatorDestroyObserved=0")
    retirement = exact(r"project-indirect\s*retirement", rf"project-indirect retirement: created=({UINT}) destroyed=({UINT}) devices=1 restored=1")
    check(bool(retirement) and int(retirement[0][1]) in (7, 8) and retirement[0][1] == retirement[0][2], "observed object retirement")
    exact("completed", rf"project-indirect completed: failures={2 if fault else 0} liveErrors=0 postErrors=0 outcome={'FAILED' if fault else 'CLEAN'}")
    family = [v for v in lines if re.search(r"(?<![a-z0-9-])project-indirect", v, re.I)]
    failures = [v for v in family if re.search(r"project-indirect\s*failure", v, re.I)]
    check(len(family) == (10 if fault else 8), "reserved family count")
    check(len(failures) == (2 if fault else 0) and all(re.fullmatch(
        r"\d{2}:\d{2}:\d{2} \[ERROR\]: project-indirect failure: red/green golden contents", v) for v in failures), "exact fault reason")
    # CC_LOG_ERROR emits a separate source-location line before each message.
    errors = [v for v in lines if "[ERROR]" in v]
    if fault:
        check(len(errors) == 4 and all(v in failures or re.fullmatch(
            r"\d{2}:\d{2}:\d{2} \[ERROR\]: \[ERROR\] file .*[\\/]RealProjectIndirect\.cpp: line [0-9]+\s*", v) for v in errors), "fault-only errors")
    else:
        check(not errors, "no error logs")
    return {"pass": not reasons, "reasons": reasons}


def self_test(case):
    base = case.read_text(encoding="utf-8")
    runtime = json.loads(case.with_name("runtime-summary.json").read_text())
    config = json.loads(case.with_name("config.json").read_text())
    mode, api, paths = config["env"]["CC_VULKAN_REAL_PROJECT_INDIRECT"], "1.3", [1, 1, 1, 1, 0]
    assert judge(base, runtime, mode, api, paths)["pass"], "real positive base"
    fixtures = [("positive", base, runtime, True)]
    mutations = [
        ("missing-indexed", lambda s: "\n".join(v for v in s.splitlines() if not ("project-indirect phase:" in v and "indexed=1" in v))),
        ("packed-zero", lambda s: s.replace("packed=1", "packed=0", 1)),
        ("mismatch", lambda s: s.replace("mismatches=0", "mismatches=1", 1)),
        ("submit-zero", lambda s: s.replace("submits=1", "submits=0", 1)),
        ("chain-zero", lambda s: s.replace("chain=1", "chain=0", 1)),
        ("leading-zero", lambda s: s.replace("checkedBytes=2048", "checkedBytes=02048", 1)),
        ("wrong-stride", lambda s: s.replace("stride=20", "stride=16", 1)),
        ("missing-retirement", lambda s: "\n".join(v for v in s.splitlines() if "project-indirect retirement:" not in v)),
        ("wrong-drops", lambda s: s.replace("bufferDrops=7", "bufferDrops=6", 1)),
        ("bad-live", lambda s: s.replace("liveErrors=0", "liveErrors=1", 1)),
        ("bad-post", lambda s: s.replace("postErrors=0", "postErrors=1", 1)),
        ("external-footer", lambda s: s + "\nfoo completed failures=1"),
        ("malformed-family", lambda s: s + "\nPROJECT-INDIRECTphase garbage"),
        ("duplicate-footer", lambda s: s + "\n" + next(v for v in s.splitlines() if "completed:" in v)),
        ("junk-leak", lambda s: s.replace("Total device memory leaked:", "garbage Total device memory leaked:", 1)),
        ("hazard", lambda s: s + "\nSYNC-HAZARD-READ-AFTER-WRITE"),
        ("wrong-api", lambda s: s.replace("effective=1.3", "effective=1.30", 1)),
        ("path-tail", lambda s: s.replace("maintenance4=0", "maintenance4=0 junk", 1)),
        ("failure", lambda s: s + "\n[ERROR]: unexpected"),
    ]
    fixtures += [(name, fn(base), runtime, False) for name, fn in mutations]
    for name, key, val in [("ntstatus", "exitCode", 3221225477), ("missing-exc", "exceptions", None),
                           ("wrong-exe", "exe", "smoke.exe"), ("wrong-exit", "exitCode", 1)]:
        modified = copy.deepcopy(runtime); modified[key] = val
        fixtures.append((name, base, modified, False))
    fault_case = case.parent.parent / "skip-second"
    fault_log = (fault_case / "debug-output.log").read_text(encoding="utf-8")
    fault_runtime = json.loads((fault_case / "runtime-summary.json").read_text())
    # The same judge accepts the intended content failure, never as a positive draw.
    assert judge(fault_log, fault_runtime, "skip-second", api, paths)["pass"]
    fault_mutations = [
        ("fault-no-golden", fault_log.replace("mismatches=512", "mismatches=0", 1)),
        ("fault-hidden-footer", fault_log.replace("failures=2", "failures=0", 1)),
        ("fault-unexpected-reason", fault_log + "\n[ERROR]: another failure"),
    ]
    for name, mutated in fault_mutations:
        assert not judge(mutated, fault_runtime, "skip-second", api, paths)["pass"], name
    fixtures += [
        ("missing-allocations", "\n".join(v for v in base.splitlines() if "project-indirect allocations:" not in v), runtime, False),
        ("duplicate-phase", base + "\n" + next(v for v in base.splitlines() if "project-indirect phase:" in v), runtime, False),
        ("junk-api-prefix", base.replace("Vulkan API:", "garbage Vulkan API:", 1), runtime, False),
    ]
    for name, log, native, expected in fixtures:
        result = judge(log, native, mode, api, paths)
        assert result["pass"] == expected, (name, result)
    print(json.dumps({"declared": len(fixtures) + 4, "executed": len(fixtures) + 4, "passed": len(fixtures) + 4}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("case", type=Path, help="case directory with log/runtime-summary/config")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.case / "debug-output.log")
    else:
        c = json.loads((args.case / "config.json").read_text())
        env = c["env"]; modern = env["CC_VULKAN_FORCE_API"] == "1.3"
        result = judge((args.case / "debug-output.log").read_text(encoding="utf-8"),
                       json.loads((args.case / "runtime-summary.json").read_text()),
                       env["CC_VULKAN_REAL_PROJECT_INDIRECT"], env["CC_VULKAN_FORCE_API"],
                       [1, 1, 1, 1, 0] if modern else [0, 0, 0, 0, 1])
        print(json.dumps(result, indent=2)); raise SystemExit(0 if result["pass"] else 1)
