"""Compare visible coloured image segments with projected telemetry in reviewed regions."""
import argparse
import hashlib
from html import escape
import json
import math
import os
from pathlib import Path

from ..artifacts import atomic_text

from ..adapters.legacy_evidence import load_evidence, render_review, validate_review
from ..pixel_alignment import analyze_segment, read_settings, detector_identity, VERSION
from ..images import decode_frame


def publish(evidence_path, regions_path, profile_path, calibration_path, output):
    inputs = {p.resolve() for p in (evidence_path, regions_path, profile_path, calibration_path)}
    if output.suffix.lower() != ".html" or output.resolve() in inputs or output.with_suffix(".json").resolve() in inputs:
        raise ValueError("Use separate HTML/JSON output paths")
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_text(output, "<!doctype html><p>Segment analysis incomplete; previous results are not current.</p>")
    atomic_text(output.with_suffix(".json"), json.dumps({"status": "inconclusive", "reason": "Analysis incomplete"}))
    evidence, evidence_hash = load_evidence(evidence_path)
    regions = json.loads(regions_path.read_text(encoding="utf-8-sig"))
    profile_bytes = profile_path.read_bytes()
    profile = json.loads(profile_bytes)
    settings = read_settings(profile)
    calibration_bytes = calibration_path.read_bytes()
    calibration = json.loads(calibration_bytes)
    if calibration.get("profile_sha256") != hashlib.sha256(profile_bytes).hexdigest() or calibration.get("detector_version") != VERSION or calibration.get("detector_identity") != detector_identity() or calibration.get("status") != "controls_passed":
        raise ValueError("Matching successful instrument controls are required")
    if regions.get("schema_version") != 1 or regions.get("evidence_sha256") != evidence_hash or not regions.get("reviewer"):
        raise ValueError("Reviewed regions must identify their reviewer and exact evidence")
    available = {f["file"]: f for f in evidence["frames"]}
    annotations = regions["frames"]
    if not annotations or len({a["file"] for a in annotations}) != len(annotations):
        raise ValueError("Reviewed regions must be nonempty and unique per frame")
    findings, rows, previews = [], [], []
    for index, annotation in enumerate(annotations):
        frame = available[annotation["file"]]
        if not annotation.get("basis"):
            raise ValueError("Each region requires an identity/visibility review basis")
        region = annotation["region_px"]
        if len(region) != 4 or any(type(v) is not int for v in region):
            raise ValueError("Reviewed regions must contain integer pixel bounds")
        if len(frame["segment"]) != 2 or any(len(p) != 2 or any(not isinstance(v, (int, float)) or not math.isfinite(v) for v in p) for p in frame["segment"]):
            raise ValueError("Expected finite projected segment coordinates")
        measured = analyze_segment(decode_frame(frame, profile["image_size"]), frame["width"], frame["height"], region,
                                   frame["segment"], annotation["visibility"], settings)
        measurements = measured["measurements"]
        rows.append(dict(file=frame["file"], region_px=region, visibility=annotation["visibility"], **measured))
        detail = measured["reason"]
        if "maximum_perpendicular_error_px" in measurements:
            detail += f"; maximum perpendicular error {measurements['maximum_perpendicular_error_px']:.2f} px, span overlap {measurements['span_overlap_fraction']:.3f}"
        findings.append(dict(id=f"segment_alignment_{index+1}", category="Visible segment alignment", assessment=measured["assessment"],
                             basis="pixels_and_telemetry", observation=detail,
                             interpretation="Alignment within this detector's scoped limits only; no body-contact or artistic-quality verdict.",
                             limits="Region identity and visibility are reviewed inputs. Colour/line fitting is material-, resolution- and view-dependent; competing components abstain.",
                             next_action="Inspect a concern against the raw pixels; obtain another view for indeterminate evidence.", evidence=[frame["file"]]))
        # Side-by-side raw pixels and measured/projected lines; no derivative PNGs.
        def preview(overlay):
            lines = ""
            if overlay:
                for segment, color in ((frame["segment"], "#ffd400"), (measurements.get("detected_segment"), "#ff48d7")):
                    if segment:
                        lines += f'<line x1="{segment[0][0]}" y1="{segment[0][1]}" x2="{segment[1][0]}" y2="{segment[1][1]}" stroke="{color}" stroke-width="1.5"/>'
            x0, y0, x1, y1 = region
            return f'<svg style="width:46%;height:260px" viewBox="{x0-8} {y0-8} {x1-x0+16} {y1-y0+16}"><image width="{frame["width"]}" height="{frame["height"]}" href="{escape(frame["image"], quote=True)}"/>{lines}</svg>'
        previews.append('<article><h2>' + escape(frame["file"]) + '</h2><p>Raw pixels / yellow telemetry / magenta pixel fit. ' + escape(measured["assessment"]) + '</p>' + preview(False) + preview(True) + '</article>')
    review = dict(schema_version=1, title="Visible segment alignment analysis", evidence_sha256=evidence_hash,
                  reviewer=dict(kind="automated_detector", identity="Colored segment alignment", version=VERSION,
                                calibration_reference=str(calibration_path) + " SHA256 " + hashlib.sha256(calibration_bytes).hexdigest()),
                  summary="Image-derived segment alignment in explicitly reviewed regions. This does not establish physical surface contact.",
                  reviewed_frames=[a["file"] for a in annotations], findings=findings)
    summary = validate_review(review, evidence, evidence_hash)
    html = render_review(review, evidence, summary, Path(os.path.relpath(evidence_path, output.parent)).as_posix())
    atomic_text(output, html + '<h1>Pixel measurements</h1>' + ''.join(previews))
    result = dict(summary, review=review, measurements=rows, settings=profile_path.as_posix(),
                  evidence_sha256=evidence_hash, profile_sha256=hashlib.sha256(profile_bytes).hexdigest(),
                  regions_sha256=hashlib.sha256(regions_path.read_bytes()).hexdigest(),
                  calibration_sha256=hashlib.sha256(calibration_bytes).hexdigest(), detector_version=VERSION, detector_identity=detector_identity())
    atomic_text(output.with_suffix(".json"), json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("evidence", "regions", "profile", "calibration", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = publish(args.evidence, args.regions, args.profile, args.calibration, args.output)
    except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
        parser.exit(1, f"Segment analysis unavailable: {error}\n")
    print(f"{result['status']}: {args.output}; {result['assessments']}")


if __name__ == "__main__":
    main()
