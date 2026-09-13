"""Qualify an explicit neutral rigid-assembly replay with installed public APIs."""
import argparse
from dataclasses import asdict, replace
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

from animation_analysis import (
    ClockStamp,
    MeshAnalysisLimits,
    MeshPairSample,
    MeshRecordLimits,
    MeshRegion,
    MeshRequirement,
    MeshSelection,
    PoseKey,
    measure_mesh_pair,
    read_mesh_observation,
    summarize_mesh_interval,
)
from animation_analysis.temporal import TimeInterval


JSON_LIMIT = 65536
RECORD_CAPS = {
    "max_vertices": 64,
    "max_indices": 128,
    "max_sections": 4,
    "max_payload_bytes": 8192,
    "max_metadata_bytes": 65536,
    "max_record_bytes": 73728,
}
ANALYSIS_CAPS = {
    "max_triangles": 12,
    "max_pair_tests": 256,
    "max_node_visits": 1024,
    "max_coordinate_bits": 256,
}
CRITERIA_FIELDS = {
    "format", "schema_version", "units", "coordinate_system", "vector_convention",
    "required_features", "known_features", "allowed_exclusions", "allowed_producers",
    "record_limits", "analysis_limits", "max_samples", "max_gap_seconds", "tolerance",
    "expected_samples",
}
ROLE_FIELDS = {
    "component_id", "component_generation", "configuration_id", "subject_id",
    "stream_id", "region_id", "topology_id", "triangle_ids",
}
SAMPLE_FIELDS = {
    "sample_id", "fixed_bundle", "moving_bundle", "acquired_seconds", "frame_id",
    "fixed_revision", "moving_revision",
}


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def _read_bytes(path, maximum=JSON_LIMIT):
    path = Path(path)
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise ValueError(f"Input must be a regular file: {path}")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if not stat.S_ISREG(opened.st_mode) or opened.st_size > maximum:
            raise ValueError(f"Input exceeds {maximum} bytes: {path}")
        data = stream.read(opened.st_size + 1)
        if len(data) != opened.st_size:
            raise ValueError(f"Input changed while reading: {path}")
    return data


def _read_json(path):
    data = _read_bytes(path)

    def nonfinite(value):
        raise ValueError(f"Non-finite JSON number: {value}")

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_pairs,
                           parse_constant=nonfinite)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Invalid JSON input: {path}") from error
    return value, hashlib.sha256(data).hexdigest()


def _mapping(value, fields, label):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError(f"{label} has missing or unknown fields")
    return value


def _text(value, label):
    if not isinstance(value, str) or not value.strip() or len(value) > 256:
        raise ValueError(f"Expected bounded {label} text")
    return value


def _integer(value, label, minimum=0, maximum=2**53 - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"Expected bounded integer {label}")
    return value


def _number(value, label, minimum=None):
    try:
        finite = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite:
        raise ValueError(f"Expected finite number {label}")
    if minimum is not None and value < minimum:
        raise ValueError(f"Expected {label} >= {minimum}")
    return value


def _names(value, label, *, empty=False):
    if not isinstance(value, list) or len(value) > 64 or (not value and not empty):
        raise ValueError(f"Expected bounded {label} list")
    result = tuple(_text(item, label) for item in value)
    if len(set(result)) != len(result):
        raise ValueError(f"Duplicate {label}")
    return result


def _sha256(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"Expected SHA-256 {label}")
    return value


def _limits(value, caps, label):
    _mapping(value, caps, label)
    result = {}
    for name, cap in caps.items():
        minimum = 8 if name == "max_coordinate_bits" else 1
        result[name] = _integer(value[name], f"{label}.{name}", minimum, cap)
    return result


def _validate_criteria(value):
    value = _mapping(value, CRITERIA_FIELDS, "criteria")
    if (value["format"] != "neutral_assembly_criteria"
            or type(value["schema_version"]) is not int or value["schema_version"] != 1):
        raise ValueError("Unsupported neutral assembly criteria schema")
    for name in ("units", "coordinate_system", "vector_convention"):
        _text(value[name], name)
    if value["vector_convention"] not in ("row", "column"):
        raise ValueError("Unsupported vector convention")
    required = _names(value["required_features"], "required feature")
    known = _names(value["known_features"], "known feature")
    exclusions = _names(value["allowed_exclusions"], "allowed exclusion", empty=True)
    producers = _names(value["allowed_producers"], "allowed producer")
    if not set(required + exclusions) <= set(known) or set(required) & set(exclusions):
        raise ValueError("Feature requirements are inconsistent")
    record = _limits(value["record_limits"], RECORD_CAPS, "record limits")
    analysis = _limits(value["analysis_limits"], ANALYSIS_CAPS, "analysis limits")
    max_samples = _integer(value["max_samples"], "max samples", 2, 8)
    gap = _number(value["max_gap_seconds"], "maximum gap", 0)
    if gap == 0:
        raise ValueError("Maximum gap must be positive")
    tolerance = _number(value["tolerance"], "tolerance", 0)
    expected = value["expected_samples"]
    if not isinstance(expected, list) or not 2 <= len(expected) <= max_samples:
        raise ValueError("Expected samples must be an explicit bounded list")
    sample_ids = set()
    for row in expected:
        _mapping(row, {"sample_id", "distance", "intersection"}, "expected sample")
        sample_id = _text(row["sample_id"], "expected sample identity")
        if sample_id in sample_ids:
            raise ValueError("Duplicate expected sample identity")
        sample_ids.add(sample_id)
        _number(row["distance"], "expected distance", 0)
        if type(row["intersection"]) is not bool:
            raise ValueError("Expected intersection must be boolean")
    return dict(value, required_features=required, known_features=known,
                allowed_exclusions=exclusions, allowed_producers=producers,
                record_limits=record, analysis_limits=analysis)


def _validate_manifest(value, criteria):
    _mapping(value, {"format", "schema_version", "roles", "samples", "controls"},
             "assembly manifest")
    if (value["format"] != "neutral_assembly_capture"
            or type(value["schema_version"]) is not int or value["schema_version"] != 1):
        raise ValueError("Unsupported neutral assembly capture schema")
    roles = _mapping(value["roles"], {"fixed", "moving"}, "roles")
    for role_name, role in roles.items():
        _mapping(role, ROLE_FIELDS, f"{role_name} role")
        for name in ("component_id", "configuration_id", "subject_id", "stream_id", "region_id"):
            _text(role[name], f"{role_name}.{name}")
        _integer(role["component_generation"], f"{role_name}.component_generation")
        _sha256(role["topology_id"], f"{role_name}.topology_id")
        ids = role["triangle_ids"]
        if not isinstance(ids, list) or not 1 <= len(ids) <= criteria["analysis_limits"]["max_triangles"]:
            raise ValueError(f"Expected bounded {role_name} triangle declarations")
        if any(type(index) is not int or index < 0 for index in ids) or len(set(ids)) != len(ids):
            raise ValueError(f"Invalid {role_name} triangle declarations")
    samples = value["samples"]
    if not isinstance(samples, list) or not 2 <= len(samples) <= criteria["max_samples"]:
        raise ValueError("Samples must be an explicit bounded list")
    names, bundles = set(), set()
    for row in samples:
        _mapping(row, SAMPLE_FIELDS, "sample declaration")
        sample_id = _text(row["sample_id"], "sample identity")
        if sample_id in names:
            raise ValueError("Duplicate sample identity")
        names.add(sample_id)
        for role_name in ("fixed", "moving"):
            bundle = _text(row[f"{role_name}_bundle"], "bundle name")
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", bundle) or bundle in bundles:
                raise ValueError("Unsafe or duplicate bundle declaration")
            bundles.add(bundle)
        _number(row["acquired_seconds"], "acquisition time")
        for name in ("frame_id", "fixed_revision", "moving_revision"):
            _integer(row[name], name)
    controls = _mapping(value["controls"], {"checks_passed", "peak_reserved_bytes"},
                        "capture controls")
    if type(controls["checks_passed"]) is not bool:
        raise ValueError("Capture checks flag must be boolean")
    _integer(controls["peak_reserved_bytes"], "peak reserved bytes")
    return value


def _completion_mapping(completion):
    request = completion.request
    return {
        "request_id": request.request_id,
        "status": completion.status,
        "reason": completion.reason,
        "acquired": asdict(request.acquired) if request.acquired else None,
        "completed": asdict(completion.completed),
    }


def _selection(observation, role_name, role, sample, criteria, analysis_limits):
    region = MeshRegion(role["region_id"], role["topology_id"],
                        tuple(role["triangle_ids"]), analysis_limits)
    pose = PoseKey(role["subject_id"], role["stream_id"], sample["frame_id"],
                   sample[f"{role_name}_revision"])
    requirement = MeshRequirement(
        role["component_id"], role["component_generation"], role["topology_id"],
        role["configuration_id"], pose, criteria["units"], criteria["coordinate_system"],
        criteria["required_features"], criteria["known_features"],
        criteria["allowed_exclusions"], criteria["allowed_producers"])
    return MeshSelection(observation, region, requirement)


def _control_mapping(value):
    mapping = value.to_mapping()
    return {"kind": "in_memory_control", "status": mapping["status"],
            "reasons": mapping["reasons"], "result": mapping}


def _negative_controls(pairs, results, interval, criteria, limits):
    labels = ("wrong_topology", "wrong_pose", "wrong_clock", "required_excluded_cloth",
              "exhausted_search", "missing_endpoint", "excessive_gap")
    if not pairs or not results:
        return {label: {"kind": "in_memory_control", "status": "insufficient",
                        "reasons": ["primary_measurement_unavailable"]} for label in labels}
    pair = pairs[0]
    controls = {}
    wrong = replace(pair.first.requirement, topology_id="0" * 64)
    controls["wrong_topology"] = _control_mapping(measure_mesh_pair(
        replace(pair, first=replace(pair.first, requirement=wrong)),
        tolerance=criteria["tolerance"], limits=limits))
    pose = pair.first.requirement.pose
    wrong = replace(pair.first.requirement, pose=replace(pose, revision=pose.revision + 1))
    controls["wrong_pose"] = _control_mapping(measure_mesh_pair(
        replace(pair, first=replace(pair.first, requirement=wrong)),
        tolerance=criteria["tolerance"], limits=limits))
    controls["wrong_clock"] = _control_mapping(measure_mesh_pair(
        replace(pair, acquired=ClockStamp("in-memory-control", pair.acquired.seconds)),
        tolerance=criteria["tolerance"], limits=limits))
    requirement = pair.first.requirement
    required = tuple(requirement.required_features) + (("cloth",)
        if "cloth" not in requirement.required_features else ())
    excluded = tuple(name for name in requirement.allowed_exclusions if name != "cloth")
    wrong = replace(requirement, required_features=required, allowed_exclusions=excluded)
    controls["required_excluded_cloth"] = _control_mapping(measure_mesh_pair(
        replace(pair, first=replace(pair.first, requirement=wrong)),
        tolerance=criteria["tolerance"], limits=limits))
    exhausted_limits = MeshAnalysisLimits(limits.max_triangles, 1, 1,
                                          limits.max_coordinate_bits)
    controls["exhausted_search"] = _control_mapping(measure_mesh_pair(
        pair, tolerance=criteria["tolerance"], limits=exhausted_limits))
    missing = summarize_mesh_interval(results[1:], interval,
        max_gap_seconds=criteria["max_gap_seconds"], max_samples=criteria["max_samples"])
    controls["missing_endpoint"] = {
        "kind": "in_memory_control", "status": missing.status,
        "reasons": list(missing.reasons), "result": missing.to_mapping()}
    positive_gaps = [right.acquired.seconds - left.acquired.seconds
                     for left, right in zip(results, results[1:])
                     if right.acquired.domain == left.acquired.domain
                     and right.acquired.seconds > left.acquired.seconds]
    strict_gap = min(positive_gaps) / 2 if positive_gaps else criteria["max_gap_seconds"] / 2
    excessive = summarize_mesh_interval(results, interval, max_gap_seconds=strict_gap,
                                         max_samples=criteria["max_samples"])
    controls["excessive_gap"] = {
        "kind": "in_memory_control", "status": excessive.status,
        "reasons": list(excessive.reasons), "result": excessive.to_mapping()}
    return controls


def evaluate_run(observations, criteria):
    """Return a detached JSON report for one explicit assembly run and criteria file."""
    root = Path(observations)
    criteria_value, criteria_hash = _read_json(criteria)
    criteria_value = _validate_criteria(criteria_value)
    manifest, manifest_hash = _read_json(root / "assembly.json")
    manifest = _validate_manifest(manifest, criteria_value)
    record_limits = MeshRecordLimits(**criteria_value["record_limits"])
    analysis_limits = MeshAnalysisLimits(**criteria_value["analysis_limits"])
    source_hashes = {"assembly.json": manifest_hash, "criteria": criteria_hash}
    read_errors, completions, results, pairs = [], [], [], []
    roles = manifest["roles"]

    for declared in manifest["samples"]:
        sample = dict(declared)
        loaded, completion_row = {}, {"sample_id": sample["sample_id"]}
        for role_name in ("fixed", "moving"):
            bundle = sample[f"{role_name}_bundle"]
            try:
                completion = read_mesh_observation(root, bundle, limits=record_limits)
                completion_row[role_name] = _completion_mapping(completion)
                if completion.status != "completed" or completion.observation is None:
                    raise ValueError(f"bundle completed with status {completion.status}")
                if completion.request.request_id != sample["sample_id"]:
                    raise ValueError("request identity differs from sample declaration")
                if completion.observation.vector_convention != criteria_value["vector_convention"]:
                    raise ValueError("vector convention differs from criteria")
                loaded[role_name] = completion.observation
                marker = _read_bytes(root / bundle / "complete.json")
                source_hashes[f"{bundle}/complete.json"] = hashlib.sha256(marker).hexdigest()
            except (OSError, ValueError, TypeError) as error:
                read_errors.append({"sample_id": sample["sample_id"], "role": role_name,
                                    "bundle": bundle, "error": str(error)})
        completions.append(completion_row)
        if set(loaded) != {"fixed", "moving"}:
            continue
        acquired = ClockStamp("unreal-monotonic", sample["acquired_seconds"])
        pair = MeshPairSample(
            sample["sample_id"], acquired,
            _selection(loaded["fixed"], "fixed", roles["fixed"], sample, criteria_value,
                       analysis_limits),
            _selection(loaded["moving"], "moving", roles["moving"], sample, criteria_value,
                       analysis_limits))
        pairs.append(pair)
        results.append(measure_mesh_pair(pair, tolerance=criteria_value["tolerance"],
                                         limits=analysis_limits))

    start = ClockStamp("unreal-monotonic", manifest["samples"][0]["acquired_seconds"])
    end = ClockStamp("unreal-monotonic", manifest["samples"][-1]["acquired_seconds"])
    interval_range = TimeInterval(start, end)
    interval = summarize_mesh_interval(results, interval_range,
        max_gap_seconds=criteria_value["max_gap_seconds"],
        max_samples=criteria_value["max_samples"])
    measurements = [result.to_mapping() for result in results]
    observed = {result.sample_id: result for result in results}
    expectations = []
    for expected in criteria_value["expected_samples"]:
        result = observed.get(expected["sample_id"])
        distance = result.minimum_distance if result and result.status == "measured" else None
        intersection = result.surface_intersection if result and result.status == "measured" else None
        matches = (distance is not None
                   and abs(distance - expected["distance"]) <= criteria_value["tolerance"]
                   and intersection is expected["intersection"])
        expectations.append({
            "sample_id": expected["sample_id"],
            "expected_distance": expected["distance"],
            "observed_distance": distance,
            "expected_intersection": expected["intersection"],
            "observed_intersection": intersection,
            "matches": matches,
        })
    controls = _negative_controls(pairs, results, interval_range, criteria_value,
                                  analysis_limits)
    declared_ids = [row["sample_id"] for row in manifest["samples"]]
    expected_ids = [row["sample_id"] for row in criteria_value["expected_samples"]]
    verified = (manifest["controls"]["checks_passed"] and not read_errors
                and interval.status == "complete" and declared_ids == expected_ids
                and all(row["matches"] for row in expectations)
                and all(row["status"] == "insufficient" for row in controls.values()))
    return {
        "format": "neutral_assembly_report",
        "schema_version": 1,
        "status": "verified" if verified else "insufficient",
        "source_hashes": source_hashes,
        "capture_controls": dict(manifest["controls"]),
        "completions": completions,
        "measurements": measurements,
        "interval": interval.to_mapping(),
        "expectations": expectations,
        "read_errors": read_errors,
        "controls": controls,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observations", required=True)
    parser.add_argument("--criteria", required=True)
    parser.add_argument("--output", required=True)
    try:
        arguments = parser.parse_args(argv)
        report = evaluate_run(arguments.observations, arguments.criteria)
        payload = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        with Path(arguments.output).open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
        return 0 if report["status"] == "verified" else 1
    except (OSError, ValueError, TypeError) as error:
        print(f"neutral assembly qualification failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
