"""Prepare isolated Creator configs; preflight packages without claiming GPU content.

Use the real project's exported build options. This never edits project settings,
assets, existing Debug resources, native sources or CMake cache.
"""
import argparse
import copy
import hashlib
import json
import re
from pathlib import Path


SCENES = {
    "bloom": ("postprocess/bloom.scene", "Builtin"),
    "compute": ("GFX/compute-demo/compute-demo.scene", "RaytracingWeekend"),
}


def scene_config(project, exported, output, scene):
    relative, pipeline = SCENES[scene]
    options = copy.deepcopy(exported)
    for transient in ("id", "taskId", "logDest", "dest"):
        options.pop(transient, None)
    meta = json.loads((project / "assets/cases" / (relative + ".meta")).read_text(encoding="utf-8-sig"))
    options.update(
        buildPath=str(output.resolve()),
        outputName="windows-vulkan-" + scene,
        taskName="vulkan-content-" + scene,
        customPipeline=True,
        debug=True,
        sourceMaps=True,
        nextStages=[],
        buildStageGroup={"build": []},
        startScene=meta["uuid"],
    )
    modules = list(options.get("includeModules", []))
    if not modules:
        engine = json.loads((project / "settings/v2/packages/engine.json").read_text(encoding="utf-8-sig"))["modules"]
        modules = engine["configs"][engine["globalConfigKey"]]["includeModules"]
    options["includeModules"] = list(dict.fromkeys(modules + ["custom-pipeline", "custom-pipeline-builtin-scripts"]))
    options["macroConfig"] = dict(options.get("macroConfig", {}), CUSTOM_PIPELINE_NAME=pipeline)
    # Keep the caller's participating scenes; add the requested scene if needed.
    scenes = {item["uuid"]: item for item in options.get("scenes", [])}
    scenes[meta["uuid"]] = {"url": "db://assets/cases/" + relative, "uuid": meta["uuid"]}
    options["scenes"] = list(scenes.values())
    return options


def check_package(root, scene):
    root = root.resolve(strict=True)
    required = ["main.js", "application.js", "src/settings.json", "src/import-map.json", "src/cocos-js/cc.js", "assets/main/index.js"]
    for name in required:
        file = root / name
        if not file.is_file() or not file.stat().st_size:
            raise ValueError("missing/empty package file: " + name)
    settings = json.loads((root / "src/settings.json").read_text(encoding="utf-8-sig"))
    if settings.get("rendering", {}).get("customPipeline") is not True:
        raise ValueError("customPipeline must be true; legacy scene entry is not content proof")
    effect_name = settings["rendering"].get("effectSettingsPath")
    if not isinstance(effect_name, str) or not effect_name:
        raise ValueError("missing effect layout data")
    effect = (root / effect_name).resolve(strict=True)
    if not effect.is_relative_to(root) or not effect.is_file() or not effect.stat().st_size:
        raise ValueError("invalid effect layout data")
    relative, pipeline = SCENES[scene]
    if settings.get("engine", {}).get("macros", {}).get("CUSTOM_PIPELINE_NAME") != pipeline:
        raise ValueError("pipeline name mismatch: expected " + pipeline)
    if settings.get("launch", {}).get("launchScene") != "db://assets/cases/" + relative:
        raise ValueError("startup scene mismatch")
    cc = (root / "src/cocos-js/cc.js").read_text(encoding="utf-8-sig")
    if "setCustomPipeline" not in cc:
        raise ValueError("custom-pipeline interface was cropped")
    main = (root / "assets/main/index.js").read_text(encoding="utf-8-sig")
    if scene == "bloom":
        scripts = "\n".join(file.read_text(encoding="utf-8-sig") for file in (root / "assets").glob("*/index.js"))
        if "BuiltinPipelineSettings" not in scripts or "bloomThreshold" not in scripts:
            raise ValueError("Builtin settings or threshold handler missing")
    else:
        imports = re.search(r'System\.register\([\"\x27]chunks:///_virtual/main[\"\x27],\s*\[(.*?)\]', main, re.S)
        if not imports or "raytracingByCompute" not in imports[1] or "RaytracingWeekend" not in main:
            raise ValueError("Compute registration module is not in main prerequisites")
    return {
        "kind": "package-preflight-only",
        "scene": scene,
        "pipeline": pipeline,
        "root": str(root),
        "effectBytes": effect.stat().st_size,
        "settingsSHA256": hashlib.sha256((root / "src/settings.json").read_bytes()).hexdigest(),
        "gpuContentVerified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", choices=SCENES, required=True)
    parser.add_argument("--check-package", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--exported-config", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.check_package:
            if any((args.project, args.exported_config, args.output)):
                raise ValueError("package check and config generation are separate modes")
            result = check_package(args.check_package, args.scene)
        else:
            if not all((args.project, args.exported_config, args.output)):
                raise ValueError("config generation requires project, exported-config and output")
            if args.output.exists():
                raise ValueError("output already exists; do not overwrite previous evidence")
            exported = json.loads(args.exported_config.read_text(encoding="utf-8-sig"))
            result = scene_config(args.project, exported, args.output.parent / "creator-output", args.scene)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("REJECTED:", error)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
