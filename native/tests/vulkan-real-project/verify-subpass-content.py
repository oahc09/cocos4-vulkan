"""Offline judge for the real-project Input Attachment / Multi Subpass evidence.

Requires native shutdown records and RenderDoc replay pixels; does not build or
launch the project. --self-test mutates the same actual positive evidence.
"""
import argparse
import copy
import json
import re
import struct
from pathlib import Path


def require(condition, label):
    if not condition:
        raise ValueError(label)


def exact(lines, prefix, pattern):
    found = [s for s in lines if prefix.lower() in s.lower()]
    require(len(found) == 1, "unique " + prefix)
    match = re.fullmatch(pattern, found[0])
    require(match is not None, "exact " + prefix)
    return match


def native(summary, log, negative=False):
    require(summary["exitCode"] == (1 if negative else 0), "native exit")
    require(not any(not e["firstChance"] for e in summary["exceptions"]), "unhandled exception")
    # CC_LOG_ERROR also emits a separate source-location line in this Debug build.
    require(summary["errorLogMessages"] == (6 if negative else 0), "actual error count")
    lines = [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR)\]: ", "", s) for s in log.splitlines()]
    require(not re.search(r"VUID-|SYNC-HAZARD|uncaught exception|INCOMPLETE|UNSUPPORTED", log, re.I), "runtime diagnostic error")
    require(sum("completed" in s.lower() for s in lines) == 1, "unique footer")
    require(sum("project-subpass" in s.lower() for s in lines) == (10 if negative else 7), "reserved family count")
    exact(lines, "project-subpass mode", r"project-subpass mode: " + ("skip-producer" if negative else "content") + r" enabled=1")
    exact(lines, "project-subpass validation", r"project-subpass validation: ENABLED")
    phases = [s for s in lines if "project-subpass phase" in s.lower()]
    require(len(phases) == 3, "three phases")
    for index, (w, h) in enumerate([(32, 16), (24, 12), (32, 16)]):
        pattern = (rf"project-subpass phase: index={index} width={w} height={h} rpContract=1 pipelines=2 "
                   rf"begins=1 next=1 ends=1 dynamic=0 producer={0 if negative else 1} consumer=1 "
                   rf"copies=1,1 submits=1 chain=1 checkedBytes={w*h*4} sourceMismatch=([0-9]+) outputMismatch=([0-9]+)")
        match = re.fullmatch(pattern, phases[index])
        require(match is not None, "phase fields")
        mismatches = tuple(int(x) for x in match.groups())
        require(all(x > 0 for x in mismatches) if negative else mismatches == (0, 0), "whole-image golden")
    exact(lines, "project-subpass retirement", r"project-subpass retirement: created=33 destroyed=33 devices=1 bufferFacades=6 bufferDrops=6 imageFacades=6 imageDrops=6")
    exact(lines, "project-subpass completed", rf"project-subpass completed: failures={3 if negative else 0} liveErrors=0 postErrors=0 outcome={'FAILED' if negative else 'CLEAN'}")
    leaks = [s for s in lines if "total device memory leaked" in s.lower()]
    require(len(leaks) == 1 and re.fullmatch(r"Total device memory leaked: 0 bytes\.", leaks[0]), "allocator leak")
    if negative:
        require([s for s in lines if "project-subpass failure" in s.lower()] == ["project-subpass failure: producer/consumer full golden"] * 3, "negative reason only")
    return True


def golden(width, height, consumer):
    left, right = ((0, 255, 0, 255), (0, 0, 255, 255)) if consumer else ((255, 0, 0, 255), (0, 255, 0, 255))
    return (bytes(left) * (width // 2) + bytes(right) * (width // 2)) * height


def replay(data, blobs):
    require(data.get("Complete") is True, "complete replay")
    require(not any(re.search(r"VUID-|HAZARD|FAIL:", m["description"]) for m in data["messages"]), "replay diagnostic")
    for name, count in {"vkCmdDraw": 6, "vkCmdBeginRenderPass": 3, "vkCmdNextSubpass": 3,
                        "vkCmdEndRenderPass": 3, "vkCmdCopyImageToBuffer": 6,
                        "vkCmdBeginRendering": 0, "vkCmdBeginRenderingKHR": 0}.items():
        require(data["apiCounts"][name] == count, "actual API " + name)
    require(len(data["actions"]) == 6, "six actual draws")
    require(data["apiCounts"]["vkQueueSubmit2"] == 3 and data["apiCounts"]["vkQueueSubmit"] == 0, "captured modern submit path")
    for index, (w, h) in enumerate([(32, 16), (24, 12), (32, 16)]):
        producer, consumer = data["actions"][2*index:2*index+2]
        for subpass, action in enumerate([producer, consumer]):
            require(not action["dynamic"] and action["renderpass"]["subpass"] == subpass, "Classic subpass identity")
            require(len(action["outputs"]) == 1, "one output")
            output = action["outputs"][0]
            require((output["width"], output["height"], output["format"]) == (w, h, "R8G8B8A8_UNORM"), "output extent/format")
            require(blobs[output["file"]] == golden(w, h, subpass == 1), "replay whole-image golden")
        require(producer["pipeline"] != consumer["pipeline"], "distinct subpass pipelines")
        require(producer["renderpass"]["resourceId"] == consumer["renderpass"]["resourceId"], "same render pass")
        require(producer["renderpass"]["colorAttachments"] == "[0]" and consumer["renderpass"]["colorAttachments"] == "[1]" and consumer["renderpass"]["inputAttachments"] == "[0]", "actual attachment indices")
        require(len(consumer["inputs"]) == 1 and consumer["inputs"][0]["id"] == producer["outputs"][0]["id"], "actual descriptor producer link")
        require(blobs[consumer["inputs"][0]["file"]] == golden(w, h, False), "actual input pixels")
        raw = blobs[consumer["shaderFile"]]
        require(len(raw) % 4 == 0 and len(raw) >= 20, "actual SPIR-V bytes")
        words = struct.unpack("<%dI" % (len(raw)//4), raw)
        require(words[0] == 0x07230203, "SPIR-V magic")
        instructions = []
        cursor = 5
        while cursor < len(words):
            size, opcode = words[cursor] >> 16, words[cursor] & 0xffff
            require(size > 0 and cursor + size <= len(words), "SPIR-V instruction")
            instructions.append((opcode, words[cursor+1:cursor+size])); cursor += size
        require(any(op == 25 and len(args) >= 3 and args[2] == 6 for op, args in instructions), "SubpassData shader image")
        require(any(op == 98 for op, _ in instructions), "actual input image read")
    return True


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("evidence_root", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(); root = args.evidence_root
    records = {}
    for name in ["modern", "legacy", "skip-producer"]:
        d = root / (name + "-green")
        summary = json.loads((d / "runtime-summary.json").read_text())
        log = (d / "debug-output.log").read_text()
        native(summary, log, name == "skip-producer"); records[name] = (summary, log)
        expected = "timeline=0 sync2=0 submit2=0 dynamicRendering=0 classicOverride=1" if name == "legacy" else "timeline=1 sync2=1 submit2=1 dynamicRendering=1 classicOverride=0"
        exact(log.splitlines(), "Vulkan paths", r"\d{2}:\d{2}:\d{2} \[INFO\]: Vulkan paths: " + expected + r" maintenance4=0")
    for name in ["default-off", "bad-mode"]:
        d = root / (name + "-green"); summary = json.loads((d / "runtime-summary.json").read_text()); log = (d / "debug-output.log").read_text()
        require(summary["exitCode"] == (0 if name == "default-off" else 1), "startup/off exit")
        require(summary["exeSHA256"] == records["modern"][0]["exeSHA256"], "startup/off same executable")
        if name == "default-off":
            require(summary["errorLogMessages"] == 0, "default-off errors")
            require(len([s for s in log.splitlines() if "project-subpass" in s]) == 1 and "project-subpass mode: off enabled=0" in log, "inert default")
            require("Total device memory leaked: 0 bytes." in log, "default-off shutdown")
        else:
            require("project-subpass failure: known mode" in log and "Vulkan API:" not in log and "project-subpass phase:" not in log, "unknown mode rejected before device")
    require(records["modern"][0]["exeSHA256"] == records["legacy"][0]["exeSHA256"] == records["skip-producer"][0]["exeSHA256"], "same native executable")
    d = root / "renderdoc"; data = json.loads((d / "inspect.json").read_text())
    state = json.loads((d / "capture-state.json").read_text())
    require(state["state"] == "CAPTURED" and state["exeSHA256"] == records["modern"][0]["exeSHA256"], "same captured executable")
    blobs = {p.name: p.read_bytes() for p in d.glob("*.bin")}
    blobs.update({p.name: p.read_bytes() for p in d.glob("*.spv")})
    replay(data, blobs)
    tests = 0
    if args.self_test:
        summary, log = records["modern"]
        mutations = [("native exit", {**summary, "exitCode": 1}, log),
                     ("missing phase", summary, "\n".join(s for s in log.splitlines() if "index=1 width=" not in s)),
                     ("zero submits", summary, log.replace("submits=1", "submits=0", 1)),
                     ("nonzero golden", summary, log.replace("sourceMismatch=0", "sourceMismatch=1", 1)),
                     ("duplicate footer", summary, log + "\nfoo completed: failures=1"),
                     ("bad retirement", summary, log.replace("destroyed=33", "destroyed=32")),
                     ("bad validation", summary, log.replace("validation: ENABLED", "validation: DISABLED"))]
        for label, s, text in mutations:
            try: native(s, text)
            except ValueError: tests += 1
            else: raise ValueError("mutation accepted: " + label)
        for field, value in [("Complete", False), ("actions", data["actions"][:-1])]:
            changed = copy.deepcopy(data); changed[field] = value
            try: replay(changed, blobs)
            except ValueError: tests += 1
            else: raise ValueError("replay mutation accepted: " + field)
        changed = dict(blobs); output = data["actions"][1]["outputs"][0]["file"]
        changed[output] = b"\0" * len(changed[output])
        try: replay(data, changed)
        except ValueError: tests += 1
        else: raise ValueError("zero output accepted")
        for label in ["input-link", "subpass", "next-subpass"]:
            changed = copy.deepcopy(data)
            if label == "input-link": changed["actions"][1]["inputs"][0]["id"] = "ResourceId::0"
            elif label == "subpass": changed["actions"][1]["renderpass"]["subpass"] = 0
            else: changed["apiCounts"]["vkCmdNextSubpass"] = 0
            try: replay(changed, blobs)
            except ValueError: tests += 1
            else: raise ValueError("replay mutation accepted: " + label)
        red = root / "modern"
        try: native(json.loads((red / "runtime-summary.json").read_text()), (red / "debug-output.log").read_text())
        except ValueError: tests += 1
        else: raise ValueError("actual pre-fix WAW accepted")
    print(json.dumps({"Pass": True, "nativeCases": 5, "draws": 6, "mutationTests": tests}))


if __name__ == "__main__":
    main()
