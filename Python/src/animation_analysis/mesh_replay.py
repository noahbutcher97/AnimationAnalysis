"""Bounded mesh replay with exclusive bundles and a final hash-bound commit marker."""
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from .contracts import ClockStamp, PoseKey
from .errors import EvidenceError
from .integrity import bundle_path
from .mesh_records import (FeatureCoverage, MeshCompletion, MeshObservation, MeshRecordLimits,
                           MeshRequest, MeshSection, MeshTopology, _hash, _integer, _json)


def _fields(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise EvidenceError('Mesh metadata has missing or unsupported fields')
    return value


def _schema(value, kind, fields):
    _fields(value, set(fields) | {'format','schema_version'})
    if value['format'] != kind or type(value['schema_version']) is not int or value['schema_version'] != 1:
        raise EvidenceError('Unsupported mesh schema version')


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise EvidenceError('Duplicate mesh JSON key')
        result[key] = value
    return result


def _decode(data):
    def reject(value):
        raise EvidenceError('Non-finite mesh JSON number')
    return json.loads(data.decode('utf-8'), object_pairs_hook=_pairs, parse_constant=reject)


def _safe_path(root, relative):
    if not isinstance(relative,str) or not relative or len(relative) > 256 or '\\' in relative:
        raise EvidenceError('Expected portable relative bundle path')
    parts = relative.split('/')
    devices = {'CON','PRN','AUX','NUL'} | {f'{prefix}{i}' for prefix in ('COM','LPT') for i in range(1,10)}
    for part in parts:
        if (not re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,127}',part)
                or part.endswith('.') or part.split('.')[0].upper() in devices):
            raise EvidenceError('Unsafe mesh bundle path')
    root = Path(root)
    current = root
    for part in [None]+parts:
        if part is not None:
            current = current/part
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0) & 0x400:
            raise EvidenceError('Linked or redirected mesh paths are unsupported')
    return bundle_path(root,relative)


def _read(path, maximum, expected=None):
    with path.open('rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > maximum:
            raise EvidenceError('Mesh file exceeds limit or is not a regular file')
        if expected is not None and info.st_size != expected:
            raise EvidenceError('Mesh file length differs from manifest')
        size = info.st_size if expected is None else expected
        data = stream.read(size+1)
        if len(data) != size:
            raise EvidenceError('Mesh file changed or is truncated')
    return data


def _descriptor(data):
    return dict(size_bytes=len(data),sha256=hashlib.sha256(data).hexdigest())


def _check_descriptor(value):
    _fields(value,('size_bytes','sha256'))
    _integer(value['size_bytes'],'file size',1); _hash(value['sha256'])


def _read_verified(folder, name, descriptor, maximum):
    _check_descriptor(descriptor)
    if descriptor['size_bytes'] > maximum:
        raise EvidenceError('Mesh file exceeds explicit limit')
    data = _read(_safe_path(folder,name),maximum,descriptor['size_bytes'])
    if hashlib.sha256(data).hexdigest() != descriptor['sha256']:
        raise EvidenceError('Mesh file hash mismatch')
    return data


def _clock(value):
    return ClockStamp(**_fields(value,('domain','seconds')))


def _pose(value):
    return PoseKey(**_fields(value,('subject_id','stream_id','frame_id','revision')))


def _request(value):
    data = dict(_fields(value,MeshRequest.__dataclass_fields__))
    data['pose'] = _pose(data['pose']) if data['pose'] is not None else None
    data['acquired'] = _clock(data['acquired']) if data['acquired'] is not None else None
    return MeshRequest(**data)


def _completion_mapping(result, buffers):
    sample = result.observation
    return dict(format='mesh_observation',schema_version=1,request=asdict(result.request),
                status=result.status,completed=asdict(result.completed),reason=result.reason,
                observation=sample.metadata() if sample else None,
                topology=sample.topology.metadata() if sample else None,buffers=buffers)


def _budget(limits, sample, metadata_bytes):
    if not isinstance(limits,MeshRecordLimits):
        raise EvidenceError('Explicit MeshRecordLimits required')
    if sample is None:
        limits.check(0,0,0,0,metadata_bytes)
    else:
        limits.check(sample.topology.vertex_count,len(sample.topology.index_data)//4,len(sample.topology.sections),
                     len(sample.position_data)+len(sample.topology.index_data),metadata_bytes)


def _write_file(path, data):
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def write_mesh_observation(root, relative_record, completion, *, limits):
    """Reserve a new directory; interruption leaves an explicitly incomplete bundle.

    Requires a stable trusted root and hard-link support for atomic exclusive marker
    publication. No completed directory or marker is ever replaced.
    """
    try:
        if not isinstance(completion,MeshCompletion):
            raise EvidenceError('Expected MeshCompletion')
        folder = _safe_path(root,relative_record)
        _budget(limits,completion.observation,0)
        sample = completion.observation
        payloads = {'positions.bin':sample.position_data,'indices.bin':sample.topology.index_data} if sample else {}
        descriptors = {name:_descriptor(data) for name,data in payloads.items()}
        record = _json(_completion_mapping(completion,descriptors))
        files = dict(descriptors,**{'record.json':_descriptor(record)})
        marker = _json(dict(format='mesh_observation_commit',schema_version=1,files=files))
        _budget(limits,sample,len(record)+len(marker))
        folder.mkdir()  # Exclusive reservation; missing parents and existing bundles reject.
        for name,data in payloads.items():
            _write_file(folder/name,data)
        _write_file(folder/'record.json',record)
        _write_file(folder/'complete.pending',marker)
        os.link(folder/'complete.pending',folder/'complete.json')
        try:
            (folder/'complete.pending').unlink()
        except OSError:
            pass  # Publication succeeded. A leftover temporary marker is not read.
        return files
    except (OSError,ValueError,TypeError,OverflowError,RecursionError) as error:
        raise EvidenceError(f'Cannot write mesh observation: {error}') from error


def read_mesh_observation(root, relative_record, *, limits):
    """Read only hash-bound files from one committed bundle under explicit limits.

    Rejects symlinks/reparse points; the caller must keep the root stable against
    concurrent path replacement. This is integrity checking, not producer authentication.
    """
    try:
        if not isinstance(limits,MeshRecordLimits):
            raise EvidenceError('Explicit MeshRecordLimits required')
        folder = _safe_path(root,relative_record)
        marker_bytes = _read(_safe_path(folder,'complete.json'),limits.max_metadata_bytes)
        marker = _decode(marker_bytes)
        _schema(marker,'mesh_observation_commit',('files',))
        files = marker['files']
        if not isinstance(files,dict) or set(files) not in ({'record.json'}, {'record.json','positions.bin','indices.bin'}):
            raise EvidenceError('Unexpected mesh commit files')
        record_bytes = _read_verified(folder,'record.json',files['record.json'],limits.max_metadata_bytes-len(marker_bytes))
        record = _decode(record_bytes)
        _schema(record,'mesh_observation',('request','status','completed','reason','observation','topology','buffers'))
        buffers = {name:desc for name,desc in files.items() if name != 'record.json'}
        if record['buffers'] != buffers:
            raise EvidenceError('Mesh payload descriptors differ from committed files')
        acquired_request = _request(record['request'])
        completed = _clock(record['completed'])
        metadata_bytes = len(marker_bytes)+len(record_bytes)
        if record['observation'] is None:
            if record['topology'] is not None or buffers:
                raise EvidenceError('Absent observation cannot own geometry')
            limits.check(0,0,0,0,metadata_bytes)
            return MeshCompletion(acquired_request,record['status'],completed,record['reason'])
        top = _fields(record['topology'],('asset_id','configuration_generation','lod','vertex_count','index_encoding','sections'))
        if top['index_encoding'] != 'uint32-le-global':
            raise EvidenceError('Unsupported mesh index encoding')
        _integer(top['vertex_count'],'vertex count',1)
        if not isinstance(top['sections'],list):
            raise EvidenceError('Expected explicit sections')
        limits.check(top['vertex_count'],0,len(top['sections']),0,metadata_bytes)
        sections = tuple(MeshSection(**_fields(s,MeshSection.__dataclass_fields__)) for s in top['sections'])
        index_count = sum(s.index_count for s in sections)
        position_bytes, index_bytes = top['vertex_count']*24,index_count*4
        limits.check(top['vertex_count'],index_count,len(sections),position_bytes+index_bytes,metadata_bytes)
        _fields(buffers,('positions.bin','indices.bin'))
        for name,count in (('positions.bin',position_bytes),('indices.bin',index_bytes)):
            _check_descriptor(buffers[name])
            if buffers[name]['size_bytes'] != count:
                raise EvidenceError('Mesh payload length differs from declared geometry')
        indices = _read_verified(folder,'indices.bin',buffers['indices.bin'],limits.max_payload_bytes)
        topology = MeshTopology(top['asset_id'],top['configuration_generation'],top['lod'],
                                top['vertex_count'],indices,sections,limits)
        data = dict(_fields(record['observation'],('component_id','component_generation','topology_id','units',
                    'coordinate_system','vector_convention','component_to_world','pose','acquired',
                    'producer_id','configuration_id','coverage','position_encoding')))
        if data.pop('topology_id') != topology.identity:
            raise EvidenceError('Mesh topology identity mismatch')
        if data.pop('position_encoding') != 'xyz-float64-le':
            raise EvidenceError('Unsupported position encoding')
        if not isinstance(data['coverage'],list) or len(data['coverage']) > 64:
            raise EvidenceError('Coverage exceeds feature limit')
        data['coverage'] = tuple(FeatureCoverage(**_fields(c,FeatureCoverage.__dataclass_fields__)) for c in data['coverage'])
        data['pose'],data['acquired'] = _pose(data['pose']),_clock(data['acquired'])
        positions = _read_verified(folder,'positions.bin',buffers['positions.bin'],limits.max_payload_bytes)
        sample = MeshObservation(topology=topology,position_data=positions,limits=limits,**data)
        return MeshCompletion(acquired_request,record['status'],completed,record['reason'],sample)
    except (OSError,ValueError,TypeError,KeyError,OverflowError,RecursionError) as error:
        raise EvidenceError(f'Cannot read mesh observation: {error}') from error
