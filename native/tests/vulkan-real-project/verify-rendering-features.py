"""Offline, fail-closed judges for actual-project resolve/Secondary/FSR evidence.

Native records include actual project shutdown. Replay records are independent
GPU captures. This script does not build, launch or change environment policies.
"""
import argparse
import copy
import json
import re
import struct
from pathlib import Path


def require(value, reason):
    if not value:
        raise ValueError(reason)


def lines(log):
    return [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR)\]: ", "", x)
            for x in log.splitlines() if x.strip()]


def only(items, prefix, expression):
    found = [x for x in items if prefix.lower() in x.lower()]
    require(len(found) == 1, "unique " + prefix)
    result = re.fullmatch(expression, found[0])
    require(result is not None, "exact " + prefix)
    return result


def common(summary, log):
    require(summary["exitCode"] == 0, "native exit")
    require(summary["errorLogMessages"] == 0, "native errors")
    require(not any(not x["firstChance"] for x in summary["exceptions"]), "unhandled exception")
    require(not re.search(r"VUID-|HAZARD|FAIL:|failure:|uncaught exception|UNSUPPORTED|INCOMPLETE", log, re.I), "runtime error")
    items = lines(log)
    require(sum("completed" in x.lower() for x in items) == 1, "one complete shutdown footer")
    only(items, "Total device memory leaked", r"Total device memory leaked: 0 bytes\.")
    return items


def resolve_native(summary, log, kind, samples, scopes=False):
    require(kind in ("color", "depth") and samples in (2, 4), "resolve expectation")
    items = common(summary, log)
    require(sum("project-resolve" in x.lower() for x in items) == (4 if scopes else 3), "resolve reserved family")
    if scopes:
        only(items, "project-resolve scopes", r"project-resolve scopes: posts=4 precise=4 broad=0 failures=0")
        only(items, "validation + best practices + sync", r"validation \+ best practices \+ sync: ENABLED")
    only(items, "project-resolve mode", rf"project-resolve mode: {kind}{samples} enabled=1")
    only(items, "project-resolve retirement", rf"project-resolve retirement: kind={kind} created=4 destroyed=4 devices=1 liveErrors=0 postErrors=0")
    only(items, "project-resolve completed", rf"project-resolve completed: kind={kind} samples={samples} diagnosticResult=0 platformResult=0 result=0 outcome=CLEAN")
    family = "msaa" if kind == "color" else "dsr"
    expected_count = 13 if kind == "color" else 9
    # Exclude the two shader compilation messages from the reserved evidence family.
    evidence = [x for x in items if not x.startswith("Shader ") and re.search(rf"\b{family}(?:-|\s+[24]x)", x, re.I)]
    require(len(evidence) == expected_count, "resolve evidence count")
    if kind == "color":
        only(items, "msaa-config", rf"msaa-config: samples={samples} resolve=1 pixels=187 hostBytes=748 runtimeDynamic=1")
    else:
        only(items, "dsr-config", rf"dsr-config: samples={samples} depthFormat=126 resolveMode=SAMPLE_ZERO pixels=187 hostBytes=1496 runtimeDynamic=1")
        only(items, "dsr-modes", r"dsr-modes: depth=1 stencil=1 \(Classic filtered values\)")
    only(items, family + "-pipelines", rf"{family}-pipelines: classic=2 dynamic=2 signatures=OK samples={samples}")
    phases = [x for x in items if (family + "-phase").lower() in x.lower()]
    readbacks = [x for x in items if (family + "-readback").lower() in x.lower()]
    patterns = ("uniform", "edge") if kind == "color" else (None,)
    require(len(phases) == len(readbacks) == 2 * len(patterns), "phase/readback count")
    i = 0
    for mode in ("classic", "dynamic"):
        rp, dynamic = ("1/1", "0/0") if mode == "classic" else ("0/0", "1/1")
        for pattern in patterns:
            prefix = f"{family}-phase: mode={mode}" + (f" pattern={pattern}" if pattern else "")
            shape = (rf"{prefix} cb=([0-9A-Fa-f]+) rp={rp} dynamic={dynamic} draws="
                     + ("1 copies=1" if kind == "color" else "2 copies=1/1")
                     + r" submits=1/1 chain=1")
            match = re.fullmatch(shape, phases[i])
            require(match is not None and int(match[1], 16) > 0, "actual CB/draw/copy/submit chain")
            if kind == "color":
                match = re.fullmatch(rf"msaa-readback: mode={mode} pattern={pattern} bytes=748 checkedBytes=748 red=([0-9]+) blue=([0-9]+) mixed=([0-9]+) mismatch=0", readbacks[i])
                require(match is not None, "full color golden")
                red, blue, mixed = map(int, match.groups())
                require(red + blue + mixed == 187, "every pixel classified")
                require((red, blue, mixed) == (187, 0, 0) if pattern == "uniform" else red > 0 and blue > 0 and mixed > 0, "uniform/edge content")
            else:
                require(re.fullmatch(rf"dsr-readback: mode={mode} colorCheckedBytes=748 depthCheckedPixels=187 colorMismatch=0 depthMismatch=0", readbacks[i]) is not None, "independent color/depth golden")
            i += 1
    if kind == "color":
        comparisons = [x for x in items if "msaa-compare" in x.lower()]
        require(comparisons == [f"msaa-compare: pattern={p} bytes=748 differences=0" for p in patterns], "both independent snapshot comparisons")
    else:
        only(items, "dsr-compare", r"dsr-compare: colorBytes=748 colorDiff=0 depthBytes=748 depthDiff=0")
    only(items, family + f" {samples}x", rf"{family} {samples}x readback: failures=0 liveErrors=0 postErrors=0 mismatches=0 outcome=CLEAN")
    return True


def features_native(summary, log, mode, legacy=False):
    require(mode in ("secondary", "fsr"), "feature expectation")
    items = common(summary, log)
    require(sum("project-features" in x.lower() for x in items) == (8 if mode == "fsr" else 7), "feature reserved family")
    only(items, "project-features mode", rf"project-features mode: {mode} enabled=1")
    only(items, "project-features validation", r"project-features validation: ENABLED")
    if mode == "fsr":
        only(items, "project-features FSR", r"project-features FSR: attachmentEnabled=1 rate2x2=1 texel=16,16 format=R8_UINT")
    phases = [x for x in items if "project-features phase" in x.lower()]
    require(len(phases) == 3, "three resource waves")
    pipelines = 1 if mode == "fsr" or legacy else 2
    secondary, fsr = int(mode == "secondary"), int(mode == "fsr")
    for i, (w, h) in enumerate(((32, 16), (24, 12), (32, 16))):
        require(re.fullmatch(rf"project-features phase: index={i} width={w} height={h} rpContract=1 pipelines={pipelines} inheritance={secondary} executes={secondary} fsrNode={fsr} begins=1 ends=1 dynamic=0 draws=1 copies=1 submits=1 chain=1 checkedBytes={w*h*4} mismatch=0 pendingDestroy=0", phases[i]) is not None, "actual inheritance/FSR/pending chain and pixels")
    count = 15 + 3 * pipelines
    facades = 6 if mode == "fsr" else 3
    only(items, "project-features retirement", rf"project-features retirement: created={count} destroyed={count} devices=1 bufferFacades={facades} bufferDrops={facades} imageFacades={facades} imageDrops={facades}")
    only(items, "project-features completed", r"project-features completed: failures=0 liveErrors=0 postErrors=0 outcome=CLEAN")
    return True


def failure_native(summary, log):
    common(summary, log)
    items = lines(log)
    recoveries = [x for x in items if "project-features recovery" in x.lower()]
    require(recoveries == [f"project-features recovery: index={i} injections=1 partialDestroyed=1 variantNull=1 classicValid=1" for i in range(3)], "three real partial-result cleanup/fallback waves")
    require(sum("project-features" in x.lower() for x in items) == 10, "failure family count")
    only(items, "project-features mode", r"project-features mode: secondary-variant-failure enabled=1")
    clean = "\n".join(x for x in items if "project-features recovery" not in x.lower())
    # The remaining exact inheritance/execute/golden/retirement contract is
    # identical to the normal Secondary path. Normalization follows the exact
    # failure-mode and recovery checks above; it cannot discard arbitrary lines.
    features_native(summary, clean.replace("mode: secondary-variant-failure", "mode: secondary"), "secondary")
    require(sum("Dynamic pipeline variant creation failed (result=-1)" in x for x in items) == 3, "production factory observed injected result")
    return True


def replay_common(data):
    require(data.get("Complete") is True, "complete GPU replay")
    require(not any(re.search(r"VUID-|HAZARD|FAIL:", x["description"], re.I) for x in data["messages"]), "replay diagnostic")


def color_replay(data, blobs):
    replay_common(data)
    require(len(data["actions"]) == 4, "four actual resolve draws")
    require(data["apiCounts"]["vkCmdCopyImageToBuffer"] == 4, "four actual resolve copies")
    values = []
    for i, a in enumerate(data["actions"]):
        require(a["dynamic"] == (i >= 2), "Classic/Dynamic mode")
        primary = next(x for x in a["attachmentTextures"] if x["position"] == 0)
        target = next(x for x in a["attachmentTextures"] if x["position"] == 1)
        require(primary["samples"] == 4 and target["samples"] == 1, "actual primary/resolve samples")
        require((target["width"], target["height"], target["format"]) == (17, 11, "R8G8B8A8_UNORM"), "resolved image shape")
        raw = blobs[target["file"]]; require(len(raw) == 748, "full resolved bytes")
        if i % 2 == 0:
            require(raw == bytes((255, 0, 0, 255)) * 187, "actual uniform golden")
        else:
            counts = [0, 0, 0]
            for pixel in range(187):
                r, g, b, alpha = raw[4*pixel:4*pixel+4]
                require(g == 0 and alpha == 255 and abs(r+b-255) <= 1, "edge channels")
                require(any(abs(r-round(255*k/4)) <= 1 for k in range(5)), "4x resolve quantization")
                x, y = pixel % 17, pixel // 17
                distance = 1 - (x+.5)/17 - (y+.5)/11
                margin = 1/17 + 1/11
                require(distance <= margin or r == 255, "inside triangle red")
                require(distance >= -margin or b == 255, "outside triangle blue")
                counts[0 if r == 255 else 1 if b == 255 else 2] += 1
            require(all(x > 0 for x in counts), "red/blue/mixed coverage")
        values.append(raw)
    require(values[0] == values[2] and values[1] == values[3], "independent replay mode comparison")
    return True


def depth_replay(data, structured, blobs):
    replay_common(data)
    require(len(data["actions"]) == 4 and structured.get("Complete") is True, "depth replay complete")
    require(data["apiCounts"]["vkCmdCopyImageToBuffer"] == 4, "actual color/depth copies")
    classic = next(x for x in data["actions"][1]["attachmentTextures"] if x["position"] == 3)
    require(classic["samples"] == 1 and classic["format"] == "D32", "Classic resolve attachment")
    def children(n):
        return {x["name"]: x for x in n.get("children", [])}
    begins = [x for x in structured["chunks"] if "BeginRendering" in x["name"]]
    require(len(begins) == 1, "one actual Dynamic depth pass")
    info = children(begins[0])["pRenderingInfo"]
    depth = children(children(info)["pDepthAttachment"])
    require(depth["resolveMode"]["value"] == 1 and depth["resolveImageLayout"]["value"] == 3, "actual SAMPLE_ZERO payload")
    view = depth["resolveImageView"]["value"]
    resources = {x["id"]: x for x in structured["resources"]}
    finals = {x["id"]: x for x in structured["finalTextures"]}
    parents = [x for x in resources[view]["parents"] if x in finals]
    require(len(parents) == 1 and parents[0] != classic["id"], "actual resolve view to independent image link")
    for image in (classic["id"], parents[0]):
        t = finals[image]
        require((t["width"], t["height"], t["samples"], t["format"]) == (17, 11, 1, "D32"), "actual final D32 shape")
        raw = blobs[t["file"]]; require(len(raw) == 748, "full depth bytes")
        require(all(x == .25 for x in struct.unpack("<187f", raw)), "depth golden after actual resolve")
    for i in (1, 3):
        a = data["actions"][i]
        require(a["dynamic"] == (i == 3), "depth pipeline mode")
        require(a["depthState"]["depthTestEnable"] and a["depthState"]["depthWriteEnable"], "actual depth state")
        primary = next(x for x in a["attachmentTextures"] if x["position"] == 2)
        require(primary["samples"] == 4 and primary["format"] == "D32", "actual MS depth primary")
        color = next(x for x in a["attachmentTextures"] if x["position"] == 1)
        require(blobs[color["file"]] == bytes((255, 0, 0, 255)) * 187, "near wins/far rejected color golden")
    return True


def feature_replay(data, blobs, mode):
    replay_common(data)
    require(len(data["actions"]) == 3, "three actual feature draws")
    require(data["apiCounts"]["vkCmdCopyImageToBuffer"] == 3, "three feature copies")
    require(data["apiCounts"]["vkCmdExecuteCommands"] == (3 if mode == "secondary" else 0), "actual secondary execution")
    require(data["apiCounts"]["vkCmdBeginRenderPass"] == 3 and data["apiCounts"]["vkQueueSubmit2"] == 3, "actual feature pass/submit path")
    for a, (w, h) in zip(data["actions"], ((32, 16), (24, 12), (32, 16))):
        require(not a["dynamic"], "Classic required feature path")
        target = next(x for x in a["attachmentTextures"] if x["position"] == 0)
        require((target["width"], target["height"], target["format"]) == (w, h, "R8G8B8A8_UNORM"), "feature image shape")
        golden = bytes((255, 0, 0, 255))*w*h if mode == "fsr" else (bytes((255, 0, 0, 255))*(w//2)+bytes((0, 255, 0, 255))*(w//2))*h
        require(blobs[target["file"]] == golden, "independent feature full pixels")
        if mode == "fsr":
            require(a["renderpass"]["shadingRateAttachment"] == 1 and a["renderpass"]["shadingRateTexelSize"] == "(16, 16)", "actual FSR attachment contract")
            rate = next(x for x in a["attachmentTextures"] if x["position"] == 1)
            require((rate["width"], rate["height"], rate["samples"], rate["format"]) == ((w+15)//16, (h+15)//16, 1, "R8_UINT"), "FSR image shape")
            require(blobs[rate["file"]] == bytes((5,))*rate["width"]*rate["height"], "actual 2x2 rate bytes")
            raw = blobs[a["shaderFile"]]; require(len(raw) >= 20 and len(raw) % 4 == 0, "FSR SPIR-V shape")
            words = struct.unpack("<%dI" % (len(raw)//4), raw)
            require(words[0] == 0x07230203, "FSR SPIR-V magic")
            at, reads_rate = 5, False
            while at < len(words):
                size, op = words[at] >> 16, words[at] & 0xffff
                require(size > 0 and at + size <= len(words), "FSR SPIR-V instruction")
                # OpDecorate(target, BuiltIn, ShadingRateKHR), not a shader-name claim.
                reads_rate |= op == 71 and size == 4 and words[at+2] == 11 and words[at+3] == 4444
                at += size
            require(reads_rate, "actual shader reads ShadingRateKHR")
    return True


def load_case(root, name):
    d = root / name
    return json.loads((d / "runtime-summary.json").read_text()), (d / "debug-output.log").read_text(encoding="utf-8")


def load_replay(root):
    return json.loads((root / "inspect.json").read_text()), {x.name: x.read_bytes() for x in root.iterdir() if x.suffix in (".bin", ".spv")}


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("resolve_root", type=Path); p.add_argument("features_root", type=Path)
    p.add_argument("--failure-root", type=Path)
    p.add_argument("--self-test", action="store_true"); args = p.parse_args()
    records = {}
    for name in ("color2", "color4", "depth2", "depth4", "color2-fence", "depth2-sync2"):
        summary, log = load_case(args.resolve_root, name)
        resolve_native(summary, log, "color" if name.startswith("color") else "depth", int(name[5 if name.startswith("color") else 5]))
        records[name] = True
    for name, mode, legacy in (("secondary", "secondary", False), ("secondary-legacy", "secondary", True), ("fsr", "fsr", False)):
        features_native(*load_case(args.features_root, name), mode, legacy); records[name] = True
    data, blobs = load_replay(args.resolve_root / "renderdoc-color4"); color_replay(data, blobs)
    d, b = load_replay(args.resolve_root / "renderdoc-depth4")
    depth_replay(d, json.loads((args.resolve_root / "renderdoc-depth4/structured-depth.json").read_text()), b)
    for mode in ("secondary", "fsr"):
        data, blobs = load_replay(args.features_root / ("renderdoc-"+mode)); feature_replay(data, blobs, mode)
    if args.failure_root:
        failure_native(*load_case(args.failure_root, "variant-failure"))
        features_native(*load_case(args.failure_root, "secondary-regression"), "secondary")
        features_native(*load_case(args.failure_root, "fsr-regression"), "fsr")
    fixtures = 0
    if args.self_test:
        summary, log = load_case(args.resolve_root, "color4")
        mutations = [(copy.deepcopy(summary), log.replace(a, b, 1)) for a, b in (
            ("mismatch=0", "mismatch=1"), ("checkedBytes=748", "checkedBytes=0"),
            ("copies=1", "copies=0"), ("draws=1", "draws=0"), ("submits=1/1", "submits=0/0"),
            ("destroyed=4", "destroyed=3"), ("postErrors=0", "postErrors=1"),
            ("result=0 outcome=CLEAN", "result=1 outcome=FAILED"),
            ("msaa-config:", "garbage msaa-config:"))]
        for extra in ("MSAA-phasebroken", "project-resolve completed broken", "foo completed failures=1", "FAIL: bad", "VUID-invalid", "Total device memory leaked: 1 bytes."):
            mutations.append((copy.deepcopy(summary), log+"\n"+extra))
        wrong = copy.deepcopy(summary); wrong["exitCode"] = 0xC0000005; mutations.append((wrong, log))
        for s, l in mutations:
            try: resolve_native(s, l, "color", 4)
            except (ValueError, KeyError): fixtures += 1
            else: raise ValueError("mutation falsely accepted")
        for name in ("skip-color", "wrong-uniform", "skip-depth"):
            s, l = load_case(args.resolve_root, name)
            require(s["exitCode"] == 1 and "mismatches=0 outcome=CLEAN" not in l, "actual negative native")
            require(not re.search(r"VUID-|HAZARD", l), "negative validation clean")
            try: resolve_native(s, l, "depth" if name == "skip-depth" else "color", 2)
            except ValueError: fixtures += 1
            else: raise ValueError("actual negative accepted")
        for name in ("skip-execute", "fsr-wrong-rate"):
            s, l = load_case(args.features_root, name)
            require(s["exitCode"] == 1 and "outcome=FAILED" in l and not re.search(r"VUID-|HAZARD", l), "actual feature negative")
            try: features_native(s, l, "fsr" if name.startswith("fsr") else "secondary")
            except ValueError: fixtures += 1
            else: raise ValueError("actual feature negative accepted")
        s, log = load_case(args.features_root, "secondary")
        for before, after in (("executes=1", "executes=0"), ("inheritance=1", "inheritance=0"),
                              ("pendingDestroy=0", "pendingDestroy=1"), ("destroyed=21", "destroyed=20"),
                              ("mismatch=0", "mismatch=1"), ("bufferDrops=3", "bufferDrops=0")):
            try: features_native(s, log.replace(before, after, 1), "secondary")
            except ValueError: fixtures += 1
            else: raise ValueError("feature mutation accepted")
        fsr_data, fsr_blobs = load_replay(args.features_root / "renderdoc-fsr")
        for target, member, value in (("renderpass", "shadingRateAttachment", -1),
                                      ("renderpass", "shadingRateTexelSize", "(1, 1)")):
            bad = copy.deepcopy(fsr_data); bad["actions"][0][target][member] = value
            try: feature_replay(bad, fsr_blobs, "fsr")
            except ValueError: fixtures += 1
            else: raise ValueError("FSR replay mutation accepted")
        bad_blobs = fsr_blobs.copy()
        rate = next(x for x in fsr_data["actions"][0]["attachmentTextures"] if x["position"] == 1)
        bad_blobs[rate["file"]] = bytes(len(bad_blobs[rate["file"]]))
        try: feature_replay(fsr_data, bad_blobs, "fsr")
        except ValueError: fixtures += 1
        else: raise ValueError("wrong actual FSR image accepted")
        if args.failure_root:
            s, log = load_case(args.failure_root, "variant-failure")
            for before, after in (("injections=1", "injections=0"), ("partialDestroyed=1", "partialDestroyed=0"),
                                  ("variantNull=1", "variantNull=0"), ("classicValid=1", "classicValid=0")):
                try: failure_native(s, log.replace(before, after, 1))
                except ValueError: fixtures += 1
                else: raise ValueError("failed variant mutation accepted")
    result = {"Actor": "Lead", "NativePositive": records, "ReplayPositive": 4, "RecoveryNative": 3 if args.failure_root else 0, "RejectedFixtures": fixtures, "Pass": True}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
