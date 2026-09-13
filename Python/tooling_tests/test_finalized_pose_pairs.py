import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from animation_analysis import (
    ClockStamp,
    FeatureCoverage,
    MeshCompletion,
    MeshObservation,
    MeshRecordLimits,
    MeshRequest,
    MeshSection,
    MeshTopology,
    PoseKey,
    write_mesh_observation,
)


SCRIPT = Path(__file__).parents[1] / "examples" / "finalized_pose_pairs.py"
RECORD_LIMITS = MeshRecordLimits(64, 256, 4, 8192, 65536, 73728)
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
FEATURE_EXCLUSIONS = (
    "morph", "cloth", "mesh_deformer", "material_displacement",
    "raster_visibility",
)
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
CUBE_INDICES = tuple(index for face in range(6)
                     for index in (4 * face, 4 * face + 1, 4 * face + 2,
                                   4 * face + 2, 4 * face + 3, 4 * face))


def _topology(role):
    if role == "body":
        indices = (0, 1, 2, 2, 1, 3)
        vertices = 4
    else:
        indices = CUBE_INDICES
        vertices = 24
    return MeshTopology(
        f"{role}-neutral-asset", 1, 0, vertices,
        struct.pack(f"<{len(indices)}I", *indices),
        (MeshSection("0", 0, len(indices), "neutral-material-0"),),
        RECORD_LIMITS,
    )


def _coverage(role, pair_id, coverage_change=None):
    feature = "bone" if role == "body" else "rigid"
    producer = ("unreal-cpu-bone-reference-v1" if role == "body"
                else "unreal-rigid-reference-v1")
    changed_role, changed_feature, changed_state = coverage_change or (None, None, None)
    return tuple(
        FeatureCoverage(
            name,
            changed_state if role == changed_role and name == changed_feature
            else "observed" if name in (feature, "pose_ordering") else "excluded",
            producer,
            pair_id,
            "independent finalized-pose fixture",
        )
        for name in (feature, "pose_ordering", *FEATURE_EXCLUSIONS)
        if not (role == changed_role and name == changed_feature
                and changed_state == "omitted")
    )


def _observation(role, topology, pair_id, index, acquired, frame_id,
                 body_revision, attached_revision, coverage_change):
    if role == "body":
        offset = BODY_OFFSETS[index]
        positions = ((offset, -10, -10), (offset, -10, 10),
                     (offset, 10, -10), (offset, 10, 10))
        transform = COMMON_TRANSFORM
        component = "animated-body"
        producer = "unreal-cpu-bone-reference-v1"
        revision = body_revision
    else:
        positions = CUBE_POSITIONS
        scale = 5.0 / 128.0
        transform = tuple(
            COMMON_TRANSFORM[row * 4 + column] * scale
            if row < 3 and column < 3 else 0.0
            for row in range(4) for column in range(4)
        )
        transform = (*transform[:12], *ATTACHED_ORIGINS[index], 1.0)
        component = "attached-part"
        producer = "unreal-rigid-reference-v1"
        revision = attached_revision
    pose = PoseKey(component, "finalized-pose", frame_id, revision)
    return MeshObservation(
        component, 1, topology,
        struct.pack(f"<{len(positions) * 3}d",
                    *(value for point in positions for value in point)),
        "centimetres", "unreal-left-handed-z-up", "row", transform, pose,
        acquired, producer, f"neutral-finalized-pose:{role}-configuration",
        _coverage(role, pair_id, coverage_change), RECORD_LIMITS,
    )


def write_run(root, *, coverage_change=None,
              frame_ids=(791, 792, 793, 794, 795),
              body_revisions=(12, 14, 16, 18, 20),
              attached_revisions=(4, 6, 8, 10, 12)):
    topologies = {role: _topology(role) for role in ("body", "attached_part")}
    roles = {
        "body": {
            "component_id": "animated-body", "component_generation": 1,
            "subject_id": "animated-body", "stream_id": "finalized-pose",
        },
        "attached_part": {
            "component_id": "attached-part", "component_generation": 1,
            "subject_id": "attached-part", "stream_id": "finalized-pose",
        },
    }
    pairs = []
    for index, pair_id in enumerate(PAIR_IDS):
        acquired = ClockStamp("unreal-monotonic", index * 0.2)
        observations = {
            role: _observation(
                role, topologies[role], pair_id, index, acquired, frame_ids[index],
                body_revisions[index], attached_revisions[index], coverage_change)
            for role in topologies
        }
        row = {
            "pair_id": pair_id,
            "body_bundle": f"{pair_id}-body",
            "attached_part_bundle": f"{pair_id}-attached-part",
            "frame_id": frame_ids[index],
            "acquired_seconds": acquired.seconds,
            "body": {
                "pose_revision": body_revisions[index],
                "configuration_id": observations["body"].configuration_id,
                "topology_id": topologies["body"].identity,
                "completed_seconds": acquired.seconds + 0.01,
            },
            "attached_part": {
                "pose_revision": attached_revisions[index],
                "configuration_id": observations["attached_part"].configuration_id,
                "topology_id": topologies["attached_part"].identity,
                "completed_seconds": acquired.seconds + 0.02,
            },
            "expected": {
                "animation_position_seconds": ANIMATION_POSITIONS[index],
                "body_position_offset_cm": [BODY_OFFSETS[index], 0.0, 0.0],
                "attached_part_world_origin_cm": list(ATTACHED_ORIGINS[index]),
                "minimum_distance_cm": EXPECTED_DISTANCES[index],
                "surface_intersection": EXPECTED_INTERSECTIONS[index],
            },
        }
        pairs.append(row)
        for role, observation in observations.items():
            request = MeshRequest(
                pair_id, observation.component_id, observation.component_generation,
                observation.configuration_id, observation.pose, observation.acquired,
                observation.topology.identity,
            )
            completion = MeshCompletion(
                request, "completed",
                ClockStamp("unreal-monotonic", row[role]["completed_seconds"]),
                "", observation,
            )
            write_mesh_observation(root, row[f"{role}_bundle"], completion,
                                   limits=RECORD_LIMITS)
    manifest = {
        "format": "neutral_finalized_pose_pairs",
        "schema_version": 1,
        "roles": roles,
        "pairs": pairs,
        "controls": {"checks_passed": True, "peak_reserved_bytes": 11550560},
    }
    (root / "finalized-pose-pairs.json").write_text(
        json.dumps(manifest), encoding="utf-8")
    return manifest


def load_example(test):
    test.assertTrue(SCRIPT.is_file(), "finalized pose pair example is not implemented")
    spec = importlib.util.spec_from_file_location("finalized_pose_pairs_example", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hashes(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*")) if path.is_file()
    }


class FinalizedPosePairTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = write_run(self.root)

    def _write_manifest(self, value):
        (self.root / "finalized-pose-pairs.json").write_text(
            json.dumps(value), encoding="utf-8")

    def test_fixed_native_profile_reports_two_component_pair_results(self):
        example = load_example(self)
        before = hashes(self.root)

        report = example.evaluate_run(self.root)

        self.assertEqual(report["format"], "neutral_finalized_pose_pair_report")
        self.assertEqual(report["status"], "verified")
        for result, expected in zip(report["pair_results"], EXPECTED_DISTANCES):
            self.assertAlmostEqual(result["minimum_distance"], expected, delta=0.001)
        self.assertEqual([row["surface_intersection"] for row in report["pair_results"]],
                         list(EXPECTED_INTERSECTIONS))
        self.assertEqual(report["interval"]["between_samples"], "not_evaluated")
        self.assertEqual(report["read_errors"], [])
        self.assertTrue(all(row["matches"] for row in report["expected_observed_checks"]))
        self.assertEqual(report["capture_controls"],
                         {"checks_passed": True, "peak_reserved_bytes": 11550560})
        self.assertEqual(report["qualification_profile"], {
            "record_limits": {
                "max_vertices": 64, "max_indices": 256, "max_sections": 4,
                "max_payload_bytes": 8192, "max_metadata_bytes": 65536,
                "max_record_bytes": 73728,
            },
            "analysis_limits": {
                "max_triangles": 16, "max_pair_tests": 256,
                "max_node_visits": 1024, "max_coordinate_bits": 256,
            },
            "max_samples": 8,
            "max_gap_seconds": 1.0,
            "proximity_tolerance_cm": 0.001,
            "numeric_tolerance_cm": 0.001,
            "between_samples": "not_evaluated",
        })
        self.assertEqual(
            set(report["controls"]),
            {"wrong_pose", "one_side_replacement", "required_excluded_coverage",
             "exhausted_work", "missing_endpoint", "excessive_gap"},
        )
        self.assertTrue(all(row["status"] == "insufficient"
                            for row in report["controls"].values()))
        reasons = {
            "wrong_pose": "pose:mismatch",
            "one_side_replacement": "component_generation:mismatch",
            "required_excluded_coverage": "cloth:excluded",
            "exhausted_work": "work_limit:",
            "missing_endpoint": "missing_start_sample",
            "excessive_gap": "exceeds_max_gap",
        }
        for name, fragment in reasons.items():
            self.assertTrue(any(fragment in reason
                                for reason in report["controls"][name]["reasons"]), name)
        self.assertEqual(len(report["completions"]), 5)
        first = report["completions"][0]
        self.assertEqual(first["body"]["acquired"], first["attached_part"]["acquired"])
        self.assertNotEqual(first["body"]["completed"],
                            first["attached_part"]["completed"])
        self.assertNotEqual(first["body"]["pose"]["revision"],
                            first["attached_part"]["pose"]["revision"])
        self.assertEqual(before, hashes(self.root), "evaluation changed replay files")
        self.assertEqual(len(report["input_hashes"]), 41)
        json.dumps(report, allow_nan=False)

    def test_manifest_identity_and_completion_declarations_bind_loaded_records(self):
        example = load_example(self)
        cases = (
            ("component", lambda value: value["roles"]["body"].__setitem__(
                "component_id", "replacement-body"), ValueError),
            ("configuration", lambda value: value["pairs"][0]["body"].__setitem__(
                "configuration_id", "replacement-configuration"), None),
            ("topology", lambda value: value["pairs"][0]["body"].__setitem__(
                "topology_id", "0" * 64), None),
            ("pose", lambda value: value["pairs"][0]["body"].__setitem__(
                "pose_revision", 999), None),
            ("completion", lambda value: value["pairs"][0]["body"].__setitem__(
                "completed_seconds", 99.0), None),
        )
        for label, mutate, exception in cases:
            with self.subTest(label=label):
                value = copy.deepcopy(self.manifest)
                mutate(value)
                self._write_manifest(value)
                if exception:
                    with self.assertRaises(exception):
                        example.evaluate_run(self.root)
                else:
                    report = example.evaluate_run(self.root)
                    self.assertEqual(report["status"], "insufficient")
                    self.assertTrue(report["read_errors"] or report["identity_errors"] or
                                    report["pair_results"][0]["status"] == "insufficient")

    def test_strict_json_rejects_unknown_duplicate_nonfinite_and_oversize_inputs(self):
        example = load_example(self)
        path = self.root / "finalized-pose-pairs.json"
        unknown = copy.deepcopy(self.manifest)
        unknown["pairs"][0]["body"]["surprise"] = 1
        boolean_schema = copy.deepcopy(self.manifest)
        boolean_schema["schema_version"] = True
        bad = [
            json.dumps(unknown),
            json.dumps(boolean_schema),
            json.dumps(self.manifest).replace(
                '"schema_version": 1', '"schema_version": 1, "schema_version": 1', 1),
            json.dumps(self.manifest).replace(
                '"acquired_seconds": 0.0', '"acquired_seconds": NaN', 1),
            " " * 65537,
        ]
        for payload in bad:
            with self.subTest(payload=payload[:40]):
                path.write_text(payload, encoding="utf-8")
                with self.assertRaises(ValueError):
                    example.evaluate_run(self.root)

    def test_fixed_profile_cannot_be_shortened_or_self_authorize_changed_expectations(self):
        example = load_example(self)
        cases = (
            ("pair_id", lambda value: value["pairs"][0].__setitem__("pair_id", "other")),
            ("shortened", lambda value: value.__setitem__("pairs", value["pairs"][:4])),
            ("animation", lambda value: value["pairs"][0]["expected"].__setitem__(
                "animation_position_seconds", 0.2)),
            ("body_offset", lambda value: value["pairs"][0]["expected"].__setitem__(
                "body_position_offset_cm", [2, 0, 0])),
            ("origin", lambda value: value["pairs"][0]["expected"].__setitem__(
                "attached_part_world_origin_cm", [0, 0, 0])),
            ("distance", lambda value: value["pairs"][0]["expected"].__setitem__(
                "minimum_distance_cm", 30)),
            ("intersection", lambda value: value["pairs"][0]["expected"].__setitem__(
                "surface_intersection", True)),
        )
        for label, mutate in cases:
            with self.subTest(label=label):
                value = copy.deepcopy(self.manifest)
                mutate(value)
                self._write_manifest(value)
                with self.assertRaises(ValueError):
                    example.evaluate_run(self.root)

    def test_exact_role_coverage_rejects_inactive_or_missing_fixed_evidence(self):
        example = load_example(self)
        cases = [
            ("body_bone_inactive", ("body", "bone", "inactive"), "bone:inactive"),
            ("attached_rigid_inactive", ("attached_part", "rigid", "inactive"),
             "rigid:inactive"),
            ("pose_ordering_inactive", ("body", "pose_ordering", "inactive"),
             "pose_ordering:inactive"),
            ("excluded_cloth_inactive", ("attached_part", "cloth", "inactive"),
             "cloth:inactive"),
        ]
        cases.extend(
            (f"missing_{feature}", ("body", feature, "omitted"), f"{feature}:missing")
            for feature in FEATURE_EXCLUSIONS
        )
        for label, change, reason in cases:
            with self.subTest(label=label):
                root = self.root / label
                root.mkdir()
                write_run(root, coverage_change=change)

                report = example.evaluate_run(root)

                self.assertEqual(report["status"], "insufficient")
                self.assertTrue(any(reason in row["error"]
                                    for row in report["identity_errors"]), reason)

    def test_distinct_frames_are_required_with_manifest_and_records_bound(self):
        example = load_example(self)
        root = self.root / "repeated-frame"
        root.mkdir()
        write_run(root, frame_ids=(791, 791, 793, 794, 795))

        with self.assertRaisesRegex(ValueError, "distinct.*frame"):
            example.evaluate_run(root)

    def test_first_observer_revisions_must_differ_with_manifest_and_records_bound(self):
        example = load_example(self)
        root = self.root / "same-first-revision"
        root.mkdir()
        write_run(root, body_revisions=(4, 14, 16, 18, 20))

        with self.assertRaisesRegex(ValueError, "observer.*revision"):
            example.evaluate_run(root)

    def test_missing_input_stays_insufficient_when_surviving_interval_meets_gap(self):
        example = load_example(self)
        (self.root / "finalized-pair-02-attached-part" / "complete.json").unlink()

        report = example.evaluate_run(self.root)

        self.assertEqual(report["status"], "insufficient")
        self.assertEqual(report["interval"]["status"], "complete")
        self.assertTrue(all(not gap["exceeds_max_gap"]
                            for gap in report["interval"]["gaps"]))
        self.assertEqual(report["read_errors"][0]["pair_id"], "finalized-pair-02")
        missing = next(row for row in report["expected_observed_checks"]
                       if row["pair_id"] == "finalized-pair-02")
        self.assertFalse(missing["matches"])
        self.assertIsNone(missing["observed_minimum_distance_cm"])

    def test_manifest_must_be_a_regular_file(self):
        example = load_example(self)
        root = self.root / "directory-manifest"
        (root / "finalized-pose-pairs.json").mkdir(parents=True)

        with self.assertRaisesRegex(ValueError, "regular file"):
            example.evaluate_run(root)

    @unittest.skipUnless(hasattr(os, "symlink"), "platform has no symlink support")
    def test_manifest_symlink_is_rejected_when_platform_permits_creation(self):
        example = load_example(self)
        root = self.root / "linked-manifest"
        root.mkdir()
        target = root / "target.json"
        target.write_text(json.dumps(self.manifest), encoding="utf-8")
        link = root / "finalized-pose-pairs.json"
        try:
            link.symlink_to(target)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symlink creation unavailable: {error}")

        with self.assertRaisesRegex(ValueError, "regular file"):
            example.evaluate_run(root)

    def test_cli_exclusively_publishes_verified_insufficient_and_malformed_results(self):
        example = load_example(self)
        verified = self.root / "verified.json"
        self.assertEqual(example.main(["--observations", str(self.root),
                                       "--output", str(verified)]), 0)
        self.assertEqual(json.loads(verified.read_text())["status"], "verified")
        original = verified.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(example.main(["--observations", str(self.root),
                                           "--output", str(verified)]), 2)
        self.assertEqual(verified.read_bytes(), original)

        insufficient = self.root / "insufficient.json"
        value = copy.deepcopy(self.manifest)
        value["controls"]["checks_passed"] = False
        self._write_manifest(value)
        self.assertEqual(example.main(["--observations", str(self.root),
                                       "--output", str(insufficient)]), 1)
        malformed = self.root / "malformed.json"
        self._write_manifest({})
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(example.main(["--observations", str(self.root),
                                           "--output", str(malformed)]), 2)
        self.assertFalse(malformed.exists())


if __name__ == "__main__":
    unittest.main()
