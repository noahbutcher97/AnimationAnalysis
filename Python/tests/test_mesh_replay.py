import dataclasses
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from animation_analysis import ClockStamp, PoseKey
from animation_analysis.mesh_records import MeshCompletion, MeshRequest
from animation_analysis.mesh_replay import read_mesh_observation, write_mesh_observation
from test_mesh_records import LIMITS, observation, request


class MeshReplayTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.sample = observation()
        self.completion = MeshCompletion(request(self.sample),'completed',ClockStamp('wall',300),'',self.sample)

    def write(self):
        return write_mesh_observation(self.root,'sample-12',self.completion,limits=LIMITS)

    def read(self, limits=LIMITS):
        return read_mesh_observation(self.root,'sample-12',limits=limits)

    def rewrite_record(self, mutate):
        folder=self.root/'sample-12'
        metadata=json.loads((folder/'record.json').read_text())
        mutate(metadata)
        data=json.dumps(metadata).encode()
        (folder/'record.json').write_bytes(data)
        commit=json.loads((folder/'complete.json').read_text())
        commit['files']['record.json']={'size_bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
        (folder/'complete.json').write_text(json.dumps(commit))

    def test_round_trip_preserves_binary_layout_pose_and_separate_completion(self):
        manifest=self.write()
        result=self.read()
        self.assertEqual(result,self.completion)
        self.assertEqual(result.observation.acquired,ClockStamp('simulation',1.25))
        self.assertEqual(result.completed,ClockStamp('wall',300))
        self.assertEqual(result.observation.pose,PoseKey('assembly','sensor',12,8))
        self.assertEqual((self.root/'sample-12/indices.bin').read_bytes(),b'\0\0\0\0\1\0\0\0\2\0\0\0\0\0\0\0\2\0\0\0\3\0\0\0')
        self.assertEqual(struct.unpack('<12d',(self.root/'sample-12/positions.bin').read_bytes()),
                         (0,0,0,1,0,0,1,1,0,0,1,0))
        self.assertEqual(set(manifest),{'record.json','indices.bin','positions.bin'})

    def test_failure_round_trip_retains_request_without_inventing_geometry(self):
        target=MeshRequest('early','panel',3,'opaque-lod0',None,None,None)
        failure=MeshCompletion(target,'cancelled',ClockStamp('wall',299),'before acquisition')
        write_mesh_observation(self.root,'sample-12',failure,limits=LIMITS)
        self.assertEqual(self.read(),failure)
        self.assertFalse((self.root/'sample-12/positions.bin').exists())

    def test_existing_bundle_is_never_overwritten(self):
        self.write()
        before=(self.root/'sample-12/complete.json').read_bytes()
        with self.assertRaises(ValueError):
            self.write()
        self.assertEqual((self.root/'sample-12/complete.json').read_bytes(),before)
        self.assertEqual(self.read(),self.completion)

    def test_unpublished_or_interrupted_bundle_is_not_completed_evidence(self):
        with patch('animation_analysis.mesh_replay.os.link',side_effect=OSError('publication interrupted')):
            with self.assertRaises(ValueError):
                self.write()
        self.assertFalse((self.root/'sample-12/complete.json').exists())
        with self.assertRaises(ValueError):
            self.read()
        with self.assertRaises(ValueError):
            self.write()

    def test_corrupted_and_truncated_payloads_are_rejected(self):
        self.write()
        path=self.root/'sample-12/positions.bin'
        original=path.read_bytes()
        for data in (original[:-1],original+b'\0',b'\0'*len(original)):
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                self.read()
        path.write_bytes(original)
        self.assertEqual(self.read(),self.completion)

    def test_pose_topology_and_configuration_cannot_be_relabelled(self):
        self.write()
        for mutate, restore in (
            (lambda d:d['observation']['pose'].update(revision=9),lambda d:d['observation']['pose'].update(revision=8)),
            (lambda d:d['observation'].update(topology_id='0'*64),lambda d:d['observation'].update(topology_id=self.sample.topology.identity)),
            (lambda d:d['observation'].update(configuration_id='different'),lambda d:d['observation'].update(configuration_id='opaque-lod0'))):
            self.rewrite_record(mutate)
            with self.assertRaises(ValueError):
                self.read()
            self.rewrite_record(restore)

    def test_unknown_schema_fields_and_encodings_are_rejected(self):
        self.write()
        for mutate,restore in (
            (lambda d:d.update(schema_version=2),lambda d:d.update(schema_version=1)),
            (lambda d:d.update(schema_version=True),lambda d:d.update(schema_version=1)),
            (lambda d:d.update(unknown=True),lambda d:d.pop('unknown')),
            (lambda d:d['observation'].update(position_encoding='float16'),lambda d:d['observation'].update(position_encoding='xyz-float64-le'))):
            self.rewrite_record(mutate)
            with self.assertRaises(ValueError):
                self.read()
            self.rewrite_record(restore)

    def test_duplicate_json_keys_are_rejected_even_with_a_matching_hash(self):
        self.write()
        path=self.root/'sample-12/complete.json'
        data=path.read_text()
        path.write_text(data.replace('"schema_version":1','"schema_version":1,"schema_version":1'))
        self.assertNotEqual(path.read_text(),data)
        with self.assertRaises(ValueError):
            self.read()

    def test_declared_counts_and_combined_limits_reject_before_payload_read(self):
        self.write()
        for changes in (dict(max_vertices=3),dict(max_payload_bytes=119),dict(max_metadata_bytes=10),dict(max_record_bytes=120)):
            with self.subTest(changes=changes),self.assertRaises(ValueError):
                self.read(dataclasses.replace(LIMITS,**changes))
        (self.root/'sample-12/positions.bin').unlink()
        self.rewrite_record(lambda d:d['topology'].update(vertex_count=2**40))
        with self.assertRaisesRegex(ValueError,'limit'):
            self.read()

    def test_oversize_writer_does_not_reserve_a_bundle(self):
        with self.assertRaises(ValueError):
            write_mesh_observation(self.root,'sample-12',self.completion,
                                   limits=dataclasses.replace(LIMITS,max_record_bytes=120))
        self.assertFalse((self.root/'sample-12').exists())

    def test_unsafe_paths_and_linked_payloads_are_rejected(self):
        for name in ('../escape','/absolute','C:relative','sample:stream','CON','LPT1.txt','bad.','bad\\part'):
            with self.subTest(name=name),self.assertRaises(ValueError):
                write_mesh_observation(self.root,name,self.completion,limits=LIMITS)
        self.write()
        outside=self.root/'outside.bin'
        payload=self.root/'sample-12/positions.bin'
        payload.rename(outside)
        try:
            payload.symlink_to(outside)
        except OSError as error:
            self.skipTest('OS does not permit symlink control: '+str(error))
        with self.assertRaises(ValueError):
            self.read()


if __name__ == '__main__':
    unittest.main()
