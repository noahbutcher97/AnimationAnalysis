import dataclasses
import struct
import unittest

from animation_analysis import ClockStamp, PoseKey
from animation_analysis import (
    FeatureCoverage, MeshCompletion, MeshObservation, MeshRecordLimits,
    MeshRequest, MeshRequirement, MeshSection, MeshTopology, assess_mesh_coverage,
)


LIMITS = MeshRecordLimits(max_vertices=100, max_indices=300, max_sections=10,
                          max_payload_bytes=4096, max_metadata_bytes=16384,
                          max_record_bytes=20480)
MATRIX = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1)


def topology(**changes):
    values = dict(asset_id='panel-asset', configuration_generation=2, lod=0,
                  vertex_count=4, index_data=struct.pack('<6I', 0, 1, 2, 0, 2, 3),
                  sections=[MeshSection('front', 0, 6, 'steel')], limits=LIMITS)
    return MeshTopology(**(values | changes))


def observation(**changes):
    values = dict(component_id='panel', component_generation=3, topology=topology(),
                  position_data=struct.pack('<12d', 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0),
                  units='metres', coordinate_system='right-handed-z-up',
                  vector_convention='row', component_to_world=MATRIX,
                  pose=PoseKey('assembly', 'sensor', 12, 8), acquired=ClockStamp('simulation', 1.25),
                  producer_id='rigid-reference-v1', configuration_id='opaque-lod0',
                  coverage=[FeatureCoverage('rigid', 'observed', 'fixture-v1', 'sample-12', '')],
                  limits=LIMITS)
    return MeshObservation(**(values | changes))


def requirement(sample, **changes):
    values = dict(component_id='panel', component_generation=3, topology_id=sample.topology.identity,
                  configuration_id='opaque-lod0',
                  pose=PoseKey('assembly', 'sensor', 12, 8), units='metres',
                  coordinate_system='right-handed-z-up', required_features=('rigid',),
                  known_features=('rigid', 'bones', 'cloth'), allowed_exclusions=(),
                  allowed_producers=('rigid-reference-v1',))
    return MeshRequirement(**(values | changes))


def request(sample):
    return MeshRequest('request-12',sample.component_id,sample.component_generation,
                       sample.configuration_id,sample.pose,sample.acquired,sample.topology.identity)


class MeshRecordTests(unittest.TestCase):
    def test_observation_owns_buffers_and_nested_metadata(self):
        positions = bytearray(observation().position_data)
        indices = bytearray(topology().index_data)
        sections = [MeshSection('front', 0, 6, 'steel')]
        matrix = list(MATRIX)
        coverage = [FeatureCoverage('rigid', 'observed', 'fixture-v1', 'sample-12', '')]
        sample = observation(position_data=positions, topology=topology(index_data=indices, sections=sections),
                             component_to_world=matrix, coverage=coverage)
        positions[:] = b'\0' * len(positions); indices[:] = b'\0' * len(indices)
        sections.clear(); matrix[12] = 999; coverage.clear()
        self.assertEqual(list(sample.positions()), [(0., 0., 0.), (1., 0., 0.), (1., 1., 0.), (0., 1., 0.)])
        self.assertEqual(list(sample.topology.indices()), [0, 1, 2, 0, 2, 3])
        self.assertEqual(sample.component_to_world[12], 10)
        self.assertTrue(assess_mesh_coverage(sample, requirement(sample)).eligible)

    def test_topology_identity_includes_section_material_and_generation(self):
        original = topology()
        for changed in (topology(configuration_generation=3), topology(lod=1), topology(asset_id='other'),
                        topology(sections=[MeshSection('front', 0, 6, None)]),
                        topology(index_data=struct.pack('<6I', 0, 2, 1, 0, 3, 2))):
            self.assertNotEqual(original.identity, changed.identity)

    def test_topology_rejects_invalid_counts_indices_and_section_coverage(self):
        for changes in (dict(vertex_count=True), dict(vertex_count=0), dict(lod=-1),
                        dict(index_data=None), dict(index_data='not binary'),
                        dict(index_data=b'bad'), dict(index_data=struct.pack('<3I',0,1,4)),
                        dict(sections=[]), dict(sections=[MeshSection('gap',3,3,'steel')]),
                        dict(sections=[MeshSection('a',0,3,'x'),MeshSection('b',0,3,'x')]),
                        dict(sections=[MeshSection('a',0,3,'x'),MeshSection('a',3,3,'x')])):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                topology(**changes)

    def test_limits_reject_before_owning_an_oversize_record(self):
        for changes in (dict(max_vertices=3), dict(max_indices=5), dict(max_payload_bytes=23),
                        dict(max_record_bytes=20), dict(max_metadata_bytes=20)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                topology(limits=dataclasses.replace(LIMITS, **changes))
        with self.assertRaises(ValueError):
            observation(limits=dataclasses.replace(LIMITS, max_payload_bytes=119))

    def test_positions_and_transform_must_be_finite_and_affine(self):
        for changes in (dict(position_data=b''), dict(position_data=b'\0'*95),
                        dict(position_data=struct.pack('<12d',float('nan'),*([0]*11))),
                        dict(component_to_world=[float('inf')]+list(MATRIX[1:])),
                        dict(component_to_world=list(MATRIX[:15])+[0]),
                        dict(component_to_world=[True]+list(MATRIX[1:])),
                        dict(vector_convention='guessed'), dict(units='')):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                observation(**changes)
        column = tuple(MATRIX[c*4+r] for r in range(4) for c in range(4))
        self.assertEqual(observation(vector_convention='column',component_to_world=column).component_to_world[3],10)

    def test_completion_keeps_acquisition_and_completion_clocks_distinct(self):
        sample = observation()
        result = MeshCompletion(request(sample),'completed',ClockStamp('wall',300), '', sample)
        self.assertEqual(result.observation.pose,PoseKey('assembly','sensor',12,8))
        self.assertEqual(result.observation.acquired,ClockStamp('simulation',1.25))
        with self.assertRaises(ValueError):
            result.completed.delta_seconds(sample.acquired)
        for status in ('failed','cancelled','timed_out','unavailable'):
            failed=MeshCompletion(request(sample),status,ClockStamp('wall',300),'explicit reason')
            self.assertIsNone(failed.observation)
            self.assertEqual(failed.request.pose, sample.pose)
            self.assertEqual(failed.request.component_generation, 3)
            with self.assertRaises(ValueError):
                MeshCompletion(request(sample),status,ClockStamp('wall',300),'reason',sample)
        with self.assertRaises(ValueError):
            MeshCompletion(request(sample),'completed',ClockStamp('wall',300),'')
        early=MeshRequest('r','panel',3,'opaque-lod0',None,None,None)
        self.assertIsNone(MeshCompletion(early,'cancelled',ClockStamp('wall',300),'before acquisition').request.pose)
        with self.assertRaises(ValueError):
            MeshCompletion(early,'completed',ClockStamp('wall',300),'',sample)
        with self.assertRaises(ValueError):
            MeshCompletion(dataclasses.replace(request(sample),component_generation=4),'completed',ClockStamp('wall',300),'',sample)
        with self.assertRaises(ValueError):
            MeshCompletion(request(sample),'completed',ClockStamp('simulation',1.0),'',sample)

    def test_eligibility_rejects_wrong_component_topology_pose_and_producer(self):
        sample = observation()
        for changes in (dict(component_id='other'), dict(component_generation=4),
                        dict(topology_id='0'*64), dict(pose=PoseKey('assembly','sensor',12,9)),
                        dict(configuration_id='changed-weights'),
                        dict(units='centimetres'), dict(coordinate_system='left-handed-z-up'),
                        dict(allowed_producers=('other',))):
            with self.subTest(changes=changes):
                result = assess_mesh_coverage(sample, requirement(sample,**changes))
                self.assertFalse(result.eligible)
                self.assertTrue(result.reasons)

    def test_excluded_cloth_is_only_eligible_for_an_explicit_reference_request(self):
        sample = observation(coverage=[FeatureCoverage('rigid','observed','p','w',''),
                                       FeatureCoverage('cloth','excluded','p','w','bone reference')])
        self.assertFalse(assess_mesh_coverage(sample,requirement(sample)).eligible)
        self.assertTrue(assess_mesh_coverage(sample,requirement(sample,allowed_exclusions=('cloth',))).eligible)
        self.assertFalse(assess_mesh_coverage(sample,requirement(sample,required_features=('rigid','cloth'))).eligible)

    def test_unknown_missing_and_unsupported_coverage_never_satisfies_required_feature(self):
        for state in ('unknown','unsupported','excluded'):
            sample=observation(coverage=[FeatureCoverage('rigid',state,'p','w','reason')])
            result=assess_mesh_coverage(sample,requirement(sample))
            self.assertFalse(result.eligible)
            self.assertIn('rigid:'+state,result.reasons)
        sample=observation(coverage=[])
        self.assertIn('rigid:missing',assess_mesh_coverage(sample,requirement(sample)).reasons)

    def test_exclusion_permission_does_not_reclassify_unknown_or_unsupported_evidence(self):
        for state in ('unknown','unsupported'):
            sample=observation(coverage=[FeatureCoverage('rigid','observed','p','w',''),
                                         FeatureCoverage('cloth',state,'p','w','unresolved')])
            result=assess_mesh_coverage(sample,requirement(sample,allowed_exclusions=('cloth',)))
            self.assertFalse(result.eligible)
            self.assertIn('cloth:'+state,result.reasons)

    def test_inactive_requires_a_witness_and_unfamiliar_features_remain_ineligible(self):
        sample=observation(coverage=[FeatureCoverage('rigid','inactive','p','w','not active')])
        self.assertTrue(assess_mesh_coverage(sample,requirement(sample)).eligible)
        with self.assertRaises(ValueError):
            FeatureCoverage('cloth','inactive','p','','not active')
        with self.assertRaises(ValueError):
            FeatureCoverage('cloth','probably','p','w','guess')
        sample=observation(coverage=[FeatureCoverage('rigid','observed','p','w',''),
                                     FeatureCoverage('future-effect','observed','p','w','')])
        self.assertFalse(assess_mesh_coverage(sample,requirement(sample)).eligible)
        with self.assertRaises(ValueError):
            observation(coverage=[FeatureCoverage('rigid','observed','p','w','')]*2)


if __name__ == '__main__':
    unittest.main()
