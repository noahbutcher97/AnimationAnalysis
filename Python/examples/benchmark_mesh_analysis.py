"""Run fixed, bounded stress measurements through the public mesh-analysis API."""
import argparse
from dataclasses import dataclass
from fractions import Fraction
import hashlib
from importlib import metadata
import json
from pathlib import Path
import platform
import shutil
import statistics
import struct
import sys
import time

from animation_analysis import (
    ClockStamp,
    FeatureCoverage,
    MeshAnalysisLimits,
    MeshObservation,
    MeshPairSample,
    MeshRecordLimits,
    MeshRegion,
    MeshRequirement,
    MeshSection,
    MeshSelection,
    MeshTopology,
    PoseKey,
    measure_mesh_pair,
)


CASE_ORDER = ("overlapping_boxes", "thin_triangles", "near_parallel")
TRIANGLE_COUNTS = (32, 128, 512)
REPETITIONS = 3
TOLERANCE = 0.0
LIMITS = MeshAnalysisLimits(
    max_triangles=512,
    max_pair_tests=2048,
    max_node_visits=8192,
    max_coordinate_bits=256,
)
RECORD_LIMITS = MeshRecordLimits(3, 1536, 1, 8192, 65536, 73728)
IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
PRODUCER_ID = "benchmark-synthetic-v1"
GEOMETRIES = {
    "overlapping_boxes": (
        ((0, 0, 0), (2, 0, 0), (0, 2, 0)),
        ((2, 2, 0), (3, 2, 0), (2, 3, 0)),
    ),
    "thin_triangles": (
        ((0, 0, 0), (1024, 0, 0), (0, Fraction(1, 1024), 0)),
        ((1024, Fraction(1, 1024), 0),
         (2048, Fraction(1, 1024), 0),
         (1024, Fraction(2, 1024), 0)),
    ),
    "near_parallel": (
        ((0, 0, 0), (2, 0, 0), (0, 2, 0)),
        ((0, 0, Fraction(1, 1048576)),
         (2, 0, Fraction(1, 1048576)),
         (0, 2, Fraction(1, 1048576))),
    ),
}
EXPECTED_SQUARED = {
    "overlapping_boxes": Fraction(2),
    "thin_triangles": Fraction(1048576, 1099511627777),
    "near_parallel": Fraction(1, 1099511627776),
}


@dataclass(frozen=True)
class BenchmarkCase:
    name: str
    triangle_count: int
    sample: MeshPairSample
    expected_minimum_squared_distance: Fraction
    expected_surface_intersection: bool
    expected_outcome: str
    expected_triangle_tests: int


def _rational(value):
    value = Fraction(value)
    return {"numerator": str(value.numerator), "denominator": str(value.denominator)}


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _observation(case_name, side, points, triangle_count):
    index_data = struct.pack("<" + "I" * (3 * triangle_count),
                             *(index for _ in range(triangle_count)
                               for index in (0, 1, 2)))
    topology = MeshTopology(
        f"{case_name}-{side}-triangle",
        0,
        0,
        3,
        index_data,
        (MeshSection("surface", 0, 3 * triangle_count, "synthetic"),),
        RECORD_LIMITS,
    )
    component_id = f"{case_name}-{side}"
    pose = PoseKey(component_id, "benchmark", 0, 0)
    acquired = ClockStamp("synthetic", 0)
    observation = MeshObservation(
        component_id,
        0,
        topology,
        struct.pack("<9d", *(float(value) for point in points for value in point)),
        "unitless",
        "shared-cartesian",
        "row",
        IDENTITY,
        pose,
        acquired,
        PRODUCER_ID,
        f"{case_name}-configuration",
        (FeatureCoverage("rigid", "observed", PRODUCER_ID,
                         f"{case_name}-{side}-explicit-geometry", ""),),
        RECORD_LIMITS,
    )
    region = MeshRegion("surface", topology.identity,
                        tuple(range(triangle_count)), LIMITS)
    requirement = MeshRequirement(
        component_id,
        0,
        topology.identity,
        observation.configuration_id,
        pose,
        observation.units,
        observation.coordinate_system,
        ("rigid",),
        ("rigid",),
        (),
        (PRODUCER_ID,),
    )
    return MeshSelection(observation, region, requirement)


def build_case(name, *, triangle_count):
    """Build one explicit repeated-triangle fixture outside the timed interval."""
    if name not in GEOMETRIES:
        raise ValueError(f"Unknown benchmark case: {name}")
    if type(triangle_count) is not int or not 1 <= triangle_count <= 512:
        raise ValueError("triangle_count must be an integer in 1..512")
    first_points, second_points = GEOMETRIES[name]
    first = _observation(name, "a", first_points, triangle_count)
    second = _observation(name, "b", second_points, triangle_count)
    sample = MeshPairSample(
        f"{name}-{triangle_count}", ClockStamp("synthetic", 0), first, second)
    exhaustive_pairs = triangle_count * triangle_count
    expected_tests = 1 if name == "near_parallel" else min(
        exhaustive_pairs, LIMITS.max_pair_tests)
    expected_outcome = (
        "completed"
        if name == "near_parallel" or exhaustive_pairs <= LIMITS.max_pair_tests
        else "budget_exhausted"
    )
    return BenchmarkCase(
        name,
        triangle_count,
        sample,
        EXPECTED_SQUARED[name],
        False,
        expected_outcome,
        expected_tests,
    )


def execution_orders():
    """Return the fixed warmup and measured orders without changing geometry."""
    base = [(name, count) for name in CASE_ORDER for count in TRIANGLE_COUNTS]
    repetitions = []
    for repetition in range(REPETITIONS):
        offset = 3 * repetition
        repetitions.append(base[offset:] + base[:offset])
    return {"warmup": base, "repetitions": repetitions}


def _outcome(result):
    if result.status == "measured":
        return "completed"
    if result.status == "insufficient" and any(
            reason.startswith("work_limit:") for reason in result.reasons):
        return "budget_exhausted"
    return "insufficient"


def _measure(case, *, phase, repetition, execution_index):
    started = time.perf_counter()
    result = measure_mesh_pair(case.sample, tolerance=TOLERANCE, limits=LIMITS)
    elapsed = time.perf_counter() - started
    outcome = _outcome(result)
    checks = {
        "outcome": outcome == case.expected_outcome,
        "triangle_tests": result.stats.triangle_tests == case.expected_triangle_tests,
        "no_numeric_minimum_when_exhausted": (
            outcome != "budget_exhausted"
            or (result.minimum_distance is None
                and result.minimum_squared_distance is None)
        ),
        "exact_minimum_when_completed": (
            outcome != "completed"
            or result.minimum_squared_distance
            == case.expected_minimum_squared_distance
        ),
        "intersection_when_completed": (
            outcome != "completed"
            or result.surface_intersection == case.expected_surface_intersection
        ),
    }
    return {
        "phase": phase,
        "repetition": repetition,
        "execution_index": execution_index,
        "case": case.name,
        "triangles_per_side": case.triangle_count,
        "elapsed_seconds": elapsed,
        "analysis_elapsed_seconds": result.stats.elapsed_seconds,
        "outcome": outcome,
        "expected": {
            "outcome": case.expected_outcome,
            "minimum_squared_distance": _rational(
                case.expected_minimum_squared_distance),
            "surface_intersection": case.expected_surface_intersection,
            "triangle_tests": case.expected_triangle_tests,
        },
        "checks": checks,
        "result": result.to_mapping(),
    }


def _distribution(values):
    if not values:
        return {"count": 0, "minimum": None, "median": None, "maximum": None}
    return {
        "count": len(values),
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
    }


def aggregate_measurements(rows):
    """Summarize wall time per geometry and size, split by terminal outcome."""
    keys = []
    for row in rows:
        key = (row["case"], row["triangles_per_side"])
        if key not in keys:
            keys.append(key)
    summaries = []
    for name, count in keys:
        group = [row for row in rows
                 if (row["case"], row["triangles_per_side"]) == (name, count)]
        summary = {"case": name, "triangles_per_side": count}
        for outcome in ("completed", "budget_exhausted"):
            summary[outcome] = {
                "elapsed_seconds": _distribution(
                    [row["elapsed_seconds"] for row in group
                     if row["outcome"] == outcome])
            }
        summary["unexpected_count"] = sum(
            row["outcome"] not in ("completed", "budget_exhausted")
            for row in group)
        summaries.append(summary)
    return summaries


def _portable_inputs():
    return {
        "format": "mesh_analysis_benchmark_inputs",
        "schema_version": 1,
        "construction": (
            "Each side repeats one explicitly listed triangle with unique triangle ordinals."
        ),
        "cases": [
            {
                "name": name,
                "first_triangle": [[_rational(value) for value in point]
                                   for point in GEOMETRIES[name][0]],
                "second_triangle": [[_rational(value) for value in point]
                                    for point in GEOMETRIES[name][1]],
                "expected_minimum_squared_distance": _rational(EXPECTED_SQUARED[name]),
                "expected_surface_intersection": False,
            }
            for name in CASE_ORDER
        ],
        "recipe": {
            "case_order": list(CASE_ORDER),
            "triangle_counts_per_side": list(TRIANGLE_COUNTS),
            "warmup_sequences": 1,
            "measured_repetitions": REPETITIONS,
            "measured_left_rotation_entries": [0, 3, 6],
            "fixture_construction_is_timed": False,
            "timed_operation": "complete public measure_mesh_pair call",
        },
    }


def _source_hashes():
    benchmark_source = Path(__file__).resolve()
    analysis_source = Path(measure_mesh_pair.__code__.co_filename).resolve()
    return {
        "benchmark_script": {"path": str(benchmark_source),
                             "sha256": _sha256(benchmark_source)},
        "installed_mesh_analysis_module": {"path": str(analysis_source),
                                           "sha256": _sha256(analysis_source)},
    }


def _input_hashes(rows):
    hashes = {}
    for row in rows:
        key = f'{row["case"]}/{row["triangles_per_side"]}'
        result = row["result"]
        hashes[key] = {
            "first_observation": result["first"]["observation_identity"],
            "second_observation": result["second"]["observation_identity"],
            "first_topology": result["first"]["topology_id"],
            "second_topology": result["second"]["topology_id"],
            "first_region": result["first"]["region_identity"],
            "second_region": result["second"]["region_identity"],
        }
    return hashes


def run_benchmarks(output):
    """Run the fixed benchmark recipe and publish it to a new output directory."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    cases = {(name, count): build_case(name, triangle_count=count)
             for name in CASE_ORDER for count in TRIANGLE_COUNTS}
    orders = execution_orders()
    warmups = [
        _measure(cases[key], phase="warmup", repetition=None,
                 execution_index=index)
        for index, key in enumerate(orders["warmup"])
    ]
    measurements = []
    for repetition, order in enumerate(orders["repetitions"]):
        measurements.extend(
            _measure(cases[key], phase="measured", repetition=repetition,
                     execution_index=index)
            for index, key in enumerate(order)
        )

    portable_inputs = _portable_inputs()
    portable_path = output / "portable-inputs.json"
    portable_path.write_text(json.dumps(portable_inputs, indent=2, allow_nan=False) + "\n",
                             encoding="utf-8")
    archived_source = output / "benchmark_mesh_analysis.py"
    shutil.copyfile(Path(__file__), archived_source)
    source_hashes = _source_hashes()
    source_hashes["archived_benchmark_script"] = {
        "path": archived_source.name,
        "sha256": _sha256(archived_source),
    }
    report = {
        "format": "mesh_analysis_benchmark_report",
        "schema_version": 1,
        "status": "verified" if all(
            all(row["checks"].values()) for row in warmups + measurements) else "unexpected",
        "scope": "narrow deterministic synthetic stress controls",
        "claims": {
            "representative_game_mesh": False,
            "realtime": False,
            "worst_case_wall_time": False,
        },
        "environment": {
            "executable": sys.executable,
            "python_version": platform.python_version(),
            "python_implementation": platform.python_implementation(),
            "python_compiler": platform.python_compiler(),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "package_version": metadata.version("animation-analysis"),
        },
        "settings": {
            "tolerance": TOLERANCE,
            "limits": {
                "max_triangles": LIMITS.max_triangles,
                "max_pair_tests": LIMITS.max_pair_tests,
                "max_node_visits": LIMITS.max_node_visits,
                "max_coordinate_bits": LIMITS.max_coordinate_bits,
            },
        },
        "recipe": portable_inputs["recipe"] | {"execution_orders": orders},
        "portable_inputs": {
            "path": portable_path.name,
            "sha256": _sha256(portable_path),
            "content": portable_inputs,
        },
        "source_hashes": source_hashes,
        "input_hashes": _input_hashes(warmups),
        "warmups": warmups,
        "measurements": measurements,
        "aggregates": aggregate_measurements(measurements),
    }
    (output / "report.json").write_text(
        json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run bounded deterministic mesh-analysis stress measurements.")
    parser.add_argument("--output", required=True,
                        help="new directory for report and reproducibility artifacts")
    try:
        arguments = parser.parse_args(argv)
    except SystemExit as error:
        return int(error.code)
    try:
        report = run_benchmarks(arguments.output)
    except (OSError, ValueError) as error:
        print(f"benchmark_mesh_analysis: {error}", file=sys.stderr)
        return 2
    return 0 if report["status"] == "verified" else 1


if __name__ == "__main__":
    raise SystemExit(main())
