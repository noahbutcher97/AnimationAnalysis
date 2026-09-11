import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

from animation_analysis import load_evidence
from animation_analysis.jobs.surface_review import publish


@unittest.skipUnless(importlib.util.find_spec('PIL'), 'Optional images extra is not installed')
class SurfacePublicationTests(unittest.TestCase):
    def test_arbitrary_label_bundle_can_publish_without_project_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = root / 'input'
            bundle.mkdir()
            data = struct.pack('<8sQII', b'SURFACE1', 50, 4, 1)
            data += bytes([60, 60, 60, 255] * 4) + bytes([3, 0, 0, 101]) + struct.pack('<8f', *([20] * 8))
            (bundle/'observation.surface').write_bytes(data)
            (bundle/'surfaces.json').write_text(json.dumps(dict(schema_version=1,
                depth_convention='camera_axis_cm_clear_infinity', label_semantics='frontmost_custom_depth',
                subjects={'Fixture': 3, 'MovingPart': 101}, frames=[dict(file='observation.surface', width=4, height=1,
                engine_frame=50, simulation_time_s=1.25, world_to_clip_row_major=[float(i % 5 == 0) for i in range(16)])])))
            result = publish(bundle, 'Fixture', 'MovingPart', .1, root/'report.html')
            self.assertEqual(result['measurements'][0]['minimum_visible_pixel_center_distance_px'], 3)
            evidence, digest = load_evidence(root/'report.html')
            self.assertEqual(digest, result['evidence_sha256'])
            self.assertEqual(evidence['frames'][0]['time'], dict(domain='simulation', seconds=1.25))
            self.assertFalse(list(root.rglob('*.png')))


if __name__ == '__main__':
    unittest.main()
