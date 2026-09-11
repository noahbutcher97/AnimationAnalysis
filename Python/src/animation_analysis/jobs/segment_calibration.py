"""Replay labelled image cohorts and controlled telemetry defects for a segment profile."""
import argparse
import hashlib
import json
import math
from pathlib import Path

from ..artifacts import atomic_text

from ..adapters.legacy_evidence import load_evidence
from ..images import decode_frame
from ..pixel_alignment import analyze_segment, read_settings, detector_identity, VERSION


def calibrate(manifest_path, profile_path, output):
    if output.suffix.lower() != ".json" or output.resolve() in (manifest_path.resolve(), profile_path.resolve()):
        raise ValueError("Calibration output cannot overwrite inputs")
    manifest_bytes, profile_bytes = manifest_path.read_bytes(), profile_path.read_bytes()
    manifest, profile = json.loads(manifest_bytes), json.loads(profile_bytes)
    # Resolve all manifest inputs before any output mutation.
    for dataset in manifest["datasets"]:
        if output.resolve() in ((manifest_path.parent / dataset[key]).resolve() for key in ("evidence", "regions")):
            raise ValueError("Calibration output cannot overwrite dataset inputs")
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_text(output, json.dumps({"status": "inconclusive", "reason": "Calibration incomplete"}))
    settings = read_settings(profile)
    if manifest.get("schema_version") != 1 or not manifest.get("basis"):
        raise ValueError("Calibration manifest requires schema and review basis")
    rows, input_hashes, identities = [], {}, set()
    labelled_consistent = {"calibration": 0, "validation": 0}
    for dataset in manifest["datasets"]:
        cohort = dataset["cohort"]
        if cohort not in labelled_consistent:
            raise ValueError("Dataset cohort must be calibration or validation")
        evidence_path = (manifest_path.parent / dataset["evidence"]).resolve()
        region_path = (manifest_path.parent / dataset["regions"]).resolve()
        if output.resolve() in (evidence_path, region_path):
            raise ValueError("Calibration output cannot overwrite dataset inputs")
        evidence, evidence_hash = load_evidence(evidence_path)
        regions_bytes = region_path.read_bytes()
        regions = json.loads(regions_bytes)
        if regions.get("schema_version") != 1 or regions.get("evidence_sha256") != evidence_hash or not regions.get("reviewer"):
            raise ValueError("Calibration regions require reviewed exact evidence")
        input_hashes[str(evidence_path)] = evidence_hash
        input_hashes[str(region_path)] = hashlib.sha256(regions_bytes).hexdigest()
        frames = {f["file"]: f for f in evidence["frames"]}
        for annotation in regions["frames"]:
            frame = frames[annotation["file"]]
            identity = frame["image_sha256"]
            if identity in identities:
                raise ValueError("A frame cannot be counted twice or occur in both cohorts")
            identities.add(identity)
            label = annotation["expected_assessment"]
            if label not in ("consistent", "indeterminate") or not annotation.get("basis"):
                raise ValueError("Baseline requires a reviewed consistent or indeterminate label and basis")
            rgb = decode_frame(frame, profile["image_size"])
            def record(control, segment, visibility, expected):
                actual = analyze_segment(rgb, frame["width"], frame["height"], annotation["region_px"], segment, visibility, settings)
                rows.append(dict(cohort=cohort, evidence=str(evidence_path), file=frame["file"], control=control,
                                 expected=expected, actual=actual["assessment"], expected_segment=segment, detail=actual))
            record("unaltered", frame["segment"], annotation["visibility"], label)
            if label == "consistent":
                labelled_consistent[cohort] += 1
                start, end = frame["segment"]
                dx, dy = end[0]-start[0], end[1]-start[1]
                length = math.hypot(dx, dy)
                if length <= 0:
                    raise ValueError("Cannot perturb a degenerate projected segment")
                for offset in (-16, -8, 8, 16):
                    shifted = [[x-offset*dy/length, y+offset*dx/length] for x, y in frame["segment"]]
                    record(f"perpendicular_{offset}px", shifted, "visible", "concern")
                shifted = [[x+100*dx/length, y+100*dy/length] for x, y in frame["segment"]]
                record("longitudinal_100px", shifted, "visible", "concern")
                # Explicitly label a visibly different pose; a static pose at another
                # time need not be detectable from image alignment.
                stale = frames[annotation["stale_reference_frame"]]
                if stale["file"] == frame["file"]:
                    raise ValueError("Stale reference must be a different reviewed pose")
                record("stale_pose_segment", stale["segment"], "visible", "concern")
            for visibility in ("occluded", "unknown"):
                record(visibility, frame["segment"], visibility, "indeterminate")
    failures = sum(r["actual"] != r["expected"] for r in rows)
    enough = all(labelled_consistent.values())
    report = dict(schema_version=1, detector_version=VERSION, detector_identity=detector_identity(), status="controls_passed" if enough and rows and not failures else "controls_failed",
                  profile_sha256=hashlib.sha256(profile_bytes).hexdigest(), manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                  basis=manifest["basis"], cases=len(rows), mismatches=failures, labelled_consistent=labelled_consistent,
                  input_hashes=input_hashes, results=rows,
                  limits="Fixed 8/16 px transverse, 100 px longitudinal and explicitly labelled stale-pose controls. Small reviewed corpus; not universal accuracy, contact proof or artistic approval.")
    atomic_text(output, json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "profile", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = calibrate(args.manifest, args.profile, args.output)
    except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
        parser.exit(1, f"Calibration unavailable: {error}\n")
    print(f"{result['status']}: {result['cases']} cases, {result['mismatches']} mismatches; {args.output}")
    return 0 if result["status"] == "controls_passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
