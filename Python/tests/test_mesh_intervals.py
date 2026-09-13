"""Sample evidence controls; no interpolation or continuous intersection claims."""
from dataclasses import FrozenInstanceError, replace
from fractions import Fraction
import json
import unittest

from animation_analysis import ClockStamp, PoseKey
from animation_analysis.temporal import TimeInterval
from animation_analysis.mesh_analysis import measure_mesh_pair
from animation_analysis.mesh_intervals import summarize_mesh_interval
from mesh_analysis_fixtures import LIMITS, observation, pair, selection


def measured(time, *, sample_id=None, height=3, region='surface', required=('bone',), **changes):
    pose = PoseKey('a', 'capture', max(0, int(time)) if abs(time) < 100 else 0, 0)
    stamp = ClockStamp('simulation', time)
    first = selection(observation(acquired=stamp, pose=pose, **changes), region_id=region, required=required)
    second = selection(observation('b', ((0,0,height), (2,0,height), (0,2,height)),
                                   acquired=stamp, pose=replace(pose, subject_id='b')))
    return measure_mesh_pair(pair(first, second, sample_id=sample_id or str(time), time=time),
                             tolerance=1, limits=LIMITS)


def summarize(results, start=0, end=1, gap=.5, **kwargs):
    return summarize_mesh_interval(results, TimeInterval(ClockStamp('simulation', start),
                                                        ClockStamp('simulation', end)),
                                   max_gap_seconds=gap, max_samples=kwargs.get('max_samples', 10))


class MeshIntervalTests(unittest.TestCase):
    def test_complete_samples_report_observed_events_and_minimum(self):
        rows = [measured(0), measured(.5, height=0), measured(1, height=.5)]
        summary = summarize(rows)
        self.assertEqual(summary.status, 'complete')
        self.assertEqual(summary.sample_count, 3)
        self.assertEqual(summary.measured_sample_count, 3)
        self.assertEqual(summary.sampled_intersection_count, 1)
        self.assertEqual(summary.sampled_within_tolerance_count, 2)
        self.assertEqual(summary.sampled_minimum_distance, 0)
        self.assertEqual(summary.sampled_minimum_squared_distance, 0)
        self.assertEqual([g.seconds for g in summary.gaps], [.5, .5])
        self.assertEqual(summary.to_mapping()['results'], [r.to_mapping() for r in rows])
        self.assertEqual(summary.to_mapping()['between_samples'], 'not_evaluated')
        json.dumps(summary.to_mapping(), allow_nan=False)
        rows.clear()
        self.assertEqual(summary.sample_count, 3)
        with self.assertRaises(FrozenInstanceError):
            summary.status = 'complete'

    def test_missing_middle_or_strict_gap_preserves_partial_observations(self):
        for times, gap in (((0, 1), .5), ((0, .5, 1), .25)):
            with self.subTest(times=times):
                summary = summarize([measured(t) for t in times], gap=gap)
                self.assertEqual(summary.status, 'insufficient')
                self.assertTrue(summary.aggregate_available)
                self.assertEqual(summary.sampled_minimum_distance, 3)
                self.assertTrue(any(g.exceeds_max_gap for g in summary.gaps))

    def test_insufficient_measurement_retained_and_excluded_from_counts(self):
        rows = [measured(0), measured(.5, required=('bone', 'cloth')), measured(1)]
        summary = summarize(rows)
        self.assertEqual(summary.status, 'insufficient')
        # Consistent required cloth still gives no numeric verdict at any sample.
        summary = summarize([measured(t, required=('bone', 'cloth')) for t in (0, .5, 1)])
        self.assertEqual(summary.measured_sample_count, 0)
        self.assertIsNone(summary.sampled_minimum_distance)
        self.assertEqual(summary.to_mapping()['results'][1]['status'], 'insufficient')
        rows[1] = measured(.5, points=((0,0,0), (1,0,0), (2,0,0)))
        partial = summarize(rows)
        self.assertEqual(partial.status, 'insufficient')
        self.assertEqual(partial.measured_sample_count, 2)
        self.assertEqual(partial.sampled_minimum_distance, 3)

    def test_empty_and_missing_endpoints_never_shrink_requested_interval(self):
        for times in ((), (.25, .5, 1), (0, .5, .75)):
            summary = summarize([measured(t) for t in times])
            self.assertEqual(summary.status, 'insufficient')
            self.assertEqual(summary.interval.start.seconds, 0)
            self.assertEqual(summary.interval.end.seconds, 1)

    def test_duplicates_reversed_unrelated_and_outside_are_insufficient(self):
        cases = ([measured(0), measured(1, sample_id='0')],
                 [measured(0), measured(0, sample_id='duplicate-time'), measured(1)],
                 [measured(1), measured(0)],
                 [measured(0), replace(measured(1), acquired=ClockStamp('other', 1))],
                 [measured(-.1), measured(0), measured(1)],
                 [measured(0), measured(1), measured(1.1)])
        for rows in cases:
            with self.subTest(rows=[r.sample_id for r in rows]):
                summary = summarize(rows, gap=2)
                self.assertEqual(summary.status, 'insufficient')
                self.assertEqual(summary.to_mapping()['results'], [r.to_mapping() for r in rows])

    def test_changed_pair_criteria_suppress_all_aggregate_measurements(self):
        variants = [measured(1, region='other'), measured(1, units='metres'),
                    measured(1, configuration_id='config-2'), measured(1, required=('bone', 'cloth')),
                    replace(measured(1), tolerance=2)]
        original = measured(1)
        variants.extend(replace(original, first=replace(original.first, **change)) for change in
                        ({'criteria_identity': 'changed'}, {'region_identity': 'changed'},
                         {'pose': replace(original.first.pose, stream_id='other')},
                         {'component_generation': 1}))
        for changed in variants:
            with self.subTest(changed=changed):
                summary = summarize([measured(0), changed], gap=1)
                self.assertEqual(summary.status, 'insufficient')
                self.assertFalse(summary.aggregate_available)
                self.assertIsNone(summary.measured_sample_count)
                self.assertIsNone(summary.sampled_intersection_count)
                self.assertIsNone(summary.sampled_within_tolerance_count)
                self.assertIsNone(summary.sampled_minimum_distance)
                self.assertEqual(len(summary.results), 2)

    def test_pose_revision_and_positions_can_change(self):
        rows = [measured(0), measured(1, height=2)]
        self.assertNotEqual(rows[0].second.observation_identity, rows[1].second.observation_identity)
        self.assertEqual(summarize(rows, gap=1).status, 'complete')

    def test_gap_comparison_uses_exact_stored_clock_numbers(self):
        summary = summarize([measured(-2**-54), measured(1)], start=-2**-54, gap=1)
        self.assertEqual(summary.status, 'insufficient')
        self.assertTrue(summary.gaps[0].exceeds_max_gap)
        self.assertGreater(summary.gaps[0].exact_seconds, Fraction(1))

    def test_overflowing_gap_remains_exact_and_json_safe(self):
        summary = summarize([measured(-1e308), measured(1e308)], start=-1e308, end=1e308, gap=1e308)
        self.assertEqual(summary.status, 'insufficient')
        self.assertIsNone(summary.gaps[0].seconds)
        self.assertGreater(summary.gaps[0].exact_seconds, Fraction(1e308))
        json.dumps(summary.to_mapping(), allow_nan=False)

    def test_explicit_bounds_reject_bad_arguments_before_consuming(self):
        for rows in ((r for r in ()), 'rows', {}, [None], [None]*11):
            with self.assertRaises(ValueError):
                summarize(rows)
        for gap in (0, -1, True, float('inf'), float('nan'), 10**1000):
            with self.assertRaises(ValueError):
                summarize([], gap=gap)
        for count in (0, -1, True, 1.5, 2**53):
            with self.assertRaises(ValueError):
                summarize([], max_samples=count)


if __name__ == '__main__':
    unittest.main()
