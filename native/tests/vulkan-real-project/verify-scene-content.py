"""Judge finite Bloom/Compute captures plus real-project native runs.

RenderDoc replay extraction is required; package presence or shader creation
alone never supplies a positive content result. No build or GPU execution here.
"""
import argparse
import copy
import hashlib
import json
import re
import struct
from pathlib import Path


def require(value, reason):
    if not value:
        raise ValueError(reason)


def load_capture(folder):
    data = json.loads((folder / "inspect.json").read_text())
    blobs = {}
    for action in data["actions"]:
        names = [action["shaderFile"]]
        for group in ("outputs", "storageOutputs", "constantBlocks"):
            names += [item["file"] for item in action.get(group, []) if "file" in item]
        for name in names:
            blobs[name] = (folder / name).read_bytes()
    return data, blobs


def image(action, blobs, group="outputs"):
    outputs = action.get(group, [])
    require(len(outputs) == 1, "exactly one captured image required")
    item = outputs[0]
    require(item["format"] in ("R8G8B8A8_UNORM", "B8G8R8A8_UNORM"), "unsupported finite content format")
    raw = blobs[item["file"]]
    require(len(raw) == item["bytes"] == item["width"] * item["height"] * 4, "image byte size mismatch")
    if item["format"] == "B8G8R8A8_UNORM":
        raw = b"".join(bytes((b, g, r, a)) for r, g, b, a in struct.iter_unpack("4B", raw))
    return item, raw


def constants(action, blobs, name):
    blocks = [b for b in action["constantBlocks"] if b.get("name") == name]
    require(len(blocks) == 1, "missing/duplicate actual uniform block: " + name)
    block = blocks[0]
    raw = blobs[block["file"]]
    require(len(raw) == block["bytes"] == block["binding"]["byteSize"], "uniform byte size mismatch")
    return struct.unpack("<%df" % (len(raw) // 4), raw)


def clean_replay(data):
    require(data.get("Complete") is True, "incomplete replay")
    require(not any(re.search(r"VUID-|HAZARD|FAIL:", m["description"]) for m in data["messages"]), "replay diagnostic error")


def bloom(data, blobs, threshold):
    clean_replay(data)
    actions = data["actions"]
    marked = [i for i, a in enumerate(actions) if any(b.get("name") == "BloomUBO_var" for b in a["constantBlocks"])]
    require(len(marked) == 2 and marked[1] - marked[0] == 7, "missing eight-pass Bloom chain")
    first, last = marked
    require(first > 0 and last + 1 < len(actions), "missing scene input or final copy")
    base, source = image(actions[first - 1], blobs)
    chain = actions[first:last + 1]
    require(all(a["api"] == "vkCmdDrawIndexed()" for a in chain), "Bloom pass did not draw")
    params = constants(chain[0], blobs, "BloomUBO_var")
    combine = constants(chain[-1], blobs, "BloomUBO_var")
    require(len(params) == len(combine) == 4 and abs(params[2] - threshold) < 1e-7 and abs(combine[2] - threshold) < 1e-7 and combine[3] == 1, "actual threshold/intensity differs")
    expected = [(400, 300), (200, 150), (100, 75), (50, 37), (100, 75), (200, 150), (400, 300), (800, 600)]
    previous = base["id"]
    output_bytes = []
    for index, (action, dimensions) in enumerate(zip(chain, expected)):
        item, raw = image(action, blobs)
        require((item["width"], item["height"]) == dimensions, "Bloom dimensions/iteration count differ")
        inputs = [b["descriptor"]["resource"] for b in action["readOnlyBindings"]]
        require(previous in inputs, "broken Bloom texture dependency")
        previous = item["id"]
        output_bytes.append(raw)
    require(previous == base["id"], "combine did not return to scene target")
    _, copied = image(actions[last + 1], blobs)
    require(copied == output_bytes[-1], "final copy differs from Bloom combine")
    return source, output_bytes, chain


def judge_bloom(low, high):
    source_lo, images_lo, chain_lo = bloom(*low, .05)
    source_hi, images_hi, chain_hi = bloom(*high, 1.0)
    require(source_lo == source_hi, "scene input changed; threshold comparison is not isolated")
    for a, b in zip(chain_lo, chain_hi):
        require(low[1][a["shaderFile"]] == high[1][b["shaderFile"]], "shader changed between thresholds")
    require(any(images_lo[0][i] for i in range(len(images_lo[0])) if i % 4 < 3), "low threshold produced no Bloom content")
    require(all(images_hi[0][i] == 0 for i in range(len(images_hi[0])) if i % 4 < 3), "high threshold prefilter was not zero")
    require(images_hi[-1] == source_hi, "zero Bloom contribution changed scene content")
    diff = sum(a != b for a, b in zip(images_lo[-1], images_hi[-1]))
    require(diff > 0, "threshold had no content effect")
    return {"passes": 8, "sourceIdentical": True, "sourceBytes": len(source_lo), "thresholds": [.05, 1.0], "combinedDifferentBytes": diff, "sourceSHA256": hashlib.sha256(source_lo).hexdigest(), "sliderVerified": False}


def judge_compute(capture):
    data, blobs = capture
    clean_replay(data)
    dispatches = [a for a in data["actions"] if a["api"] == "vkCmdDispatch()"]
    require(len(dispatches) == 1, "exactly one actual dispatch required")
    dispatch = dispatches[0]
    values = constants(dispatch, blobs, "b_sphereBuffer_var")
    require(values == (0, 0, -1, .5, 0, -100.5, -1, 100), "explicit sphere buffer was not bound")
    require(dispatch["dimensions"] == [100, 150, 1] and dispatch["reflection"]["dispatchThreadsDimension"] == "(8, 4, 1)", "dispatch/local size mismatch")
    storage, raw = image(dispatch, blobs, "storageOutputs")
    require((storage["width"], storage["height"]) == (800, 600), "finite Compute extent differs")
    draws = [a for a in data["actions"] if a["api"] == "vkCmdDrawIndexed()"]
    require(len(draws) == 1 and draws[0]["event"] > dispatch["event"], "missing downstream fullscreen draw")
    require(storage["id"] in [x["descriptor"]["resource"] for x in draws[0]["readOnlyBindings"]], "fullscreen draw did not sample storage result")
    _, output = image(draws[0], blobs)
    flipped = b"".join(raw[y * 3200:(y + 1) * 3200] for y in range(599, -1, -1))
    require(output == raw or output == flipped, "fullscreen output differs from storage image")
    checked = 0
    # Upper quarter rays miss both declared spheres. This is a finite gradient
    # golden, not a claim about the effect's entire ray-intersection algorithm.
    for y in range(480, 600):
        expected = (round(255 * (1 - .5 * y / 600)), round(255 * (1 - .3 * y / 600)), 255, 255)
        for x in range(800):
            pixel = raw[(y * 800 + x) * 4:(y * 800 + x + 1) * 4]
            require(all(abs(a - b) <= 1 for a, b in zip(pixel, expected)), "Compute background golden mismatch")
            checked += 1
    center = raw[(300 * 800 + 400) * 4:(300 * 800 + 401) * 4]
    require(all(abs(a - b) <= 1 for a, b in zip(center, (128, 128, 255, 255))), "sphere-center golden mismatch")
    return {"dispatch": [100, 150, 1], "localSize": [8, 4, 1], "sphereBytes": 32, "storageBytes": len(raw), "goldenBackgroundPixels": checked, "sphereCenter": list(center), "fullscreenExact": True, "flipped": output == flipped}


def native_run(folder, expected_paths, scene):
    summary = json.loads((folder / "runtime-summary.json").read_text())
    log = (folder / "debug-output.log").read_text(errors="replace")
    require(summary["exitCode"] == 0 and summary["errorLogMessages"] == 0, "native exit/errors failed: " + folder.name)
    require(not re.search(r"VUID-|SYNC-HAZARD-|\[ERROR\]|Uncaught Exception|Assertion failed|corrupted", log, re.I), "native correctness error: " + folder.name)
    require("Total device memory leaked: 0 bytes." in log, "missing native zero-leak shutdown")
    require(any("VkLayer_khronos_validation.dll" in m["path"] for m in summary["modules"]), "Validation DLL was not loaded")
    require("validation unavailable" not in log.lower() and "validation features unavailable" not in log.lower(), "Validation unavailable")
    for text in expected_paths:
        require(text in log, "missing actual runtime path: " + text)
    if scene:
        require("Success to load scene: db://assets/cases/" + scene in log, "scene did not load")
    return summary["exeSHA256"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        root = args.evidence_root.resolve(strict=True)
        low = load_capture(root / "renderdoc-bloom-low-final")
        high = load_capture(root / "renderdoc-bloom-high-final")
        compute = load_capture(root / "renderdoc-compute-clean-final")
        results = {"Bloom": judge_bloom(low, high), "Compute": judge_compute(compute)}
        hashes = []
        for case, paths, scene in [
            ("compute-modern-final", ["timeline=1 sync2=1 submit2=1"], "GFX/compute-demo/compute-demo.scene"),
            ("compute-legacy-final", ["timeline=0 sync2=0 submit2=0"], "GFX/compute-demo/compute-demo.scene"),
            ("bloom-current-final", ["timeline=1 sync2=1 submit2=1"], "postprocess/bloom.scene"),
            ("default-current-final", ["project-prewarm mode: phase=off enabled=0"], None),
        ]:
            hashes.append(native_run(root / case, paths, scene))
        require(len(set(hashes)) == 1, "native cases use different exes")
        for case in ("bloom-low", "bloom-high", "compute-clean"):
            state = json.loads((root / ("renderdoc-" + case + "-final") / "capture-state.json").read_text())
            require(state["state"] == "CAPTURED" and state["exeSHA256"] == hashes[0], "capture/current-exe mismatch")
        results.update(exeSHA256=hashes[0], nativeRuns=4, overallGate=False)
        if args.self_test:
            failed = []
            tests = []
            bad = copy.deepcopy(compute)
            block = bad[0]["actions"][0]["constantBlocks"][0]
            bad[1][block["file"]] = bytes(32)
            tests.append(("zero-sphere-buffer", lambda: judge_compute(bad)))
            no_dispatch = copy.deepcopy(compute)
            no_dispatch[0]["actions"] = no_dispatch[0]["actions"][1:]
            tests.append(("no-dispatch", lambda: judge_compute(no_dispatch)))
            tests.append(("identical-thresholds", lambda: judge_bloom(low, low)))
            broken = copy.deepcopy(low)
            broken[0]["actions"][3]["readOnlyBindings"][0]["descriptor"]["resource"] = "ResourceId::0"
            tests.append(("broken-texture-chain", lambda: judge_bloom(broken, high)))
            old = load_capture(root / "renderdoc-compute-clean")
            tests.append(("actual-old-zero-uniform-capture", lambda: judge_compute(old)))
            for name, test in tests:
                try:
                    test()
                    failed.append(name)
                except (ValueError, KeyError, OSError):
                    pass
            require(not failed, "negative fixtures falsely passed: " + str(failed))
            results["negativeFixtures"] = len(tests)
        print(json.dumps(results, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print("FAILED:", error)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
