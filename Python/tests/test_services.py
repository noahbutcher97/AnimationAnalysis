"""Neutral evidence controls for the shared service boundary; no image extra."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from animation_analysis.artifacts import atomic_json, atomic_text, file_manifest, identity
from animation_analysis.contracts import ClockStamp
from animation_analysis.errors import EvidenceError
from animation_analysis.integrity import png_dimensions, read_json, read_lines
from animation_analysis.metrics import distribution, numeric_deltas, summarize_statuses
from animation_analysis.temporal import TimeInterval, bracket_observations, event_window


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(scanlines=b"\0\xff\0\0", compressed=None):
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines) if compressed is None else compressed)
            + chunk(b"IEND", b""))


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_failed_replace_preserves_last_complete_artifact_and_cleans_temporary(self):
        output = self.root / "review.json"
        atomic_json(output, {"status": "inconclusive"})
        previous = output.read_bytes()
        with patch("animation_analysis.artifacts.os.replace", side_effect=OSError("writer unavailable")):
            with self.assertRaises(OSError):
                atomic_json(output, {"status": "pass"})
        self.assertEqual(output.read_bytes(), previous)
        self.assertEqual(list(self.root.iterdir()), [output])

    def test_serialization_failure_cannot_publish_nonfinite_result(self):
        output = self.root / "measurement.json"
        atomic_text(output, "previous")
        with self.assertRaises(ValueError):
            atomic_json(output, {"distance": float("nan")})
        self.assertEqual(output.read_text(), "previous")
        self.assertEqual(list(self.root.iterdir()), [output])

    def test_explicit_manifest_is_relocatable_and_ignores_unselected_output(self):
        for name in ("one", "two"):
            directory = self.root / name
            directory.mkdir()
            (directory / "sensor.data").write_bytes(b"observation")
        first = file_manifest(self.root / "one", ["sensor.data"])
        self.assertEqual(first, file_manifest(self.root / "two", ["sensor.data"]))
        (self.root / "one" / "derived.json").write_text("{}")
        self.assertEqual(first, file_manifest(self.root / "one", ["sensor.data"]))
        (self.root / "one" / "sensor.data").write_bytes(b"different observation")
        self.assertNotEqual(first, file_manifest(self.root / "one", ["sensor.data"]))

    def test_manifest_rejects_escape_duplicate_absolute_and_missing_inputs(self):
        (self.root / "sensor.data").write_bytes(b"observation")
        for names in (["../sensor.data"], ["sensor.data", "./sensor.data"], [self.root / "sensor.data"]):
            with self.subTest(names=names), self.assertRaises(EvidenceError):
                file_manifest(self.root, names)
        with self.assertRaises(OSError):
            file_manifest(self.root, ["absent.data"])

    def test_canonical_identity_is_order_independent_and_content_sensitive(self):
        self.assertEqual(identity({"a": 1, "b": 2}), identity({"b": 2, "a": 1}))
        self.assertNotEqual(identity({"a": 1}), identity({"a": 2}))
        with self.assertRaises(ValueError):
            identity({"a": float("inf")})

    def test_json_and_lines_reject_nonfinite_constants_and_exponent_overflow(self):
        path = self.root / "observations.json"
        for bad in ("NaN", "Infinity", "-Infinity", "1e999"):
            path.write_text('{"value":' + bad + '}')
            for reader in (read_json, read_lines):
                with self.subTest(bad=bad, reader=reader), self.assertRaises(EvidenceError):
                    reader(path)
        path.write_text('\ufeff{"name":"moving part","value":1.25}\n\n', encoding="utf-8")
        self.assertEqual(read_lines(path), [read_json(path)])

    def test_png_crc_compression_and_scanlines_are_checked_without_decoder_extra(self):
        path = self.root / "frame.png"
        path.write_bytes(png())
        self.assertEqual(png_dimensions(path), (1, 1))
        corrupt_crc = bytearray(png())
        corrupt_crc[-1] ^= 1
        for payload in (bytes(corrupt_crc), png()[:-1], png() + b"extra", png(b"\5\xff\0\0"),
                        png(b"\0\xff"), png(b"\0\xff\0\0\0"), png(compressed=b"bad deflate")):
            path.write_bytes(payload)
            with self.subTest(payload=payload), self.assertRaises(EvidenceError):
                png_dimensions(path)


class TemporalTests(unittest.TestCase):
    def test_named_window_and_complete_bracketing_use_caller_selected_clock_and_records(self):
        stamp = lambda seconds: ClockStamp("robot_sensor", seconds)
        window = event_window([("engage", stamp(2)), ("release", stamp(3))], "engage", "release", .1, .2)
        rows = [{"acquired": stamp(value), "part": "clamp"} for value in (1, 1.8, 2.5, 3.3, 4)]
        selected = bracket_observations(rows, window, lambda row: row["acquired"])
        self.assertEqual(selected, rows[1:4])
        self.assertEqual(window.start, stamp(1.9))
        self.assertEqual(window.end, stamp(3.2))

    def test_missing_coverage_is_not_silently_shortened(self):
        window = TimeInterval(ClockStamp("sensor", 1), ClockStamp("sensor", 2))
        for rows in ([], [.9, 1.5], [1.5, 2.1]):
            with self.subTest(rows=rows), self.assertRaisesRegex(EvidenceError, "bracket"):
                bracket_observations(rows, window, lambda value: ClockStamp("sensor", value))

    def test_clock_mismatch_and_unordered_observations_are_rejected(self):
        with self.assertRaisesRegex(EvidenceError, "clock"):
            TimeInterval(ClockStamp("sensor", 1), ClockStamp("wall", 2))
        window = TimeInterval(ClockStamp("sensor", 1), ClockStamp("sensor", 2))
        with self.assertRaisesRegex(EvidenceError, "clock"):
            bracket_observations([0, 3], window, lambda value: ClockStamp("wall", value))
        with self.assertRaisesRegex(EvidenceError, "ordered"):
            bracket_observations([0, 3, 2], window, lambda value: ClockStamp("sensor", value))

    def test_ambiguous_events_invalid_padding_and_reversed_windows_are_rejected(self):
        events = [("start", ClockStamp("sensor", 1)), ("end", ClockStamp("sensor", 2))]
        for records, first, last, padding in ((events[:1], "start", "end", 0),
                                             (events + events[:1], "start", "end", 0),
                                             (events, "end", "start", 0),
                                             (events, "start", "end", -1)):
            with self.subTest(records=records, padding=padding), self.assertRaises(EvidenceError):
                event_window(records, first, last, padding)


class MetricTests(unittest.TestCase):
    def test_numeric_comparison_ignores_flags_and_missing_values(self):
        current = {"part": {"distance": 8, "visible": True}, "missing": 4, "unknown": None}
        baseline = {"part": {"distance": 3, "visible": False}, "unknown": 1}
        self.assertEqual(numeric_deltas(current, baseline), {"part.distance": {"current": 8, "baseline": 3, "delta": 5}})
        with self.assertRaises(EvidenceError):
            numeric_deltas({"x": float("inf")}, {"x": 1})

    def test_distribution_handles_empty_and_rejects_nonnumeric_observations(self):
        self.assertEqual(distribution([]), {"count": 0, "min": None, "median": None, "max": None})
        self.assertEqual(distribution([8, 2, 5]), {"count": 3, "min": 2, "median": 5, "max": 8})
        with self.assertRaises(EvidenceError):
            distribution([True])

    def test_status_precedence_preserves_inconclusive_and_unexecuted_semantics(self):
        self.assertEqual(summarize_statuses([]), "not_run")
        self.assertEqual(summarize_statuses(["pass", "not_run"]), "pass")
        self.assertEqual(summarize_statuses(["pass", "inconclusive"]), "inconclusive")
        self.assertEqual(summarize_statuses(["inconclusive", "fail"]), "fail")
        with self.assertRaises(EvidenceError):
            summarize_statuses(["unrecognized"])
