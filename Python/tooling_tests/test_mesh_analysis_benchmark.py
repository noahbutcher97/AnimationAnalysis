"""Contract tests for the bounded mesh-analysis benchmark example."""
import contextlib
from fractions import Fraction
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

from animation_analysis import MeshAnalysisLimits, measure_mesh_pair


SCRIPT = Path(__file__).parents[1] / "examples" / "benchmark_mesh_analysis.py"
LIMITS = MeshAnalysisLimits(512, 2048, 8192, 256)


def load_example(test):
    test.assertTrue(SCRIPT.is_file(), "mesh-analysis benchmark is not implemented")
    spec = importlib.util.spec_from_file_location("mesh_analysis_benchmark", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MeshAnalysisBenchmarkTests(unittest.TestCase):
    def test_explicit_geometries_have_literal_exact_distances(self):
        example = load_example(self)
        cases = (
            ("overlapping_boxes", Fraction(2)),
            ("thin_triangles", Fraction(1048576, 1099511627777)),
            ("near_parallel", Fraction(1, 1099511627776)),
        )

        for name, expected in cases:
            with self.subTest(name=name):
                case = example.build_case(name, triangle_count=1)
                result = measure_mesh_pair(case.sample, tolerance=0.0, limits=LIMITS)
                self.assertEqual(result.status, "measured")
                self.assertEqual(result.minimum_squared_distance, expected)
                self.assertFalse(result.surface_intersection)

    def test_fixed_hard_workloads_complete_or_exhaust_at_literal_pair_counts(self):
        example = load_example(self)

        completed = measure_mesh_pair(
            example.build_case("overlapping_boxes", triangle_count=32).sample,
            tolerance=0.0,
            limits=LIMITS,
        )
        exhausted = measure_mesh_pair(
            example.build_case("thin_triangles", triangle_count=128).sample,
            tolerance=0.0,
            limits=LIMITS,
        )

        self.assertEqual(completed.status, "measured")
        self.assertEqual(completed.stats.triangle_tests, 1024)
        self.assertEqual(completed.minimum_squared_distance, Fraction(2))
        self.assertEqual(exhausted.status, "insufficient")
        self.assertEqual(exhausted.stats.triangle_tests, 2048)
        self.assertTrue(any(reason.startswith("work_limit:") for reason in exhausted.reasons))
        self.assertIsNone(exhausted.minimum_distance)
        self.assertIsNone(exhausted.minimum_squared_distance)

    def test_near_parallel_large_fixture_prunes_after_one_pair(self):
        example = load_example(self)

        result = measure_mesh_pair(
            example.build_case("near_parallel", triangle_count=512).sample,
            tolerance=0.0,
            limits=LIMITS,
        )

        self.assertEqual(result.status, "measured")
        self.assertEqual(result.stats.triangle_tests, 1)
        self.assertEqual(result.minimum_squared_distance, Fraction(1, 1099511627776))

    def test_fixture_identity_and_rotated_execution_orders_are_deterministic(self):
        example = load_example(self)
        first = measure_mesh_pair(
            example.build_case("thin_triangles", triangle_count=4).sample,
            tolerance=0.0,
            limits=LIMITS,
        )
        second = measure_mesh_pair(
            example.build_case("thin_triangles", triangle_count=4).sample,
            tolerance=0.0,
            limits=LIMITS,
        )

        self.assertEqual(first.first.observation_identity, second.first.observation_identity)
        self.assertEqual(first.second.observation_identity, second.second.observation_identity)
        self.assertEqual(first.first.region_identity, second.first.region_identity)
        self.assertEqual(first.second.region_identity, second.second.region_identity)
        base = [
            (name, count)
            for name in ("overlapping_boxes", "thin_triangles", "near_parallel")
            for count in (32, 128, 512)
        ]
        orders = example.execution_orders()
        self.assertEqual(orders["warmup"], base)
        self.assertEqual(orders["repetitions"][0], base)
        self.assertEqual(orders["repetitions"][1], base[3:] + base[:3])
        self.assertEqual(orders["repetitions"][2], base[6:] + base[:6])

    def test_aggregates_keep_completed_and_exhausted_measurements_separate(self):
        example = load_example(self)
        rows = [
            {"case": "thin_triangles", "triangles_per_side": 128,
             "outcome": "completed", "elapsed_seconds": 3.0},
            {"case": "thin_triangles", "triangles_per_side": 128,
             "outcome": "completed", "elapsed_seconds": 1.0},
            {"case": "thin_triangles", "triangles_per_side": 128,
             "outcome": "budget_exhausted", "elapsed_seconds": 4.0},
        ]

        summary = example.aggregate_measurements(rows)[0]

        self.assertEqual(summary["completed"]["elapsed_seconds"],
                         {"count": 2, "minimum": 1.0, "median": 2.0, "maximum": 3.0})
        self.assertEqual(summary["budget_exhausted"]["elapsed_seconds"],
                         {"count": 1, "minimum": 4.0, "median": 4.0, "maximum": 4.0})
        self.assertNotIn("minimum_distance", summary["budget_exhausted"])

    def test_existing_output_directory_is_rejected_without_changes(self):
        example = load_example(self)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "existing"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("original", encoding="utf-8")

            with contextlib.redirect_stderr(io.StringIO()) as errors:
                code = example.main(["--output", str(output)])

            self.assertEqual(code, 2)
            self.assertIn("already exists", errors.getvalue())
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "original")
            self.assertEqual(list(output.iterdir()), [sentinel])


if __name__ == "__main__":
    unittest.main()
