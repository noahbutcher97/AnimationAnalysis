import dataclasses
from fractions import Fraction
import json
import math
import tempfile
import unittest
import weakref
import gc

from animation_analysis import (ClockStamp, FeatureCoverage, MeshCompletion,
                               MeshRequest, PoseKey, read_mesh_observation,
                               write_mesh_observation)
from animation_analysis import MeshAnalysisLimits, MeshRegion, measure_mesh_pair
from mesh_analysis_fixtures import IDENTITY, LIMITS, RECORD_LIMITS, observation, pair, selection


class MeshAnalysisTests(unittest.TestCase):
    def measure(self, sample=None, **changes):
        return measure_mesh_pair(sample or pair(), **(dict(tolerance=0, limits=LIMITS) | changes))

    def test_parallel_surfaces_measure_global_distance_and_preserve_witnesses(self):
        result = self.measure()
        self.assertEqual(result.status, 'measured')
        self.assertEqual(result.minimum_squared_distance, 9)
        self.assertEqual(result.minimum_distance, 3)
        self.assertEqual(math.dist(result.first_point, result.second_point), 3)
        self.assertEqual((result.first_triangle, result.second_triangle), (0, 0))
        self.assertFalse(result.surface_intersection)
        self.assertFalse(result.within_tolerance)
        self.assertEqual(result.containment, 'not_evaluated')
        document = result.to_mapping()
        self.assertEqual(document['minimum_squared_distance'], {'numerator': '9', 'denominator': '1'})
        json.dumps(document, allow_nan=False)
        self.assertFalse(hasattr(result.first, 'position_data'))
        self.assertEqual(result.first.pose, PoseKey('a', 'capture', 0, 0))

    def test_edge_piercing_triangle_interior_is_an_intersection(self):
        b = observation('b', ((.5,.5,-1), (.5,.5,1), (.5,1.5,0)))
        result = self.measure(pair(second=selection(b)))
        self.assertEqual(result.status, 'measured')
        self.assertEqual(result.minimum_distance, 0)
        self.assertTrue(result.surface_intersection)

    def test_proximity_does_not_turn_small_nonzero_gap_into_intersection(self):
        gap = 2.0**-40
        b = observation('b', ((0,0,gap), (2,0,gap), (0,2,gap)))
        result = self.measure(pair(second=selection(b)), tolerance=2*gap)
        self.assertEqual(result.minimum_distance, gap)
        self.assertTrue(result.within_tolerance)
        self.assertFalse(result.surface_intersection)

    def test_exact_square_underflow_keeps_representable_positive_distance(self):
        e = 2.0**-200
        a = observation('a', ((0,0,0), (1,e,0), (0,1,e)))
        b = observation('b', ((e,.5,e/2), (e,.5,1), (e,1,1)))
        result = self.measure(pair(selection(a), selection(b)))
        self.assertEqual(result.status, 'measured')
        q = Fraction(1, 2**200)
        self.assertEqual(result.minimum_squared_distance, q**6/(1+q*q+q**4))
        self.assertGreater(result.minimum_distance, 0)
        self.assertAlmostEqual(result.minimum_distance/(2.0**-600), 1)
        self.assertFalse(result.surface_intersection)

    def test_region_selection_changes_measurement_without_anatomical_inference(self):
        a = observation('a', ((0,0,0),(2,0,0),(0,2,0),(0,0,10),(2,0,10),(0,2,10)),
                        ((0,1,2),(3,4,5)))
        self.assertEqual(self.measure(pair(first=selection(a))).minimum_distance, 3)
        selected = selection(a, (1,))
        self.assertEqual(self.measure(pair(first=selected)).minimum_distance, 7)
        self.assertEqual(self.measure(pair(first=selected)).first_triangle, 1)
        self.assertNotEqual(selected.region.identity, selection(a).region.identity)

    def test_regions_normalize_order_and_reject_duplicates_or_excess_before_copy(self):
        a = observation()
        x = MeshRegion('patch', a.topology.identity, [4, 2], LIMITS)
        y = MeshRegion('patch', a.topology.identity, [2, 4], LIMITS)
        self.assertEqual(x.identity, y.identity)
        for indices in ([], [0, 0], [-1], [True], range(3)):
            with self.subTest(indices=indices), self.assertRaises(ValueError):
                MeshRegion('patch', a.topology.identity, indices, LIMITS)
        with self.assertRaises(ValueError):
            MeshRegion('patch', a.topology.identity, [0, 1], MeshAnalysisLimits(1, 10, 10))

    def test_row_and_column_nonuniform_transforms_are_applied_exactly(self):
        row = (2,0,0,0, 0,3,0,0, 0,0,4,0, 0,0,3,1)
        col = tuple(row[c*4+r] for r in range(4) for c in range(4))
        for convention, transform in (('row', row), ('column', col)):
            with self.subTest(convention=convention):
                b = observation('b', transform=transform, convention=convention)
                self.assertEqual(self.measure(pair(second=selection(b))).minimum_distance, 3)

    def test_topology_mismatch_and_out_of_range_region_are_insufficient(self):
        selected = selection(observation())
        for region in (dataclasses.replace(selected.region, topology_id='0'*64),
                       MeshRegion('surface', selected.region.topology_id, (100,), LIMITS)):
            result = self.measure(pair(first=dataclasses.replace(selected, region=region)))
            self.assertEqual(result.status, 'insufficient')
            self.assertIsNone(result.minimum_distance)
            self.assertIsNone(result.surface_intersection)

    def test_required_omitted_deformation_prevents_false_intersection_result(self):
        # A would intersect B after an omitted +3 Z displacement. Its current
        # unmodified triangles cannot establish absence of that intersection.
        for effect in ('morph', 'cloth', 'material_displacement'):
            a = observation(coverage=(FeatureCoverage('bone','observed','fixture','pose',''),
                                     FeatureCoverage(effect,'excluded','fixture','omission','omitted +3 Z')))
            sample = pair(first=selection(a, required=('bone', effect)))
            result = self.measure(sample)
            self.assertEqual(result.status, 'insufficient')
            self.assertIsNone(result.surface_intersection)
            self.assertTrue(any(effect in reason for reason in result.reasons))
            accepted_reference = self.measure(pair(first=selection(a, excluded=(effect,))))
            self.assertEqual(accepted_reference.minimum_distance, 3)

    def test_accepted_exclusion_retains_achieved_coverage_not_only_permission(self):
        omitted = FeatureCoverage('cloth','excluded','fixture','known-omission','garment omitted')
        a = observation(coverage=(FeatureCoverage('bone','observed','fixture','pose',''), omitted))
        result = self.measure(pair(first=selection(a, excluded=('cloth',))))
        self.assertEqual(result.status, 'measured')
        self.assertIn(omitted, result.first.coverage)
        self.assertIn({'feature':'cloth','state':'excluded','producer_id':'fixture',
                       'evidence_id':'known-omission','reason':'garment omitted'}, result.to_mapping()['first']['coverage'])

    def test_result_does_not_keep_source_mesh_buffers_alive(self):
        a = observation()
        reference = weakref.ref(a)
        result = self.measure(pair(first=selection(a)))
        del a
        gc.collect()
        self.assertIsNone(reference())
        self.assertEqual(result.minimum_distance, 3)

    def test_wrong_requirement_pose_remains_in_result_as_insufficient_evidence(self):
        selected = selection(observation())
        requirement = dataclasses.replace(selected.requirement, pose=PoseKey('a','capture',1,1))
        result = self.measure(pair(first=dataclasses.replace(selected, requirement=requirement)))
        self.assertEqual(result.status, 'insufficient')
        self.assertEqual(result.first.pose.frame_id, 0)
        self.assertTrue(any('pose' in reason for reason in result.reasons))

    def test_acquisition_clocks_times_units_and_same_stream_pose_must_match(self):
        bad = [observation('b', acquired=ClockStamp('other',0)),
               observation('b', acquired=ClockStamp('simulation',1)),
               observation('b', units='metres'), observation('b',coordinate_system='other-world'),
               observation('b', pose=PoseKey('a','capture',1,1))]
        for b in bad:
            with self.subTest(b=b.component_id):
                result = self.measure(pair(second=selection(b)))
                self.assertEqual(result.status, 'insufficient')
                self.assertIsNone(result.minimum_squared_distance)

    def test_selected_degenerate_triangle_is_insufficient_without_repair(self):
        a = observation('a', ((0,0,0),(1,0,0),(2,0,0),(0,0,4),(2,0,4),(0,2,4)),
                        ((0,1,2),(3,4,5)))
        self.assertEqual(self.measure(pair(first=selection(a))).status, 'insufficient')
        self.assertEqual(self.measure(pair(first=selection(a,(1,)))).minimum_distance, 1)
        b = observation('b', transform=(0,)*15+(1,))
        self.assertEqual(self.measure(pair(second=selection(b))).status, 'insufficient')

    def test_coordinate_bound_applies_before_transform_cancellation(self):
        a = observation('a', ((2.0**300,0,0),(0,1,0),(0,0,1)),
                        transform=(2.0**-300,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1))
        result = self.measure(pair(first=selection(a)))
        self.assertEqual(result.status, 'insufficient')
        self.assertTrue(any('coordinate' in reason for reason in result.reasons))

    def test_limits_and_tolerance_reject_malformed_arguments(self):
        for tolerance in (-1, math.inf, math.nan, True, 10**400):
            with self.subTest(tolerance=tolerance), self.assertRaises(ValueError):
                self.measure(tolerance=tolerance)
        for arguments in ((0,1,1),(1,0,1),(1,1,False),(1,1,1,257)):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                MeshAnalysisLimits(*arguments)

    def test_measurement_admission_uses_current_limits_not_region_creation_limits(self):
        a = observation('a', ((0,0,0),(2,0,0),(0,2,0),(4,0,0),(6,0,0),(4,2,0)),
                        ((0,1,2),(3,4,5)))
        result = self.measure(pair(first=selection(a)), limits=MeshAnalysisLimits(1,10,10))
        self.assertEqual(result.status, 'insufficient')
        self.assertIsNone(result.minimum_distance)

    def test_exhausted_work_never_exposes_a_partial_minimum(self):
        a = observation('a', triangles=((0,1,2),(0,1,2)))
        b = observation('b', ((2,2,0),(3,2,0),(2,3,0)))
        for limits in (MeshAnalysisLimits(10,1,100), MeshAnalysisLimits(10,100,1)):
            result = self.measure(pair(selection(a),selection(b)), limits=limits)
            self.assertEqual(result.status, 'insufficient')
            self.assertIsNone(result.minimum_distance)
            self.assertIsNone(result.surface_intersection)
            self.assertLessEqual(result.stats.triangle_tests, limits.max_pair_tests)
            self.assertLessEqual(result.stats.node_visits, limits.max_node_visits)

    def test_nested_closed_surfaces_do_not_imply_disjoint_volumes(self):
        # Unit cube sits inside [-1,2]^3: nearest surfaces are one unit apart.
        faces=((0,1,2),(0,2,3),(4,6,5),(4,7,6),(0,4,5),(0,5,1),
               (3,2,6),(3,6,7),(0,3,7),(0,7,4),(1,5,6),(1,6,2))
        vertices=((0,0,0),(1,0,0),(1,1,0),(0,1,0),(0,0,1),(1,0,1),(1,1,1),(0,1,1))
        a=observation('a', vertices, faces)
        b=observation('b', tuple(tuple(3*v-1 for v in p) for p in vertices), faces)
        result=self.measure(pair(selection(a),selection(b)))
        self.assertEqual(result.minimum_squared_distance, 1)
        self.assertFalse(result.surface_intersection)
        self.assertEqual(result.containment, 'not_evaluated')

    def test_existing_schema_one_replay_supplies_the_same_measurement(self):
        a = observation()
        request = MeshRequest('request',a.component_id,a.component_generation,a.configuration_id,
                              a.pose,a.acquired,a.topology.identity)
        completion = MeshCompletion(request,'completed',ClockStamp('wall',10),'',a)
        with tempfile.TemporaryDirectory() as root:
            write_mesh_observation(root,'mesh',completion,limits=RECORD_LIMITS)
            decoded = read_mesh_observation(root,'mesh',limits=RECORD_LIMITS)
        result = self.measure(pair(first=selection(decoded.observation)))
        original = self.measure(pair(first=selection(a)))
        self.assertEqual(result.minimum_squared_distance, 9)
        self.assertEqual(result.first.observation_identity, original.first.observation_identity)


if __name__ == '__main__':
    unittest.main()
