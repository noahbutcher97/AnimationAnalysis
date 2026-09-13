import copy
import contextlib
import hashlib
import importlib.util
import io
import json
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


SCRIPT = Path(__file__).parents[1] / "examples" / "neutral_assembly.py"
RECORD_LIMITS = MeshRecordLimits(64, 128, 4, 8192, 65536, 73728)
POSITIONS = (
    (-50, -50, -50), (50, -50, -50), (50, 50, -50), (-50, 50, -50),
    (-50, -50, 50), (50, -50, 50), (50, 50, 50), (-50, 50, 50),
)
TRIANGLES = (
    (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
    (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
    (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
)
EXPECTED_DISTANCES = [40, 10, 0, 0, 40]
EXPECTED_INTERSECTIONS = [False, False, True, True, False]
FEATURES = ("rigid", "pose_ordering", "morph", "cloth", "mesh_deformer",
            "material_displacement", "raster_visibility")


def criteria():
    return {
        "format": "neutral_assembly_criteria",
        "schema_version": 1,
        "units": "centimetres",
        "coordinate_system": "unreal-left-handed-z-up",
        "vector_convention": "row",
        "required_features": ["rigid", "pose_ordering"],
        "known_features": list(FEATURES),
        "allowed_exclusions": list(FEATURES[2:]),
        "allowed_producers": ["unreal-rigid-reference-v1"],
        "record_limits": {
            "max_vertices": 64,
            "max_indices": 128,
            "max_sections": 4,
            "max_payload_bytes": 8192,
            "max_metadata_bytes": 65536,
            "max_record_bytes": 73728,
        },
        "analysis_limits": {
            "max_triangles": 12,
            "max_pair_tests": 256,
            "max_node_visits": 1024,
            "max_coordinate_bits": 256,
        },
        "max_samples": 8,
        "max_gap_seconds": 1.0,
        "tolerance": 0.0,
        "expected_samples": [
            {"sample_id": f"step-{index:02d}", "distance": distance,
             "intersection": intersects}
            for index, (distance, intersects) in enumerate(
                zip(EXPECTED_DISTANCES, EXPECTED_INTERSECTIONS))
        ],
    }


def _topology(role):
    flat = tuple(index for triangle in TRIANGLES for index in triangle)
    return MeshTopology(
        f"{role}-cube", 1, 0, len(POSITIONS), struct.pack("<36I", *flat),
        (MeshSection("surface", 0, 36, "fixture-material"),), RECORD_LIMITS)


def _observation(role, topology, sample_id, sample_index, center_x):
    pose = PoseKey(f"{role}-part", "assembly", 100 + sample_index, 10 + sample_index)
    acquired = ClockStamp("unreal-monotonic", sample_index * 0.2)
    producer = "unreal-rigid-reference-v1"
    coverage = tuple(
        FeatureCoverage(feature, "observed" if feature in FEATURES[:2] else "excluded",
                        producer, sample_id, "fixture declaration")
        for feature in FEATURES
    )
    transform = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, center_x, 0, 0, 1)
    return MeshObservation(
        f"{role}-part", 1, topology,
        struct.pack("<24d", *(value for point in POSITIONS for value in point)),
        "centimetres", "unreal-left-handed-z-up", "row", transform, pose,
        acquired, producer, f"{role}-configuration", coverage, RECORD_LIMITS)


def write_run(root):
    topologies = {role: _topology(role) for role in ("fixed", "moving")}
    roles = {
        role: {
            "component_id": f"{role}-part",
            "component_generation": 1,
            "configuration_id": f"{role}-configuration",
            "subject_id": f"{role}-part",
            "stream_id": "assembly",
            "region_id": f"{role}-surface",
            "topology_id": topologies[role].identity,
            "triangle_ids": list(range(12)),
        }
        for role in ("fixed", "moving")
    }
    samples = []
    for index, moving_x in enumerate((140, 110, 100, 80, 140)):
        sample_id = f"step-{index:02d}"
        row = {
            "sample_id": sample_id,
            "fixed_bundle": f"{sample_id}-fixed",
            "moving_bundle": f"{sample_id}-moving",
            "acquired_seconds": index * 0.2,
            "frame_id": 100 + index,
            "fixed_revision": 10 + index,
            "moving_revision": 10 + index,
        }
        samples.append(row)
        for role, center_x in (("fixed", 0), ("moving", moving_x)):
            observation = _observation(role, topologies[role], sample_id, index, center_x)
            request = MeshRequest(
                sample_id, observation.component_id, observation.component_generation,
                observation.configuration_id, observation.pose, observation.acquired,
                observation.topology.identity)
            completion = MeshCompletion(
                request, "completed",
                ClockStamp("unreal-monotonic", observation.acquired.seconds +
                           (0.01 if role == "fixed" else 0.02)),
                "", observation)
            write_mesh_observation(root, row[f"{role}_bundle"], completion,
                                   limits=RECORD_LIMITS)
    manifest = {
        "format": "neutral_assembly_capture",
        "schema_version": 1,
        "roles": roles,
        "samples": samples,
        "controls": {"checks_passed": True, "peak_reserved_bytes": 10510240},
    }
    (root / "assembly.json").write_text(json.dumps(manifest), encoding="utf-8")
    criteria_path = root / "criteria.json"
    criteria_path.write_text(json.dumps(criteria()), encoding="utf-8")
    return manifest, criteria_path


def load_example(test):
    test.assertTrue(SCRIPT.is_file(), "neutral assembly example is not implemented")
    spec = importlib.util.spec_from_file_location("neutral_assembly_example", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hashes(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*")) if path.is_file()
    }


class NeutralAssemblyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest, self.criteria_path = write_run(self.root)

    def test_explicit_cube_replay_reports_literal_expected_results(self):
        example = load_example(self)
        before = hashes(self.root)

        report = example.evaluate_run(self.root, self.criteria_path)

        self.assertEqual(report["format"], "neutral_assembly_report")
        self.assertEqual(report["status"], "verified")
        self.assertEqual([row["minimum_distance"] for row in report["measurements"]],
                         EXPECTED_DISTANCES)
        self.assertEqual([row["surface_intersection"] for row in report["measurements"]],
                         EXPECTED_INTERSECTIONS)
        self.assertEqual(report["interval"]["between_samples"], "not_evaluated")
        self.assertEqual(report["read_errors"], [])
        self.assertTrue(all(row["matches"] for row in report["expectations"]))
        self.assertEqual(report["capture_controls"],
                         {"checks_passed": True, "peak_reserved_bytes": 10510240})
        self.assertEqual(
            set(report["controls"]),
            {"wrong_topology", "wrong_pose", "wrong_clock",
             "required_excluded_cloth", "exhausted_search",
             "missing_endpoint", "excessive_gap"})
        self.assertTrue(all(row["status"] == "insufficient"
                            for row in report["controls"].values()))
        expected_control_reasons = {
            "wrong_topology": "topology:mismatch",
            "wrong_pose": "pose:mismatch",
            "wrong_clock": "acquisition_mismatch",
            "required_excluded_cloth": "cloth:excluded",
            "exhausted_search": "work_limit:",
            "missing_endpoint": "missing_start_sample",
            "excessive_gap": "exceeds_max_gap",
        }
        for label, fragment in expected_control_reasons.items():
            self.assertTrue(any(fragment in reason
                                for reason in report["controls"][label]["reasons"]), label)
        self.assertEqual(len(report["completions"]), 5)
        first_completion = report["completions"][0]
        self.assertEqual(first_completion["fixed"]["acquired"],
                         first_completion["moving"]["acquired"])
        self.assertNotEqual(first_completion["fixed"]["completed"],
                            first_completion["moving"]["completed"])
        self.assertIn("assembly.json", report["source_hashes"])
        self.assertIn("criteria", report["source_hashes"])
        self.assertEqual(before, hashes(self.root), "evaluation or controls changed replay files")
        json.dumps(report, allow_nan=False)

    def test_missing_middle_bundle_stays_insufficient_when_surviving_gap_passes(self):
        example = load_example(self)
        (self.root / "step-02-moving" / "complete.json").unlink()

        report = example.evaluate_run(self.root, self.criteria_path)

        self.assertEqual(report["status"], "insufficient")
        self.assertEqual(report["interval"]["status"], "complete")
        self.assertTrue(all(not gap["exceeds_max_gap"] for gap in report["interval"]["gaps"]))
        self.assertEqual(report["read_errors"][0]["sample_id"], "step-02")
        missing = next(row for row in report["expectations"] if row["sample_id"] == "step-02")
        self.assertIsNone(missing["observed_distance"])
        self.assertFalse(missing["matches"])

    def test_role_and_sample_declarations_bind_loaded_records(self):
        example = load_example(self)
        cases = (
            ("configuration", lambda data: data["roles"]["fixed"].__setitem__(
                "configuration_id", "wrong-configuration")),
            ("topology", lambda data: data["roles"]["fixed"].__setitem__(
                "topology_id", "0" * 64)),
            ("pose", lambda data: data["samples"][0].__setitem__("fixed_revision", 999)),
        )
        for label, mutate in cases:
            with self.subTest(label=label):
                data = copy.deepcopy(self.manifest)
                mutate(data)
                (self.root / "assembly.json").write_text(json.dumps(data), encoding="utf-8")
                report = example.evaluate_run(self.root, self.criteria_path)
                self.assertEqual(report["status"], "insufficient")
                self.assertEqual(report["measurements"][0]["status"], "insufficient")
                self.assertIsNone(report["measurements"][0]["minimum_distance"])

    def test_strict_json_rejects_unknown_duplicate_and_nonfinite_nested_fields(self):
        example = load_example(self)
        bad_values = []
        unknown_limit = criteria()
        unknown_limit["analysis_limits"]["surprise"] = 1
        bad_values.append(json.dumps(unknown_limit))
        duplicate = json.dumps(criteria()).replace(
            '"max_samples": 8', '"max_samples": 8, "max_samples": 8')
        bad_values.append(duplicate)
        nonfinite = json.dumps(criteria()).replace('"distance": 40', '"distance": NaN', 1)
        bad_values.append(nonfinite)
        for text in bad_values:
            with self.subTest(text=text[:50]):
                self.criteria_path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    example.evaluate_run(self.root, self.criteria_path)
        manifest = copy.deepcopy(self.manifest)
        manifest["roles"]["fixed"]["unknown"] = True
        (self.root / "assembly.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.criteria_path.write_text(json.dumps(criteria()), encoding="utf-8")
        with self.assertRaises(ValueError):
            example.evaluate_run(self.root, self.criteria_path)

    def test_input_size_and_declared_work_bounds_are_enforced(self):
        example = load_example(self)
        self.criteria_path.write_bytes(b" " * 65537)
        with self.assertRaises(ValueError):
            example.evaluate_run(self.root, self.criteria_path)
        value = criteria()
        value["analysis_limits"]["max_pair_tests"] = 257
        self.criteria_path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(ValueError):
            example.evaluate_run(self.root, self.criteria_path)
        for bad_gap in (10**1000,):
            value = criteria()
            value["max_gap_seconds"] = bad_gap
            self.criteria_path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(ValueError):
                example.evaluate_run(self.root, self.criteria_path)

    def test_boolean_schema_versions_are_rejected(self):
        example = load_example(self)
        value = criteria()
        value["schema_version"] = True
        self.criteria_path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(ValueError):
            example.evaluate_run(self.root, self.criteria_path)
        self.criteria_path.write_text(json.dumps(criteria()), encoding="utf-8")
        manifest = copy.deepcopy(self.manifest)
        manifest["schema_version"] = True
        (self.root / "assembly.json").write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(ValueError):
            example.evaluate_run(self.root, self.criteria_path)

    def test_two_sample_profile_cannot_self_authorize_verification(self):
        example = load_example(self)
        manifest = copy.deepcopy(self.manifest)
        manifest["samples"] = manifest["samples"][:2]
        (self.root / "assembly.json").write_text(json.dumps(manifest), encoding="utf-8")
        value = criteria()
        value["expected_samples"] = value["expected_samples"][:2]
        self.criteria_path.write_text(json.dumps(value), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "expected_samples"):
            example.evaluate_run(self.root, self.criteria_path)
        output = self.root / "shortcut-report.json"
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            code = example.main(["--observations", str(self.root),
                                 "--criteria", str(self.criteria_path),
                                 "--output", str(output)])
        self.assertEqual(code, 2)
        self.assertIn("expected_samples", errors.getvalue())
        self.assertFalse(output.exists())

    def test_fixed_profile_rejects_altered_qualification_semantics(self):
        example = load_example(self)

        def set_nested(section, name, value):
            return lambda data: data[section].__setitem__(name, value)

        cases = (
            ("units", lambda data: data.__setitem__("units", "metres")),
            ("coordinate_system", lambda data: data.__setitem__("coordinate_system", "other")),
            ("vector_convention", lambda data: data.__setitem__("vector_convention", "column")),
            ("required_features", lambda data: data.__setitem__("required_features", ["rigid"])),
            ("known_features", lambda data: data["known_features"].append("invented")),
            ("allowed_exclusions", lambda data: data["allowed_exclusions"].remove("cloth")),
            ("allowed_producers", lambda data: data["allowed_producers"].append("synthetic-v1")),
            ("record_limits", set_nested("record_limits", "max_vertices", 63)),
            ("analysis_limits", set_nested("analysis_limits", "max_pair_tests", 255)),
            ("max_samples", lambda data: data.__setitem__("max_samples", 7)),
            ("max_gap_seconds", lambda data: data.__setitem__("max_gap_seconds", 2.0)),
            ("tolerance", lambda data: data.__setitem__("tolerance", 1.0)),
            ("expected_samples", lambda data: data["expected_samples"][0].__setitem__("distance", 41)),
        )
        for label, mutate in cases:
            with self.subTest(label=label):
                value = criteria()
                mutate(value)
                self.criteria_path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, label):
                    example.evaluate_run(self.root, self.criteria_path)

    def test_canonical_profile_is_semantic_not_json_byte_locked(self):
        example = load_example(self)
        value = criteria()
        for name in ("required_features", "known_features", "allowed_exclusions",
                     "allowed_producers"):
            value[name].reverse()
        self.criteria_path.write_text(json.dumps(value, indent=4, sort_keys=True), encoding="utf-8")

        report = example.evaluate_run(self.root, self.criteria_path)

        self.assertEqual(report["status"], "verified")
        self.assertNotEqual(report["source_hashes"]["criteria"],
                            hashlib.sha256(json.dumps(criteria()).encode()).hexdigest())

    def test_cli_exclusively_writes_reports_and_returns_nonzero_on_failure(self):
        example = load_example(self)
        output = self.root / "report.json"
        manifest = copy.deepcopy(self.manifest)
        manifest["controls"]["checks_passed"] = False
        (self.root / "assembly.json").write_text(json.dumps(manifest), encoding="utf-8")
        arguments = ["--observations", str(self.root), "--criteria", str(self.criteria_path),
                     "--output", str(output)]

        self.assertEqual(example.main(arguments), 1)
        self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "insufficient")
        original = output.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertNotEqual(example.main(arguments), 0)
        self.assertIn("File exists", errors.getvalue())
        self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
