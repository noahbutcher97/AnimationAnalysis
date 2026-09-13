"""Qualify the fixed neutral finalized-pose pair replay with installed public APIs."""
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
    "max_indices": 256,
    "max_sections": 4,
    "max_payload_bytes": 8192,
    "max_metadata_bytes": 65536,
    "max_record_bytes": 73728,
}
ANALYSIS_CAPS = {
    "max_triangles": 16,
    "max_pair_tests": 256,
    "max_node_visits": 1024,
    "max_coordinate_bits": 256,
}
MAX_SAMPLES = 8
MAX_GAP_SECONDS = 1.0
PROXIMITY_TOLERANCE_CM = 0.001
NUMERIC_TOLERANCE_CM = 0.001
PAIR_IDS = tuple(f"finalized-pair-{index:02d}" for index in range(5))
ANIMATION_POSITIONS = (0.1, 0.2, 0.3, 0.4, 0.5)
BODY_OFFSETS = (1.0, 2.0, 3.0, 4.0, 5.0)
ATTACHED_ORIGINS = (
    (336.37412127436448, -109.1528291647774, 903.7779496455596),
    (328.71711832374257, -113.75362069758395, 902.6811255549133),
    (321.0601153731206, -118.35441223039052, 901.5843014642669),
    (316.8062248449973, -120.91040752639417, 900.9749547472412),
    (314.2538905281233, -122.44400470399636, 900.6093467170257),
)
EXPECTED_DISTANCES = (25.0, 15.0, 5.0, 0.0, 0.0)
EXPECTED_INTERSECTIONS = (False, False, False, True, True)
EXCLUSIONS = (
    "morph", "cloth", "mesh_deformer", "material_displacement",
    "raster_visibility",
)
ROLE_PROFILE = {
    "body": {
        "component_id": "animated-body",
        "component_generation": 1,
        "subject_id": "animated-body",
        "stream_id": "finalized-pose",
        "feature": "bone",
        "producer": "unreal-cpu-bone-reference-v1",
        "vertices": 4,
        "triangles": 2,
    },
    "attached_part": {
        "component_id": "attached-part",
        "component_generation": 1,
        "subject_id": "attached-part",
        "stream_id": "finalized-pose",
        "feature": "rigid",
        "producer": "unreal-rigid-reference-v1",
        "vertices": 24,
        "triangles": 12,
    },
}
COMMON_TRANSFORM = (
    0.8507781056246613, 0.5111990592007294, 0.12186934340514755, 0.0,
    -0.5210703978329632, 0.8507008550956521, 0.06923651956680049, 0.0,
    -0.06828071097982338, -0.12240742220884954, 0.990128359101119, 0.0,
    310.0, -125.0, 900.0, 1.0,
)
CUBE_POSITIONS = (
    (-128, 128, -128), (-128, -128, -128), (128, -128, -128), (128, 128, -128),
    (-128, 128, 128), (128, 128, 128), (128, -128, 128), (-128, -128, 128),
    (-128, 128, -128), (128, 128, -128), (128, 128, 128), (-128, 128, 128),
    (128, 128, -128), (128, -128, -128), (128, -128, 128), (128, 128, 128),
    (128, -128, -128), (-128, -128, -128), (-128, -128, 128), (128, -128, 128),
    (-128, -128, -128), (-128, 128, -128), (-128, 128, 128), (-128, -128, 128),
)
TOP_FIELDS = {"format", "schema_version", "roles", "pairs", "controls"}
ROLE_FIELDS = {"component_id", "component_generation", "subject_id", "stream_id"}
PAIR_FIELDS = {
    "pair_id", "body_bundle", "attached_part_bundle", "frame_id",
    "acquired_seconds", "body", "attached_part", "expected",
}
PARTICIPANT_FIELDS = {
    "pose_revision", "configuration_id", "topology_id", "completed_seconds",
}
EXPECTED_FIELDS = {
    "animation_position_seconds", "body_position_offset_cm",
    "attached_part_world_origin_cm", "minimum_distance_cm", "surface_intersection",
}


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def _read_bytes(path, maximum):
    path = Path(path)
    info = path.lstat()
    attributes = getattr(info, "st_file_attributes", 0)
    if (not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode)
            or attributes & 0x400):
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
    data = _read_bytes(path, JSON_LIMIT)

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


def _integer(value, label, minimum=0):
    if type(value) is not int or not minimum <= value <= 2**53 - 1:
        raise ValueError(f"Expected bounded integer {label}")
    return value


def _number(value, label):
    try:
        valid = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(f"Expected finite number {label}")
    return value


def _sha256(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"Expected SHA-256 {label}")
    return value


def _vector(value, label):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"Expected three-coordinate {label}")
    return tuple(_number(item, label) for item in value)


def _fixed(name, observed, expected):
    if observed != expected:
        raise ValueError(f"Finalized-pose {name} must match the fixed qualification profile")


def _validate_manifest(value):
    value = _mapping(value, TOP_FIELDS, "finalized-pose manifest")
    if (value["format"] != "neutral_finalized_pose_pairs"
            or type(value["schema_version"]) is not int or value["schema_version"] != 1):
        raise ValueError("Unsupported finalized-pose manifest schema")
    roles = _mapping(value["roles"], ROLE_PROFILE, "roles")
    for role_name, role in roles.items():
        _mapping(role, ROLE_FIELDS, f"{role_name} role")
        for name in ("component_id", "subject_id", "stream_id"):
            _text(role[name], f"{role_name}.{name}")
        _integer(role["component_generation"], f"{role_name}.component_generation")
        fixed_role = {name: ROLE_PROFILE[role_name][name] for name in ROLE_FIELDS}
        _fixed(f"roles.{role_name}", role, fixed_role)
    rows = value["pairs"]
    if not isinstance(rows, list) or len(rows) != len(PAIR_IDS):
        raise ValueError("Finalized-pose pairs must contain the exact five-sample profile")
    bundles = set()
    for index, row in enumerate(rows):
        _mapping(row, PAIR_FIELDS, "pair declaration")
        pair_id = _text(row["pair_id"], "pair identity")
        _fixed(f"pairs[{index}].pair_id", pair_id, PAIR_IDS[index])
        for role_name in ROLE_PROFILE:
            bundle = _text(row[f"{role_name}_bundle"], "bundle name")
            if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", bundle)
                    or bundle in bundles):
                raise ValueError("Unsafe or duplicate bundle declaration")
            bundles.add(bundle)
            participant = _mapping(row[role_name], PARTICIPANT_FIELDS,
                                   f"{role_name} participant")
            _integer(participant["pose_revision"], f"{role_name}.pose_revision")
            _text(participant["configuration_id"], f"{role_name}.configuration_id")
            _sha256(participant["topology_id"], f"{role_name}.topology_id")
            _number(participant["completed_seconds"], f"{role_name}.completed_seconds")
        _integer(row["frame_id"], "frame_id")
        _number(row["acquired_seconds"], "acquired_seconds")
        expected = _mapping(row["expected"], EXPECTED_FIELDS, "pair expectation")
        animation = _number(expected["animation_position_seconds"], "animation position")
        body_offset = _vector(expected["body_position_offset_cm"], "body offset")
        origin = _vector(expected["attached_part_world_origin_cm"], "attached origin")
        distance = _number(expected["minimum_distance_cm"], "minimum distance")
        if type(expected["surface_intersection"]) is not bool:
            raise ValueError("Expected surface intersection must be boolean")
        _fixed(f"pairs[{index}].animation_position_seconds",
               animation, ANIMATION_POSITIONS[index])
        _fixed(f"pairs[{index}].body_position_offset_cm",
               body_offset, (BODY_OFFSETS[index], 0, 0))
        _fixed(f"pairs[{index}].attached_part_world_origin_cm",
               origin, ATTACHED_ORIGINS[index])
        _fixed(f"pairs[{index}].minimum_distance_cm",
               distance, EXPECTED_DISTANCES[index])
        _fixed(f"pairs[{index}].surface_intersection",
               expected["surface_intersection"], EXPECTED_INTERSECTIONS[index])
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
        "component_id": request.component_id,
        "component_generation": request.component_generation,
        "configuration_id": request.configuration_id,
        "topology_id": request.topology_id,
        "pose": asdict(request.pose) if request.pose else None,
        "acquired": asdict(request.acquired) if request.acquired else None,
        "completed": asdict(completion.completed),
    }


def _bundle_hashes(root, bundle):
    limits = {
        "complete.json": JSON_LIMIT,
        "record.json": RECORD_CAPS["max_metadata_bytes"],
        "positions.bin": RECORD_CAPS["max_payload_bytes"],
        "indices.bin": RECORD_CAPS["max_payload_bytes"],
    }
    return {
        f"{bundle}/{name}": hashlib.sha256(
            _read_bytes(Path(root) / bundle / name, maximum)).hexdigest()
        for name, maximum in limits.items()
    }


def _requirement(role_name, role, participant, frame_id):
    profile = ROLE_PROFILE[role_name]
    pose = PoseKey(role["subject_id"], role["stream_id"], frame_id,
                   participant["pose_revision"])
    feature = profile["feature"]
    return MeshRequirement(
        role["component_id"], role["component_generation"],
        participant["topology_id"], participant["configuration_id"], pose,
        "centimetres", "unreal-left-handed-z-up",
        (feature, "pose_ordering"), (feature, "pose_ordering", *EXCLUSIONS),
        EXCLUSIONS, (profile["producer"],),
    )


def _selection(observation, role_name, role, participant, frame_id, limits):
    triangle_count = ROLE_PROFILE[role_name]["triangles"]
    region = MeshRegion(
        f"{role_name}-complete-surface", participant["topology_id"],
        tuple(range(triangle_count)), limits,
    )
    return MeshSelection(
        observation, region,
        _requirement(role_name, role, participant, frame_id),
    )


def _close(left, right):
    return abs(left - right) <= NUMERIC_TOLERANCE_CM


def _vector_close(left, right):
    return len(left) == len(right) and all(_close(a, b) for a, b in zip(left, right))


def _analytic_check(index, loaded, result):
    offset = BODY_OFFSETS[index]
    expected_body = (
        (offset, -10, -10), (offset, -10, 10),
        (offset, 10, -10), (offset, 10, 10),
    )
    body = loaded.get("body")
    attached = loaded.get("attached_part")
    body_positions = tuple(body.positions()) if body else ()
    attached_positions = tuple(attached.positions()) if attached else ()
    observed_offset = [body_positions[0][0], 0.0, 0.0] if body_positions else None
    origin = list(attached.component_to_world[12:15]) if attached else None
    body_geometry = (
        body is not None
        and body.topology.vertex_count == ROLE_PROFILE["body"]["vertices"]
        and len(body.topology.index_data) // 12 == ROLE_PROFILE["body"]["triangles"]
        and len(body_positions) == len(expected_body)
        and all(_vector_close(actual, expected)
                for actual, expected in zip(body_positions, expected_body))
        and _vector_close(body.component_to_world, COMMON_TRANSFORM)
    )
    scale = 5.0 / 128.0
    expected_basis = tuple(COMMON_TRANSFORM[row * 4 + column] * scale
                           for row in range(3) for column in range(3))
    actual_basis = tuple(attached.component_to_world[row * 4 + column]
                         for row in range(3) for column in range(3)) if attached else ()
    attached_geometry = (
        attached is not None
        and attached.topology.vertex_count == ROLE_PROFILE["attached_part"]["vertices"]
        and len(attached.topology.index_data) // 12 == ROLE_PROFILE["attached_part"]["triangles"]
        and len(attached_positions) == len(CUBE_POSITIONS)
        and all(_vector_close(actual, expected)
                for actual, expected in zip(attached_positions, CUBE_POSITIONS))
        and _vector_close(actual_basis, expected_basis)
        and _vector_close(origin, ATTACHED_ORIGINS[index])
    )
    distance = result.minimum_distance if result and result.status == "measured" else None
    intersection = (result.surface_intersection
                    if result and result.status == "measured" else None)
    matches = (
        body_geometry and attached_geometry and distance is not None
        and _close(distance, EXPECTED_DISTANCES[index])
        and intersection is EXPECTED_INTERSECTIONS[index]
    )
    return {
        "pair_id": PAIR_IDS[index],
        "declared_animation_position_seconds": ANIMATION_POSITIONS[index],
        "expected_body_position_offset_cm": [BODY_OFFSETS[index], 0.0, 0.0],
        "observed_body_position_offset_cm": observed_offset,
        "expected_attached_part_world_origin_cm": list(ATTACHED_ORIGINS[index]),
        "observed_attached_part_world_origin_cm": origin,
        "expected_minimum_distance_cm": EXPECTED_DISTANCES[index],
        "observed_minimum_distance_cm": distance,
        "expected_surface_intersection": EXPECTED_INTERSECTIONS[index],
        "observed_surface_intersection": intersection,
        "body_geometry_matches": body_geometry,
        "attached_part_geometry_matches": attached_geometry,
        "matches": matches,
    }


def _control_mapping(result):
    mapping = result.to_mapping()
    return {
        "kind": "detached_in_memory_control",
        "status": mapping["status"],
        "reasons": mapping["reasons"],
        "result": mapping,
    }


def _interval_control(summary):
    mapping = summary.to_mapping()
    return {
        "kind": "detached_in_memory_control",
        "status": mapping["status"],
        "reasons": mapping["reasons"],
        "result": mapping,
    }


def _negative_controls(samples, results, interval, limits):
    labels = (
        "wrong_pose", "one_side_replacement", "required_excluded_coverage",
        "exhausted_work", "missing_endpoint", "excessive_gap",
    )
    measured = [(sample, result) for sample, result in zip(samples, results)
                if result.status == "measured"]
    if not measured:
        return {
            label: {
                "kind": "detached_in_memory_control",
                "status": "insufficient",
                "reasons": ["primary_measurement_unavailable"],
            }
            for label in labels
        }
    pair, _ = measured[0]
    controls = {}
    pose = pair.first.requirement.pose
    wrong = replace(pair.first.requirement,
                    pose=replace(pose, revision=pose.revision + 1))
    controls["wrong_pose"] = _control_mapping(measure_mesh_pair(
        replace(pair, first=replace(pair.first, requirement=wrong)),
        tolerance=PROXIMITY_TOLERANCE_CM, limits=limits))
    replacement = replace(
        pair.second.requirement,
        component_generation=pair.second.requirement.component_generation + 1,
    )
    controls["one_side_replacement"] = _control_mapping(measure_mesh_pair(
        replace(pair, second=replace(pair.second, requirement=replacement)),
        tolerance=PROXIMITY_TOLERANCE_CM, limits=limits))
    requirement = pair.first.requirement
    excluded = replace(
        requirement,
        required_features=(*requirement.required_features, "cloth"),
        allowed_exclusions=tuple(name for name in requirement.allowed_exclusions
                                 if name != "cloth"),
    )
    controls["required_excluded_coverage"] = _control_mapping(measure_mesh_pair(
        replace(pair, first=replace(pair.first, requirement=excluded)),
        tolerance=PROXIMITY_TOLERANCE_CM, limits=limits))
    exhausted = MeshAnalysisLimits(limits.max_triangles, 1, 1,
                                   limits.max_coordinate_bits)
    controls["exhausted_work"] = _control_mapping(measure_mesh_pair(
        pair, tolerance=PROXIMITY_TOLERANCE_CM, limits=exhausted))
    missing = summarize_mesh_interval(
        results[1:], interval, max_gap_seconds=MAX_GAP_SECONDS,
        max_samples=MAX_SAMPLES)
    controls["missing_endpoint"] = _interval_control(missing)
    gaps = [right.acquired.seconds - left.acquired.seconds
            for left, right in zip(results, results[1:])
            if right.acquired.domain == left.acquired.domain
            and right.acquired.seconds > left.acquired.seconds]
    strict_gap = min(gaps) / 2 if gaps else MAX_GAP_SECONDS / 2
    excessive = summarize_mesh_interval(
        results, interval, max_gap_seconds=strict_gap, max_samples=MAX_SAMPLES)
    controls["excessive_gap"] = _interval_control(excessive)
    return controls


def evaluate_run(observations):
    """Return a detached report for one explicit finalized-pose manifest."""
    root = Path(observations)
    manifest, manifest_hash = _read_json(root / "finalized-pose-pairs.json")
    manifest = _validate_manifest(manifest)
    record_limits = MeshRecordLimits(**RECORD_CAPS)
    analysis_limits = MeshAnalysisLimits(**ANALYSIS_CAPS)
    input_hashes = {"finalized-pose-pairs.json": manifest_hash}
    read_errors = []
    identity_errors = []
    completions = []
    samples = []
    results = []
    loaded_by_pair = {}
    roles = manifest["roles"]

    for declared in manifest["pairs"]:
        pair_id = declared["pair_id"]
        completion_row = {"pair_id": pair_id}
        loaded = {}
        for role_name in ROLE_PROFILE:
            bundle = declared[f"{role_name}_bundle"]
            try:
                completion = read_mesh_observation(root, bundle, limits=record_limits)
                completion_row[role_name] = _completion_mapping(completion)
                if completion.status != "completed" or completion.observation is None:
                    raise ValueError(f"bundle completed with status {completion.status}")
                if completion.request.request_id != pair_id:
                    identity_errors.append({
                        "pair_id": pair_id, "role": role_name, "bundle": bundle,
                        "error": "request identity differs from pair declaration",
                    })
                if (completion.completed.domain != "unreal-monotonic"
                        or completion.completed.seconds
                        != declared[role_name]["completed_seconds"]):
                    identity_errors.append({
                        "pair_id": pair_id, "role": role_name, "bundle": bundle,
                        "error": "completion identity differs from pair declaration",
                    })
                observation = completion.observation
                if observation.vector_convention != "row":
                    identity_errors.append({
                        "pair_id": pair_id, "role": role_name, "bundle": bundle,
                        "error": "vector convention differs from fixed profile",
                    })
                loaded[role_name] = observation
                input_hashes.update(_bundle_hashes(root, bundle))
            except (OSError, ValueError, TypeError) as error:
                read_errors.append({
                    "pair_id": pair_id, "role": role_name, "bundle": bundle,
                    "error": str(error),
                })
        completions.append(completion_row)
        loaded_by_pair[pair_id] = loaded
        if set(loaded) != set(ROLE_PROFILE):
            continue
        acquired = ClockStamp("unreal-monotonic", declared["acquired_seconds"])
        sample = MeshPairSample(
            pair_id, acquired,
            _selection(loaded["body"], "body", roles["body"], declared["body"],
                       declared["frame_id"], analysis_limits),
            _selection(loaded["attached_part"], "attached_part", roles["attached_part"],
                       declared["attached_part"], declared["frame_id"], analysis_limits),
        )
        samples.append(sample)
        results.append(measure_mesh_pair(
            sample, tolerance=PROXIMITY_TOLERANCE_CM, limits=analysis_limits))

    start = ClockStamp("unreal-monotonic", manifest["pairs"][0]["acquired_seconds"])
    end = ClockStamp("unreal-monotonic", manifest["pairs"][-1]["acquired_seconds"])
    interval_range = TimeInterval(start, end)
    interval = summarize_mesh_interval(
        results, interval_range, max_gap_seconds=MAX_GAP_SECONDS,
        max_samples=MAX_SAMPLES)
    by_id = {result.sample_id: result for result in results}
    checks = [
        _analytic_check(index, loaded_by_pair.get(pair_id, {}), by_id.get(pair_id))
        for index, pair_id in enumerate(PAIR_IDS)
    ]
    controls = _negative_controls(samples, results, interval_range, analysis_limits)
    verified = (
        manifest["controls"]["checks_passed"]
        and not read_errors and not identity_errors
        and len(results) == len(PAIR_IDS)
        and all(result.status == "measured" for result in results)
        and interval.status == "complete"
        and all(check["matches"] for check in checks)
        and all(control["status"] == "insufficient" for control in controls.values())
    )
    return {
        "format": "neutral_finalized_pose_pair_report",
        "schema_version": 1,
        "status": "verified" if verified else "insufficient",
        "qualification_profile": {
            "record_limits": dict(RECORD_CAPS),
            "analysis_limits": dict(ANALYSIS_CAPS),
            "max_samples": MAX_SAMPLES,
            "max_gap_seconds": MAX_GAP_SECONDS,
            "proximity_tolerance_cm": PROXIMITY_TOLERANCE_CM,
            "numeric_tolerance_cm": NUMERIC_TOLERANCE_CM,
            "between_samples": "not_evaluated",
        },
        "input_hashes": input_hashes,
        "capture_controls": dict(manifest["controls"]),
        "completions": completions,
        "pair_results": [result.to_mapping() for result in results],
        "interval": interval.to_mapping(),
        "expected_observed_checks": checks,
        "read_errors": read_errors,
        "identity_errors": identity_errors,
        "controls": controls,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observations", required=True)
    parser.add_argument("--output", required=True)
    try:
        arguments = parser.parse_args(argv)
        report = evaluate_run(arguments.observations)
        payload = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        with Path(arguments.output).open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
        return 0 if report["status"] == "verified" else 1
    except (OSError, ValueError, TypeError) as error:
        print(f"finalized-pose qualification failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
