"""Offline functional judge for the actual test-cases.exe (not a smoke binary).

Inputs are a Windows debug-output log and its native runtime summary. Restored
requires the successfully judged cold payload byte count. No performance gate.
"""
import argparse
import copy
import json
import re
from pathlib import Path

MAX_PAYLOAD = 64 * 1024 * 1024
LOG_PREFIX = r"(?:\d{2}:\d{2}:\d{2} \[INFO\]: )?"
UINT = r"(?:0|[1-9][0-9]*)"


def judge(log, runtime, phase, expected_initial=None, expected_api=None, expected_paths=None):
    reasons = []

    def check(ok, message):
        if not ok:
            reasons.append(message)

    if phase not in ("off", "cold", "restored", "rejected", "wrong-key"):
        return {"pass": False, "reasons": ["unknown phase"], "savedBytes": 0}
    check(isinstance(log, str) and bool(log.strip()), "log required")
    if not isinstance(log, str):
        log = ""
    if not isinstance(runtime, dict):
        runtime = {}
    expected_exit = 1 if phase == "wrong-key" else 0
    check(type(runtime.get("exitCode")) is int and runtime["exitCode"] == expected_exit, "exact native exit")
    check(runtime.get("executor") == "Lead", "execution actor")
    exe = str(runtime.get("exe", "")).replace("\\", "/").lower()
    check(exe.endswith("/cocos4-vulkan-test-project/build/windows/proj/debug/test-cases.exe"), "actual test project")
    check(re.fullmatch(r"[0-9a-fA-F]{64}", str(runtime.get("exeSHA256", ""))) is not None, "exe SHA required")
    exceptions = runtime.get("exceptions")
    check(isinstance(exceptions, list), "native exception evidence required")
    if isinstance(exceptions, list):
        for exc in exceptions:
            try:
                check(exc.get("firstChance") is True and int(exc["exception"], 16) not in
                      (0xC0000005, 0xC0000409, 0xC000001D), "native crash")
            except (ValueError, TypeError, KeyError, AttributeError):
                check(False, "malformed native exception")
    check(not re.search(r"\[ERROR\]|VUID-|(?:SYNC-)?HAZARD|Uncaught|SKIPPED|INCOMPLETE|FAIL:", log, re.I), "error/hazard/JS/failure")
    # A previously valid file is not proof that the current save succeeded.
    check(not re.search(r"Vulkan pipeline cache (?:retrieval|save|path) failed", log, re.I), "cache save/retrieval failure")
    check("INSTANCE_LAYERS: VK_LAYER_KHRONOS_validation" in log, "actual validation layer")
    lines = log.splitlines()
    api_lines = [line for line in lines if re.search(r"Vulkan\s*API", line, re.I)]
    version = r"[0-9]+\.[0-9]+"
    api = re.fullmatch(LOG_PREFIX + rf"Vulkan API: loader={version} requested={version} XR={version} instance={version} physical={version} effective=(1\.[123])",
                       api_lines[0]) if len(api_lines) == 1 else None
    check(api is not None and (expected_api is None or api[1] == expected_api), "actual effective API")
    path_lines = [line for line in lines if re.search(r"Vulkan\s*paths", line, re.I)]
    paths = re.fullmatch(LOG_PREFIX + r"Vulkan paths: timeline=([01]) sync2=([01]) submit2=([01]) dynamicRendering=([01]) classicOverride=([01]) maintenance4=([01])",
                         path_lines[0]) if len(path_lines) == 1 else None
    check(paths is not None, "unique exact runtime paths")
    if expected_paths is not None:
        valid = isinstance(expected_paths, list) and len(expected_paths) in (5, 6) and all(type(v) is int and v in (0, 1) for v in expected_paths)
        check(valid and paths is not None and list(map(int, paths.groups()))[:len(expected_paths)] == expected_paths, "runtime paths match requested case")
    leak_lines = [line for line in lines if re.search(r"Total\s*device\s*memory\s*leaked", line, re.I)]
    check(len(leak_lines) == 1 and re.fullmatch(LOG_PREFIX + r"Total device memory leaked: 0 bytes\.", leak_lines[0]) is not None
          if leak_lines else False, "unique exact leak zero")
    markers = [line for line in lines if re.search(r"project-prewarm", line, re.I)]
    check(len(markers) == (1 if phase == "off" else 11), "reserved marker count")

    def one(family, pattern):
        reserved = [line for line in markers if re.search(r"project-prewarm\s*" + family, line, re.I)]
        check(len(reserved) == 1, "unique " + family)
        match = re.fullmatch(LOG_PREFIX + pattern, reserved[0]) if len(reserved) == 1 else None
        check(match is not None, "exact " + family)
        return match

    one("mode", rf"project-prewarm mode: phase={phase} enabled={int(phase != 'off')}")
    if phase == "off":
        return {"pass": not reasons, "reasons": reasons, "savedBytes": 0}
    one("validation", r"project-prewarm validation: ENABLED")
    identity = one("identity", rf"project-prewarm identity: driver=({UINT}) vendor=({UINT}) device=({UINT})")
    if identity:
        check(all(0 < int(v) <= 0xFFFFFFFF for v in identity.groups()), "actual identity bounds")
    cache = one("cache-input", rf"project-prewarm cache-input: calls=1 loaded=([01]) initialBytes=({UINT}) exact=1")
    if cache:
        loaded, initial = map(int, cache.groups())
        check(loaded == int(phase == "restored"), "loaded matches phase")
        if phase == "restored":
            try:
                valid = (type(expected_initial) is int and 0 < expected_initial <= MAX_PAYLOAD)
                check(valid and initial == expected_initial, "cold/restored payload link")
            except (ValueError, OverflowError):
                check(False, "cold/restored payload link")
        else:
            check(initial == 0, "cold/rejected/wrong input empty")
    one("manifest", r"project-prewarm manifest: keys=2 distinct=1 duplicate=1 beforeCreates=0")
    one("queue", rf"project-prewarm queue: attempted=2 ready={1 if phase == 'wrong-key' else 2} failed={1 if phase == 'wrong-key' else 0} creates=2")
    one("ownership", r"project-prewarm ownership: queued=1 released=1")
    scene = one("scene", rf"project-prewarm scene: graphics=({UINT}) cached=({UINT}) destroyed=({UINT})")
    if scene:
        graphics, cached, destroyed = map(int, scene.groups())
        check(0 < graphics <= 4096 and graphics == cached == destroyed, "scene cache and retirement")
    saved = one("saved", rf"project-prewarm saved: bytes=({UINT}) validated=1")
    saved_bytes = int(saved[1]) if saved else 0
    check(0 < saved_bytes <= MAX_PAYLOAD, "saved payload bounds")
    one("retirement", r"project-prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1 devices=1")
    footer = [line for line in lines if re.search("completed", line, re.I)]
    outcome = "REJECTED" if phase == "wrong-key" else "CLEAN"
    check(len(footer) == 1 and re.fullmatch(LOG_PREFIX + rf"project-prewarm {phase} completed: failures=0 liveErrors=0 deviceErrors=0 outcome={outcome}", footer[0]) is not None
          if footer else False, "unique exact footer")
    return {"pass": not reasons, "reasons": reasons, "savedBytes": saved_bytes if not reasons else 0}


def self_test():
    runtime = {"exitCode": 0, "executor": "Lead", "exceptions": [], "exeSHA256": "a" * 64,
               "exe": "E:/AIProMax/github/cocos4-vulkan-test-project/build/windows/proj/Debug/test-cases.exe"}
    base = "\n".join([
        "INSTANCE_LAYERS: VK_LAYER_KHRONOS_validation", "Total device memory leaked: 0 bytes.",
        "Vulkan API: loader=1.4 requested=1.3 XR=1.3 instance=1.3 physical=1.4 effective=1.3",
        "Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=0 maintenance4=0",
        "project-prewarm mode: phase=cold enabled=1", "project-prewarm validation: ENABLED",
        "project-prewarm identity: driver=1 vendor=4318 device=10208",
        "project-prewarm cache-input: calls=1 loaded=0 initialBytes=0 exact=1",
        "project-prewarm manifest: keys=2 distinct=1 duplicate=1 beforeCreates=0",
        "project-prewarm queue: attempted=2 ready=2 failed=0 creates=2",
        "project-prewarm ownership: queued=1 released=1",
        "project-prewarm scene: graphics=47 cached=47 destroyed=47",
        "project-prewarm saved: bytes=14048 validated=1",
        "project-prewarm retirement: pipelines=2/2 modules=2/2 layouts=1/1 devices=1",
        "project-prewarm cold completed: failures=0 liveErrors=0 deviceErrors=0 outcome=CLEAN"])
    fixtures = [("cold-valid", base, runtime, "cold", None, True)]
    restored = base.replace("phase=cold", "phase=restored").replace("cold completed", "restored completed").replace("loaded=0 initialBytes=0", "loaded=1 initialBytes=14048")
    fixtures += [("restored-valid", restored, runtime, "restored", 14048, True),
                 ("restored-missing-link", restored, runtime, "restored", None, False),
                 ("restored-wrong-link", restored, runtime, "restored", 999, False)]
    rejected = base.replace("phase=cold", "phase=rejected").replace("cold completed", "rejected completed")
    fixtures.append(("rejected-valid", rejected, runtime, "rejected", None, True))
    wrong = base.replace("phase=cold", "phase=wrong-key").replace("cold completed", "wrong-key completed").replace("ready=2 failed=0", "ready=1 failed=1").replace("outcome=CLEAN", "outcome=REJECTED")
    wrong_runtime = dict(runtime, exitCode=1)
    fixtures.append(("wrong-key-valid", wrong, wrong_runtime, "wrong-key", None, True))
    off = "\n".join(line for line in base.splitlines() if "project-prewarm" not in line) + "\nproject-prewarm mode: phase=off enabled=0"
    fixtures.append(("off-valid", off, runtime, "off", None, True))
    replacements = {
        "missing-validation": ("project-prewarm validation: ENABLED", ""),
        "no-scene": ("graphics=47 cached=47 destroyed=47", "graphics=0 cached=0 destroyed=0"),
        "not-cached": ("cached=47", "cached=46"), "scene-leak": ("destroyed=47", "destroyed=46"),
        "no-driver": ("creates=2", "creates=0"), "not-distinct": ("distinct=1", "distinct=0"),
        "borrowed-input": ("queued=1", "queued=0"), "retained-input": ("released=1", "released=0"),
        "recipe-leak": ("modules=2/2", "modules=2/1"), "no-shutdown": ("devices=1", "devices=0"),
        "initial-mismatch": ("exact=1", "exact=0"), "bad-saved": ("validated=1", "validated=0"),
        "overflow": ("bytes=14048", "bytes=999999999999999999999999"),
        "noncanonical": ("failures=0", "failures=00"), "live-errors": ("liveErrors=0", "liveErrors=1"),
        "post-errors": ("deviceErrors=0", "deviceErrors=1"),
        "api-garbage": ("project-prewarm identity:", "garbage project-prewarm identity:"),
        "marker-junk": ("devices=1", "devices=1 junk"),
        "api-tail": ("effective=1.3", "effective=1.30"),
        "paths-tail": ("maintenance4=0", "maintenance4=0 junk"),
        "garbage-leak": ("Total device memory leaked:", "garbage Total device memory leaked:"),
    }
    for name, (before, after) in replacements.items():
        assert base.count(before) == 1, (name, before)
        fixtures.append((name, base.replace(before, after), runtime, "cold", None, False))
    for name, line in (("unknown-marker", "project-prewarm_unknown: malformed"),
                       ("duplicate-marker", "project-prewarm ownership: queued=1 released=1"),
                       ("foreign-footer", "other completed failures=1"), ("hazard", "SYNC-HAZARD-WAW"),
                       ("vuid", "VUID-vkCmdDraw-None-09600"), ("js-error", "Uncaught exception"),
                       ("save-failed-with-old-valid-file", "Vulkan pipeline cache save failed; previous file preserved.")):
        fixtures.append((name, base + "\n" + line, runtime, "cold", None, False))
    fixtures.append(("NTSTATUS", base, dict(runtime, exitCode=3221225477), "cold", None, False))
    fixtures.append(("wrong-native-zero", wrong, runtime, "wrong-key", None, False))
    fixtures.append(("second-chance", base, dict(runtime, exceptions=[{"exception": "0xe06d7363", "firstChance": False}]), "cold", None, False))
    results = []
    for name, log, actual_runtime, phase, link, expected in fixtures:
        result = judge(log, copy.deepcopy(actual_runtime), phase, link)
        results.append({"name": name, "pass": result["pass"] == expected, "actual": result["pass"]})
    failed = [result for result in results if not result["pass"]]
    print(json.dumps({"declared": len(results), "executed": len(results), "passed": len(results) - len(failed), "failures": failed}))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--case-dir", type=Path)
    parser.add_argument("--phase", choices=("off", "cold", "restored", "rejected", "wrong-key"))
    parser.add_argument("--expected-initial", type=int)
    args = parser.parse_args()
    if args.self_test:
        if args.case_dir or args.phase or args.expected_initial is not None:
            parser.error("self-test is a separate mode")
        return self_test()
    if not args.case_dir or not args.phase:
        parser.error("case-dir and phase required")
    try:
        config = json.loads((args.case_dir / "config.json").read_text(encoding="utf-8"))
        result = judge((args.case_dir / "debug-output.log").read_text(encoding="utf-8"),
                       json.loads((args.case_dir / "runtime-summary.json").read_text(encoding="utf-8")),
                       args.phase, args.expected_initial, config.get("env", {}).get("CC_VULKAN_FORCE_API"), config.get("expectedPaths"))
    except (OSError, ValueError) as exc:
        result = {"pass": False, "reasons": [str(exc)], "savedBytes": 0}
    (args.case_dir / "prewarm-judge.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
