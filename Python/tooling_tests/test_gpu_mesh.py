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


SCRIPT = Path(__file__).parents[1] / 'verify_gpu_mesh.py'
SPEC = importlib.util.spec_from_file_location('verify_gpu_mesh', SCRIPT)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)
LIMITS = MeshRecordLimits(1000, 3000, 20, 100000, 65536, 200000)


def write_mesh(root, name='gpu', producer='unreal-skin-cache-bone-v1'):
    positions = (-.5,-.5,0, -.5,.5,0, .5,.5,0, .5,-.5,0)
    indices = struct.pack('<6I', 0,2,1, 0,3,2)
    topology = MeshTopology('fixture', 2, 0, 4, indices,
                            (MeshSection('surface', 0, 6, 'opaque'),), LIMITS)
    pose = PoseKey('fixture', 'render', 42, 8)
    acquired = ClockStamp('unreal-monotonic', 2.0)
    sample = MeshObservation('fixture', 1, topology, struct.pack('<12d', *positions),
        'centimetres', 'unreal-left-handed-z-up', 'row',
        (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1), pose, acquired, producer,
        'gpu-config', (FeatureCoverage('bone', 'observed', producer, name, ''),), LIMITS)
    request = MeshRequest(name, 'fixture', 1, 'gpu-config', pose, acquired, topology.identity)
    write_mesh_observation(root, name, MeshCompletion(request, 'completed',
                           ClockStamp('unreal-monotonic', 2.1), '', sample), limits=LIMITS)
    return topology.identity


def write_surface(path, frame=42):
    width = height = 20
    count = width * height
    bgra = bytearray(count * 4)
    labels = bytearray(count)
    scene = [float('inf')] * count
    labelled = [float('inf')] * count
    labels[0] = 23  # A different enrolled primitive does not belong to label 7's oracle.
    scene[0] = labelled[0] = 1000.0
    for y in range(5, 15):
        for x in range(5, 15):
            index = y * width + x
            labels[index] = 7
            scene[index] = labelled[index] = 1.0
    path.write_bytes(struct.pack('<8sQII', b'SURFACE1', frame, width, height) + bgra + labels +
                     struct.pack(f'<{count}f', *scene) + struct.pack(f'<{count}f', *labelled))


def view(topology, **updates):
    value = {'format':'gpu_mesh_raster_view', 'schema_version':1, 'engine_frame':42,
             'renderer_frame':99, 'world_to_clip_row_major':[1,0,0,0, 0,1,0,0,
             0,0,1,0, 0,0,0,1], 'label':7, 'topology_id':topology,
             'configuration_id':'gpu-config', 'producer_id':'unreal-skin-cache-bone-v1',
             'lod':0, 'render_pass_id':'viewport-main-depth-v1'}
    value.update(updates)
    return value


class GpuMeshVerifierTests(unittest.TestCase):
    def test_matches_independent_square_silhouette_depth_and_wrong_pose_control(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            topology = write_mesh(root)
            surface = root/'gpu.surface'; write_surface(surface)
            sidecar = root/'gpu.view.json'; sidecar.write_text(json.dumps(view(topology)), encoding='utf-8')

            result = VERIFY.verify(root/'gpu', surface, sidecar, mesh_only=False)

            self.assertEqual(result['silhouette']['iou'], 1.0)
            self.assertEqual(result['depth']['interior_samples'], 64)
            self.assertLess(result['depth']['max_scene_error_cm'], 1e-12)
            self.assertLess(result['depth']['max_label_error_cm'], 1e-12)
            self.assertLess(result['wrong_pose']['iou'], .9)
            self.assertTrue(result['passed'])

    def test_rejects_incorrect_view_identities_before_metrics(self):
        cases = {'lod':1, 'configuration_id':'wrong', 'engine_frame':41,
                 'render_pass_id':'wrong-pass'}
        for field, value in cases.items():
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); topology = write_mesh(root)
                surface = root/'gpu.surface'; write_surface(surface)
                sidecar = root/'gpu.view.json'; sidecar.write_text(json.dumps(view(topology, **{field:value})), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, field.replace('_', ' ')):
                    VERIFY.verify(root/'gpu', surface, sidecar, mesh_only=False)

    def test_mesh_only_accepts_gpu_fine_or_coarse_replay_without_sidecars(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); write_mesh(root, 'coarse')
            result = VERIFY.verify(root/'coarse', None, None, mesh_only=True)
            self.assertTrue(result['passed'])
            self.assertEqual(result['mode'], 'mesh-only')
            self.assertEqual(result['mesh']['producer_id'], 'unreal-skin-cache-bone-v1')


if __name__ == '__main__':
    unittest.main()
