"""Offline judge of S1-S8 running on the actual test project's existing device."""
import argparse
import copy
import json
import re
from pathlib import Path


def require(value, what):
    if not value:
        raise ValueError(what)


def unique(lines, prefix, expression):
    found = [x for x in lines if prefix.lower() in x.lower()]
    require(len(found) == 1, "unique " + prefix)
    match = re.fullmatch(expression, found[0])
    require(match is not None, "exact " + prefix)
    return match


def judge(summary, log, modern, negative=False):
    require(summary["exitCode"] == int(negative), "normal native exit")
    require(summary["errorLogMessages"] == (2 if negative else 0), "actual errors")
    require(not any(not x["firstChance"] for x in summary["exceptions"]), "unhandled exception")
    require(not re.search(r"VUID-|HAZARD|INCOMPLETE|SKIPPED|uncaught exception", log, re.I), "validation/scenario error")
    lines = [re.sub(r"^\d{2}:\d{2}:\d{2} \[(?:INFO|ERROR)\]: ", "", x) for x in log.splitlines() if x.strip()]
    require(sum("project-wsi" in x.lower() for x in lines) == (5 if negative else 4), "reserved WSI family count")
    unique(lines, "project-wsi mode", r"project-wsi mode: " + ("skip-surface" if negative else "content") + r" enabled=1")
    unique(lines, "validation + best practices + sync", r"validation \+ best practices \+ sync: ENABLED")
    unique(lines, "S1 stable frames", r"S1 stable frames: presented=120/120")
    unique(lines, "S2 resizes", r"S2 resizes: presented=30/30, final generation=31")
    unique(lines, "S3 zero-size", r"S3 zero-size suspend/restore done")
    unique(lines, "S4 surface", r"S4 surface destroy/recreate done, generation=33")
    unique(lines, "S5 two-window", r"S5 two-window: B presented=10/10 while A suspended")
    unique(lines, "S6 all-failed", r"S6 all-failed acquire: empty submit \+ empty present done")
    unique(lines, "S7 result injection", r"S7 result injection: completed")
    unique(lines, "S8 offscreen", r"S8 offscreen-between-frames: completed \(delta=0\)")
    complete = unique(lines, "completion backend", r"completion backend=(timeline|fence) submits=([1-9][0-9]*) submission-fences=(0|[1-9][0-9]*)")
    require(complete[1] == ("timeline" if modern else "fence"), "actual completion backend")
    require(int(complete[3]) == (0 if modern else int(complete[2])), "actual fence submission ownership")
    encoder = unique(lines, "submit encoder", r"submit encoder=(submit2|legacy) observed-legacy=(0|[1-9][0-9]*) observed-submit2=(0|[1-9][0-9]*)")
    require(encoder[1] == ("submit2" if modern else "legacy"), "actual submit encoder")
    require(int(encoder[2 if modern else 3]) == 0 and int(encoder[3 if modern else 2]) > 0, "actual submit dispatches")
    barriers = unique(lines, "barrier encoder", r"barrier encoder=(sync2|legacy) observed-legacy=(0|[1-9][0-9]*) observed-sync2=(0|[1-9][0-9]*)")
    require(barriers[1] == ("sync2" if modern else "legacy"), "actual barrier encoder")
    require(int(barriers[2 if modern else 3]) == 0 and int(barriers[3 if modern else 2]) > 0, "actual barrier dispatches")
    unique(lines, "WSI smoke", rf"WSI smoke: validation errors=0 \(pre-teardown 0\), failures={int(negative)}")
    dispatch = unique(lines, "project-wsi dispatch", rf"project-wsi dispatch: surfaces=([1-9][0-9]*) swapchains=([1-9][0-9]*) presents=([1-9][0-9]*) pairedPresents=([1-9][0-9]*) diagnostic={int(negative)}")
    surfaces, chains, presents, pairs = map(int, dispatch.groups())
    require(surfaces >= 4 and chains >= 32 and presents >= 120 and pairs > 0, "real surface/recreation/pair API counts")
    retirement = unique(lines, "project-wsi retirement", r"project-wsi retirement: created=([1-9][0-9]*) destroyed=([1-9][0-9]*) devices=1 liveErrors=0 postErrors=0")
    require(int(retirement[1]) == int(retirement[2]) == surfaces+chains, "all tracked WSI handles destroyed once")
    unique(lines, "project-wsi completed", rf"project-wsi completed: failures=0 diagnostic={int(negative)} platformResult=0 outcome={'FAILED' if negative else 'CLEAN'}")
    unique(lines, "Total device memory leaked", r"Total device memory leaked: 0 bytes\.")
    failures = [x for x in lines if "project-wsi failure" in x.lower()]
    require(failures == (["project-wsi failure: S4: renderable after surface recreate"] if negative else []), "real missing-recreation negative only")
    return True


def load(root, name):
    d = root / name
    return json.loads((d/"runtime-summary.json").read_text()), (d/"debug-output.log").read_text(encoding="utf-8")


def main():
    p = argparse.ArgumentParser(__doc__); p.add_argument("root", type=Path); p.add_argument("--self-test", action="store_true"); args = p.parse_args()
    for name, modern, negative in (("modern", True, False), ("legacy", False, False), ("skip-surface", True, True)):
        judge(*load(args.root, name), modern, negative)
    rejected = 0
    if args.self_test:
        s, log = load(args.root, "modern")
        for before, after in (("presented=120/120", "presented=119/120"), ("pairedPresents=14", "pairedPresents=0"),
                              ("surfaces=4", "surfaces=0"), ("validation errors=0", "validation errors=1"),
                              ("liveErrors=0", "liveErrors=1"), ("postErrors=0", "postErrors=1"),
                              ("destroyed=43", "destroyed=42"), ("observed-legacy=0", "observed-legacy=1")):
            require(before in log, "fixture actually changes real evidence")
            try: judge(s, log.replace(before, after, 1), True)
            except ValueError: rejected += 1
            else: raise ValueError("WSI mutation accepted")
        for extra in ("project-wsi completed malformed", "PROJECT-WSI dispatchbroken", "VUID-invalid", "SYNC-HAZARD-WAW", "Total device memory leaked: 1 bytes."):
            try: judge(s, log+"\n"+extra, True)
            except ValueError: rejected += 1
            else: raise ValueError("WSI extra bad evidence accepted")
        bad = copy.deepcopy(s); bad["exitCode"] = 0xC0000005
        try: judge(bad, log, True)
        except ValueError: rejected += 1
        else: raise ValueError("native crash accepted")
    print(json.dumps({"Actor": "Lead", "NativeCases": 3, "RejectedFixtures": rejected, "Pass": True}, indent=2))


if __name__ == "__main__":
    main()
