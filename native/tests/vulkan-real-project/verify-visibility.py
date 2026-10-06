"""Offline judge for the real-project known-buffer visibility diagnostic."""
import argparse
import copy
import json
import re
from pathlib import Path


def need(value, reason):
    if not value:
        raise ValueError(reason)


def judge(summary, text, mode, api, sync2, submit2, timeline):
    if mode in ("stress", "stress-wrong"):
        return judge_stress(summary, text, mode, api, sync2, submit2, timeline)
    try:
        need(mode in ("content", "fallback", "wrong-input"), "expected mode")
        negative = mode == "wrong-input"
        need(summary["exitCode"] == int(negative), "exact native exit")
        need(all(x["firstChance"] for x in summary["exceptions"]), "no unhandled exception")
        lines = [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR|WARN)\]: ", "", v) for v in text.splitlines() if v.strip()]
        need(not re.search(r"VUID-|HAZARD|uncaught exception|UNSUPPORTED|INCOMPLETE", text, re.I), "Validation/runtime failure")
        family = [v for v in lines if "project-visibility" in v.lower() and not v.startswith("Shader '")]
        need(len(family) == 7, "seven reserved evidence lines")

        def one(prefix, pattern):
            rows = [v for v in lines if prefix.lower() in v.lower()]
            need(len(rows) == 1, "unique " + prefix)
            m = re.fullmatch(pattern, rows[0])
            need(m is not None, "exact " + prefix)
            return m

        one("project-visibility mode", rf"project-visibility mode: {mode} enabled=1")
        one("project-visibility validation", "project-visibility validation: ENABLED")
        need(sum("completed" in v.lower() for v in lines) == 1, "unique shutdown footer")
        one("project-visibility completed", rf"project-visibility completed: failures={3 if negative else 0} liveErrors=0 postErrors=0 outcome={'FAILED' if negative else 'CLEAN'}")
        one("project-visibility retirement", "project-visibility retirement: created=12 destroyed=12 devices=1")
        one("Total device memory leaked", r"Total device memory leaked: 0 bytes\.")
        # Version/policy evidence comes from the real backend, not requested env.
        one("Vulkan API", rf"Vulkan API: loader=[0-9]+\.[0-9]+ requested={re.escape(api)} XR={re.escape(api)} instance={re.escape(api)} physical=[0-9]+\.[0-9]+ effective={re.escape(api)}")
        one("Vulkan paths", rf"Vulkan paths: timeline={timeline} sync2={sync2} submit2={submit2} dynamicRendering=0 classicOverride=1 maintenance4=[01]")
        phases = [v for v in lines if "project-visibility phase" in v.lower()]
        need(len(phases) == 3, "three phases")
        for i, size in enumerate((64, 128, 64)):
            fallback = mode == "fallback"
            expected = rf"project-visibility phase: index={i} bytes={size} managerCalls=1 global={int(fallback)} resources={1 if fallback else 2} matched={1 if fallback else 2} dispatches=1 copies=1 submits=1 managerSubmits=1 checkedBytes={size} mismatches={int(negative)} pendingDestroy=0 resized=1"
            need(re.fullmatch(expected, phases[i]) is not None, "phase resource/content/lifetime evidence")
        failures = [v for v in lines if re.search(r"FAIL:", v, re.I)]
        if negative:
            need(failures == ["FAIL: visibility compute whole-buffer golden"] * 3, "exact content negative failures")
            need(summary["errorLogMessages"] == 6, "exact negative error messages")
        else:
            need(not failures and summary["errorLogMessages"] == 0, "clean native output")
        return {"Pass": True, "CheckedBytes": 256, "ContentNegative": negative}
    except (KeyError, TypeError, ValueError) as e:
        return {"Pass": False, "Reasons": [str(e)]}


def judge_stress(summary, text, mode, api, sync2, submit2, timeline):
    try:
        negative = mode == "stress-wrong"
        need(summary["exitCode"] == int(negative), "exact native stress exit")
        need(all(x["firstChance"] for x in summary["exceptions"]), "no unhandled stress exception")
        lines = [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR|WARN)\]: ", "", v) for v in text.splitlines() if v.strip()]
        need(not re.search(r"VUID-|HAZARD|uncaught exception|UNSUPPORTED|INCOMPLETE", text, re.I), "stress Validation/runtime failure")
        family = [v for v in lines if "project-visibility" in v.lower() and not v.startswith("Shader '")]
        need(len(family) == 52, "52 stress reserved lines")
        def one(prefix, pattern):
            rows = [v for v in lines if prefix.lower() in v.lower()]
            need(len(rows) == 1 and re.fullmatch(pattern, rows[0]), "unique exact " + prefix)
        one("project-visibility mode", rf"project-visibility mode: {mode} enabled=1")
        one("project-visibility validation", "project-visibility validation: ENABLED")
        one("project-visibility retirement", "project-visibility retirement: created=96 destroyed=96 devices=1")
        one("project-visibility completed", rf"project-visibility completed: failures={2 if negative else 0} liveErrors=0 postErrors=0 outcome={'FAILED' if negative else 'CLEAN'}")
        need(sum("completed" in v.lower() for v in lines) == 1, "one shutdown footer")
        one("Total device memory leaked", r"Total device memory leaked: 0 bytes\.")
        one("Vulkan API", rf"Vulkan API: loader=[0-9]+\.[0-9]+ requested={re.escape(api)} XR={re.escape(api)} instance={re.escape(api)} physical=[0-9]+\.[0-9]+ effective={re.escape(api)}")
        one("Vulkan paths", rf"Vulkan paths: timeline={timeline} sync2={sync2} submit2={submit2} dynamicRendering=0 classicOverride=1 maintenance4=[01]")
        phases = [v for v in lines if "project-visibility phase" in v.lower()]
        textures = [v for v in lines if "project-visibility texture" in v.lower()]
        need(len(phases) == len(textures) == 24, "24 independent buffer and texture waves")
        buffer_bytes = texture_bytes = 0
        for i in range(24):
            size = 131072 if i % 2 else 65536
            width = 257 if i % 2 else 129
            image_bytes = width * 127 * 4
            mismatch = int(negative and i == 23)
            need(re.fullmatch(rf"project-visibility phase: index={i} bytes={size} managerCalls=1 global=0 resources=2 matched=2 dispatches=1 copies=1 submits=1 managerSubmits=1 checkedBytes={size} mismatches={mismatch} pendingDestroy=0 resized=1", phases[i]), "stress full buffer chain/golden/lease")
            need(re.fullmatch(rf"project-visibility texture: index={i} width={width} height=127 uploads=4 copies=1 submits=1 checkedBytes={image_bytes} mismatches={mismatch}", textures[i]), "stress actual texture uploads/full golden")
            buffer_bytes += size
            texture_bytes += image_bytes
        failures = [v for v in lines if re.search(r"FAIL:", v, re.I)]
        expected_failures = ["FAIL: visibility compute whole-buffer golden", "FAIL: visibility texture whole-image golden"] if negative else []
        need(failures == expected_failures and summary["errorLogMessages"] == (4 if negative else 0), "exact stress failures")
        return {"Pass": True, "Waves": 24, "BufferCheckedBytes": buffer_bytes, "TextureCheckedBytes": texture_bytes, "ContentNegative": negative}
    except (KeyError, TypeError, ValueError) as e:
        return {"Pass": False, "Reasons": [str(e)]}


def self_test(summary, text, expected):
    results = []
    results.append(judge(summary, text, *expected)["Pass"])
    changes = [("global=0", "global=1"), ("resources=2", "resources=1"), ("matched=2", "matched=1"),
               ("dispatches=1", "dispatches=0"), ("copies=1", "copies=0"), ("submits=1", "submits=0"), ("managerSubmits=1", "managerSubmits=0"),
               ("checkedBytes=64", "checkedBytes=0"), ("mismatches=0", "mismatches=1"),
               ("pendingDestroy=0", "pendingDestroy=1"), ("resized=1", "resized=0"),
               ("destroyed=12", "destroyed=11"), ("liveErrors=0", "liveErrors=1"),
               ("postErrors=0", "postErrors=1"), ("effective=1.3", "effective=1.30")]
    for old, new in changes:
        need(old in text, "fixture mutation present " + old)
        results.append(not judge(summary, text.replace(old, new, 1), *expected)["Pass"])
    for extra in ("foo completed failures=1", "project-visibilityphase: junk", "[INFO]: FAIL: broken", "SYNC-HAZARD-WAW", "VUID-test"):
        results.append(not judge(summary, text + "\n" + extra, *expected)["Pass"])
    broken = copy.deepcopy(summary)
    broken["exitCode"] = 0xC0000005
    results.append(not judge(broken, text, *expected)["Pass"])
    broken = copy.deepcopy(summary)
    broken["exceptions"].append({"firstChance": False})
    results.append(not judge(broken, text, *expected)["Pass"])
    need(all(results), "all single-mutation fixtures")
    return {"Declared": len(results), "Executed": len(results), "Passed": sum(results)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("root", type=Path)
    p.add_argument("--self-test", action="store_true")
    a = p.parse_args()
    cases = [("legacy", "content", "1.3", 0, 0, 1), ("sync2", "content", "1.3", 1, 1, 1),
             ("fence11", "content", "1.1", 0, 0, 0), ("fallback", "fallback", "1.3", 1, 1, 1),
             ("wrong-input", "wrong-input", "1.3", 1, 1, 1)]
    results = []
    for name, *expected in cases:
        d = a.root / name
        summary = json.loads((d / "runtime-summary.json").read_text())
        text = (d / "debug-output.log").read_text(encoding="utf-8")
        result = judge(summary, text, *expected)
        results.append({"Case": name, **result})
        if a.self_test and name == "legacy":
            print(json.dumps(self_test(summary, text, expected)))
    (a.root / "judgment.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results))
    need(all(v["Pass"] for v in results), "native visibility matrix")


if __name__ == "__main__":
    main()
