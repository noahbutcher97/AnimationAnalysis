"""Adapter for a bounded viewport surface bundle; no gameplay classes or fixture names."""
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def load_surface_bundle(directory):
    directory = Path(directory).resolve()
    manifest_path = directory / 'surfaces.json'
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes.decode('utf-8-sig'))
    if manifest.get('schema_version') != 1 or manifest.get('depth_convention') != 'camera_axis_cm_clear_infinity' or manifest.get('label_semantics') != 'frontmost_custom_depth':
        raise ValueError('Unsupported surface evidence contract')
    subjects = manifest['subjects']
    if not subjects or any(not isinstance(k, str) or not k or type(v) is not int or not 1 <= v <= 255 for k,v in subjects.items()) or len(set(subjects.values())) != len(subjects):
        raise ValueError('Subjects require unique nonzero byte IDs and names')
    hashes, frames, identities = {'surfaces.json': hashlib.sha256(manifest_bytes).hexdigest()}, [], set()
    if not manifest['frames'] or len(manifest['frames']) > 60:
        raise ValueError('Surface review requires 1..60 bounded observations')
    previous_frame, previous_time = -1, -math.inf
    for metadata in manifest['frames']:
        path = (directory / metadata['file']).resolve()
        if path.parent != directory or path.suffix != '.surface' or path.name in identities:
            raise ValueError('Surface file must be a unique direct bundle member')
        identities.add(path.name)
        if path.stat().st_size > 24+1920*1080*13:
            raise ValueError('Surface observation exceeds byte budget')
        data = path.read_bytes()
        if len(data) < 24:
            raise ValueError('Truncated surface header')
        magic, engine_frame, width, height = struct.unpack_from('<8sQII', data)
        count = width*height
        if magic != b'SURFACE1' or count == 0 or count > 1920*1080 or len(data) != 24+count*13:
            raise ValueError('Invalid surface header or channel sizes')
        if any(type(metadata.get(k)) is not int or metadata[k] != v for k,v in (('engine_frame',engine_frame),('width',width),('height',height))):
            raise ValueError('Surface/manifest frame identity or dimensions differ')
        timestamp = metadata.get('simulation_time_s')
        if isinstance(timestamp, bool) or not isinstance(timestamp, (int, float)) or not math.isfinite(timestamp) or timestamp < previous_time or engine_frame <= previous_frame:
            raise ValueError('Surface observations require finite ordered clocks and distinct increasing render frames')
        previous_frame, previous_time = engine_frame, timestamp
        matrix = metadata['world_to_clip_row_major']
        if len(matrix) != 16 or any(not isinstance(v,(int,float)) or not math.isfinite(v) for v in matrix):
            raise ValueError('Surface projection must be finite')
        labels = data[24+count*4:24+count*5]
        if set(labels)-{0}-set(subjects.values()):
            raise ValueError('Unregistered raster label')
        planes=[]
        for offset in (24+count*5, 24+count*9):
            plane=array('f'); plane.frombytes(data[offset:offset+count*4])
            if sys.byteorder != 'little': plane.byteswap()
            planes.append(plane)
        hashes[path.name]=hashlib.sha256(data).hexdigest()
        frames.append(dict(metadata=metadata, bgra=data[24:24+count*4], labels=labels,
                           scene_depth_cm=planes[0], label_depth_cm=planes[1]))
    return manifest, frames, hashes
