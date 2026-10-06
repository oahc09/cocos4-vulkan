"""Strict evidence for synchronized upload batches; not game/display frame timing."""
import csv
import io
import re

def judge(summary, text, csv_text, mode, validation):
    try:
        def need(v, why):
            if not v: raise ValueError(why)
        need(mode in ("upload-perf", "upload-perf-baseline") and validation in ("on", "off"), "expected parameters")
        need(summary["exitCode"] == 0 and summary["errorLogMessages"] == 0 and all(x["firstChance"] for x in summary["exceptions"]), "native clean exit")
        need(not re.search(r"FAIL:|VUID-|HAZARD|uncaught exception|INCOMPLETE|UNSUPPORTED", text, re.I), "runtime failure")
        lines = [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR|WARN)\]: ", "", v) for v in text.splitlines() if v.strip()]
        def one(prefix, pattern):
            rows = [v for v in lines if prefix.lower() in v.lower()]
            need(len(rows) == 1 and re.fullmatch(pattern, rows[0]), "unique exact " + prefix)
        one("project-visibility mode", rf"project-visibility mode: {mode} enabled=1")
        one("project-visibility validation", "project-visibility validation: " + ("ENABLED" if validation == "on" else "DISABLED"))
        one("project-visibility samples", "project-visibility samples: warmup=300 count=1000 bytes=131072")
        one("project-visibility retirement", "project-visibility retirement: created=4 destroyed=4 devices=1")
        one("project-visibility completed", "project-visibility completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN")
        one("Total device memory leaked", r"Total device memory leaked: 0 bytes\.")
        one("Vulkan API", r"Vulkan API: loader=[0-9]+\.[0-9]+ requested=1.3 XR=1.3 instance=1.3 physical=[0-9]+\.[0-9]+ effective=1.3")
        one("Vulkan paths", r"Vulkan paths: timeline=1 sync2=0 submit2=0 dynamicRendering=0 classicOverride=1 maintenance4=[01]")
        need(sum("completed" in v.lower() for v in lines) == 1, "single footer")
        family = [v for v in lines if "project-visibility" in v.lower() and not v.startswith("Shader '")]
        need(len(family) == 1305, "1305 reserved lines")
        batches = [v for v in lines if "project-visibility batch" in v.lower()]
        need(len(batches) == 1300, "1300 submitted/golden batches")
        baseline = mode == "upload-perf-baseline"
        for i, v in enumerate(batches):
            need(re.fullmatch(rf"project-visibility batch: cycle={i} bytes=131072 managerCalls=1 global={int(baseline)} resources={0 if baseline else 2} matched={0 if baseline else 2} dispatches=1 copies=1 submits=1 managerSubmits=1 checkedBytes=131072 mismatches=0 pendingDestroy=0 resized={int(i == 1299)} uploads=8 stagingSources=[1-4]", v), "exact batch identity/content/retention")
        reader = csv.reader(io.StringIO(csv_text)); rows = list(reader)
        header = ["sample", "batch_ns", "upload_ns", "manager_ns", "compute_cb_gpu_ns", "checked_bytes", "staging_sources", "record_ns", "submit_ns", "completion_ns", "wait_api_ns", "wait_calls", "wait_failures"]
        need(len(rows) == 1001 and rows[0] == header, "CSV header + 1000 samples")
        parsed = []
        for i, row in enumerate(rows[1:]):
            need(len(row) == 13 and all(re.fullmatch(r"0|[1-9][0-9]*", v) for v in row), "canonical CSV numbers")
            v = list(map(int, row)); need(all(x <= 2**64 - 1 for x in v), "UInt64 range")
            need(v[0] == i and v[5] == 131072 and 1 <= v[6] <= 4 and v[1] > 0 and v[1] >= v[2] + v[3], "sample index/content/timing closure")
            need(v[1] == v[2] + v[3] + v[7] + v[8] + v[9] and v[10] <= v[9] and v[12] == 0 and (v[11] > 0 or v[10] == 0), "five-part host closure/actual wait results")
            parsed.append(dict(zip(header, v)))
        return {"Pass":True,"Rows":parsed,"Samples":1000,"CheckedBytes":131072000}
    except (ValueError, TypeError, KeyError) as e: return {"Pass":False,"Reasons":[str(e)]}
