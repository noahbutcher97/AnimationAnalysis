import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

from animation_analysis import ClockStamp, PoseKey
from animation_analysis.mesh_records import (FeatureCoverage, MeshCompletion, MeshObservation,
    MeshRecordLimits, MeshRequest, MeshSection, MeshTopology)
from animation_analysis.mesh_replay import write_mesh_observation


SCRIPT = Path(__file__).parents[1] / 'verify_mesh_reference.py'
SPEC = importlib.util.spec_from_file_location('verify_mesh_reference', SCRIPT)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)
LIMITS = MeshRecordLimits(100, 300, 10, 10000, 65536, 80000)


def completion(name, feature, lod, sections=1):
    indices = struct.pack('<6I', 0, 1, 2, 0, 2, 3)
    ranges = ((MeshSection('0', 0, 6, 'material-0'),) if sections == 1 else
              (MeshSection('0', 0, 3, 'material-0'), MeshSection('1', 3, 3, 'material-1')))
    topology = MeshTopology(name + '-asset', 1, lod, 4, indices, ranges, LIMITS)
    pose = PoseKey('assembly-é', 'inspection', 12, 3)
    acquired = ClockStamp('unreal-monotonic', 1.25)
    sample = MeshObservation(name, 1, topology, struct.pack('<12d', *range(12)),
        'centimetres', 'unreal-left-handed-z-up', 'row',
        (1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1), pose, acquired,
        'unreal-cpu-bone-reference-v1' if feature == 'bone' else 'unreal-rigid-reference-v1',
        'neutral-reference:fingerprint',
        (FeatureCoverage(feature, 'observed', 'native', name, 'qualified'),), LIMITS)
    request = MeshRequest(name, name, 1, sample.configuration_id, pose, acquired, topology.identity)
    return MeshCompletion(request, 'completed', ClockStamp('unreal-monotonic', 1.5), '', sample)


class VerifyMeshReferenceTests(unittest.TestCase):
    def test_reads_expected_bundles_and_reports_independent_native_controls(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_mesh_observation(root, 'fine', completion('fine', 'bone', 0, 2), limits=LIMITS)
            write_mesh_observation(root, 'coarse', completion('coarse', 'bone', 1, 2), limits=LIMITS)
            write_mesh_observation(root, 'rigid', completion('rigid', 'rigid', 0), limits=LIMITS)
            (root/'reference-controls.json').write_text(json.dumps({
                'fine_max_error_cm': .0002, 'coarse_max_error_cm': .0003,
                'live_change_cm': 4.0, 'peak_reserved_bytes': 1234,
                'vertices': 4, 'indices': 6,
                'acquisition_ms_batches': [[1,2,3], [2,3,4], [3,4,5]],
            }), encoding='utf-8')

            result = VERIFY.verify(root, LIMITS)

            self.assertEqual(set(result['bundles']), {'fine', 'coarse', 'rigid'})
            self.assertEqual(result['bundles']['fine']['section_count'], 2)
            self.assertEqual(result['controls']['fine_max_error_cm'], .0002)
            self.assertEqual(result['performance']['sample_count'], 9)
            self.assertEqual(result['performance']['p50_ms'], 3.0)
            self.assertEqual(result['performance']['p95_ms'], 5.0)

    def test_rejects_wrong_required_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_mesh_observation(root, 'fine', completion('fine', 'rigid', 0, 2), limits=LIMITS)
            write_mesh_observation(root, 'coarse', completion('coarse', 'bone', 1, 2), limits=LIMITS)
            write_mesh_observation(root, 'rigid', completion('rigid', 'rigid', 0), limits=LIMITS)
            (root/'reference-controls.json').write_text(json.dumps({'acquisition_ms_batches': [[1]]}), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'fine.*bone'):
                VERIFY.verify(root, LIMITS)


if __name__ == '__main__':
    unittest.main()
