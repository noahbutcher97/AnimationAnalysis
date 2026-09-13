#!/usr/bin/env python3
"""Verify one explicit native CPU mesh-reference observation run."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

from animation_analysis.mesh_records import MeshRecordLimits
from animation_analysis.mesh_replay import read_mesh_observation


EXPECTED = {
    'fine': ('bone', 'unreal-cpu-bone-reference-v1', 0, 2),
    'coarse': ('bone', 'unreal-cpu-bone-reference-v1', 1, 2),
    'rigid': ('rigid', 'unreal-rigid-reference-v1', 0, 1),
}
CONTROL_FIELDS = {
    'fine_max_error_cm', 'coarse_max_error_cm', 'live_change_cm',
    'peak_reserved_bytes', 'vertices', 'indices', 'acquisition_ms_batches',
}


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f'duplicate control key: {key}')
        result[key] = value
    return result


def _finite_number(value, label, *, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < minimum:
        raise ValueError(f'{label} must be a finite number >= {minimum}')
    return value


def _percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def _read_controls(path):
    try:
        with path.open('rb') as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise ValueError('reference-controls.json exceeds 64 KiB')
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_pairs,
                           parse_constant=lambda token: (_ for _ in ()).throw(ValueError(f'nonfinite control: {token}')))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f'cannot read native reference controls: {error}') from error
    if not isinstance(value, dict) or set(value) != CONTROL_FIELDS:
        raise ValueError('reference-controls.json has missing or unsupported fields')
    for field in CONTROL_FIELDS - {'acquisition_ms_batches'}:
        _finite_number(value[field], field)
    batches = value['acquisition_ms_batches']
    if not isinstance(batches, list) or len(batches) != 3 or any(not isinstance(batch, list) or not batch for batch in batches):
        raise ValueError('acquisition_ms_batches must contain three nonempty batches')
    samples = []
    for batch_index, batch in enumerate(batches):
        for sample_index, sample in enumerate(batch):
            samples.append(float(_finite_number(sample, f'batch {batch_index} sample {sample_index}')))
    return value, samples


def verify(observations, limits):
    root = Path(observations)
    if not root.is_dir():
        raise ValueError('explicit native observations directory must exist')
    if not isinstance(limits, MeshRecordLimits):
        raise ValueError('explicit MeshRecordLimits required')
    bundles = {}
    topology_ids = {}
    for name, (feature, producer, lod, minimum_sections) in EXPECTED.items():
        completion = read_mesh_observation(root, name, limits=limits)
        sample = completion.observation
        if completion.status != 'completed' or sample is None:
            raise ValueError(f'{name} is not a completed geometry observation')
        if completion.request.request_id != name:
            raise ValueError(f'{name} request identity differs from bundle role')
        if sample.units != 'centimetres' or sample.coordinate_system != 'unreal-left-handed-z-up' or sample.vector_convention != 'row':
            raise ValueError(f'{name} has unsupported native units or coordinate convention')
        if sample.acquired.domain != 'unreal-monotonic' or completion.completed.domain != 'unreal-monotonic':
            raise ValueError(f'{name} does not preserve the Unreal monotonic clock domain')
        coverage = {item.feature: item for item in sample.coverage}
        if feature not in coverage or coverage[feature].state != 'observed':
            raise ValueError(f'{name} lacks required observed {feature} coverage')
        if sample.producer_id != producer or sample.topology.lod != lod:
            raise ValueError(f'{name} producer or analysis LOD differs from the native reference contract')
        if len(sample.topology.sections) < minimum_sections:
            raise ValueError(f'{name} does not contain the required section coverage')
        topology_ids[name] = sample.topology.identity
        bundles[name] = {
            'vertex_count': sample.topology.vertex_count,
            'index_count': len(sample.topology.index_data) // 4,
            'section_count': len(sample.topology.sections),
            'topology_sha256': sample.topology.identity,
            'positions_sha256': hashlib.sha256(sample.position_data).hexdigest(),
            'indices_sha256': hashlib.sha256(sample.topology.index_data).hexdigest(),
            'producer_id': sample.producer_id,
            'pose_frame': sample.pose.frame_id,
            'pose_revision': sample.pose.revision,
        }
    if topology_ids['fine'] == topology_ids['coarse']:
        raise ValueError('fine and coarse LOD topology identities must differ')
    controls, samples = _read_controls(root/'reference-controls.json')
    return {
        'format': 'native_mesh_reference_verification',
        'schema_version': 1,
        'observations': str(root.resolve()),
        'bundles': bundles,
        'controls': {key: controls[key] for key in sorted(CONTROL_FIELDS - {'acquisition_ms_batches'})},
        'performance': {
            'batch_count': len(controls['acquisition_ms_batches']),
            'sample_count': len(samples),
            'minimum_ms': min(samples),
            'p50_ms': _percentile(samples, .50),
            'p95_ms': _percentile(samples, .95),
            'maximum_ms': max(samples),
            'raw_batches_ms': controls['acquisition_ms_batches'],
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('observations', help='run-owned directory containing fine, coarse and rigid bundles')
    parser.add_argument('--max-vertices', type=int, required=True)
    parser.add_argument('--max-indices', type=int, required=True)
    parser.add_argument('--max-sections', type=int, required=True)
    parser.add_argument('--max-payload-bytes', type=int, required=True)
    parser.add_argument('--max-metadata-bytes', type=int, required=True)
    parser.add_argument('--max-record-bytes', type=int, required=True)
    parser.add_argument('--output', help='optional JSON report path; stdout is always written')
    arguments = parser.parse_args(argv)
    try:
        limits = MeshRecordLimits(arguments.max_vertices, arguments.max_indices, arguments.max_sections,
                                  arguments.max_payload_bytes, arguments.max_metadata_bytes, arguments.max_record_bytes)
        result = verify(arguments.observations, limits)
        encoded = json.dumps(result, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n'
        if arguments.output:
            output = Path(arguments.output)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(encoded, encoding='utf-8')
        sys.stdout.write(encoded)
        return 0
    except (OSError, ValueError, TypeError) as error:
        sys.stderr.write(f'native mesh reference verification failed: {error}\n')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
