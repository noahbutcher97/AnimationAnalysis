#!/usr/bin/env python3
"""Qualify one explicit native GPU mesh replay and its matching SURFACE1 raster."""
from array import array
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

from animation_analysis.mesh_records import MeshRecordLimits
from animation_analysis.mesh_replay import read_mesh_observation


LIMITS = MeshRecordLimits(2_000_000, 12_000_000, 256, 100_000_000, 65_536, 100_065_536)
VIEW_FIELDS = {'format','schema_version','engine_frame','renderer_frame','world_to_clip_row_major',
               'label','topology_id','configuration_id','producer_id','lod','render_pass_id'}
PRODUCER = 'unreal-skin-cache-bone-v1'
RENDER_PASS = 'viewport-main-depth-v1'
MAX_PIXELS = 1920 * 1080


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def _json(path):
    try:
        with Path(path).open('rb') as stream:
            data = stream.read(65_537)
        if len(data) > 65_536:
            raise ValueError('view metadata exceeds 64 KiB')
        value = json.loads(data.decode('utf-8'), object_pairs_hook=_pairs,
            parse_constant=lambda token: (_ for _ in ()).throw(ValueError(f'nonfinite JSON number: {token}')))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f'cannot read view metadata: {error}') from error
    return value, data


def _mesh(bundle):
    bundle = Path(bundle)
    if not bundle.name or not bundle.parent.is_dir():
        raise ValueError('mesh must name an explicit bundle directory')
    completion = read_mesh_observation(bundle.parent, bundle.name, limits=LIMITS)
    sample = completion.observation
    if completion.status != 'completed' or sample is None:
        raise ValueError('mesh replay must contain a completed immutable snapshot')
    if sample.producer_id != PRODUCER:
        raise ValueError(f'mesh producer id must be {PRODUCER}')
    coverage = {item.feature:item for item in sample.coverage}
    if ('bone' not in coverage or coverage['bone'].state != 'observed' or
            coverage['bone'].producer_id != sample.producer_id):
        raise ValueError('mesh replay requires observed bone coverage')
    if (sample.units, sample.coordinate_system, sample.vector_convention) != (
            'centimetres','unreal-left-handed-z-up','row'):
        raise ValueError('mesh replay has unsupported units or coordinate convention')
    return completion, sample


def _surface(path):
    path = Path(path)
    maximum = 24 + MAX_PIXELS * 13
    try:
        with path.open('rb') as stream:
            data = stream.read(maximum + 1)
    except OSError as error:
        raise ValueError(f'cannot read surface: {error}') from error
    if len(data) < 24 or len(data) > maximum:
        raise ValueError('surface is truncated or exceeds the raster bound')
    magic, frame, width, height = struct.unpack_from('<8sQII', data)
    count = width * height
    if magic != b'SURFACE1' or not count or count > MAX_PIXELS or len(data) != 24 + count * 13:
        raise ValueError('invalid SURFACE1 header or channel sizes')
    labels = data[24+count*4:24+count*5]
    planes = []
    for offset in (24+count*5, 24+count*9):
        values = array('f'); values.frombytes(data[offset:offset+count*4])
        if sys.byteorder != 'little': values.byteswap()
        if any(math.isnan(value) or value <= 0 for value in values):
            raise ValueError('surface depth planes require positive centimetres or positive infinity for clear depth')
        planes.append(values)
    return {'frame':frame, 'width':width, 'height':height, 'labels':labels,
            'scene':planes[0], 'labelled':planes[1], 'sha256':hashlib.sha256(data).hexdigest()}


def _view(path):
    value, data = _json(path)
    if not isinstance(value, dict) or set(value) != VIEW_FIELDS or value['format'] != 'gpu_mesh_raster_view' or value['schema_version'] != 1:
        raise ValueError('view metadata has missing fields or unsupported schema')
    for field in ('engine_frame','renderer_frame','lod','label'):
        if type(value[field]) is not int or value[field] < 0 or value[field] > 2**53-1:
            raise ValueError(f'view {field.replace("_", " ")} must be a bounded integer')
    matrix = value['world_to_clip_row_major']
    if not isinstance(matrix, list) or len(matrix) != 16 or any(type(item) not in (int,float) or not math.isfinite(item) for item in matrix):
        raise ValueError('view projection must contain 16 finite row-major values')
    for field in ('topology_id','configuration_id','producer_id','render_pass_id'):
        if not isinstance(value[field], str) or not value[field]:
            raise ValueError(f'view {field.replace("_", " ")} must be explicit text')
    value['_sha256'] = hashlib.sha256(data).hexdigest()
    return value


def _transform(point, matrix):
    source = (*point, 1.0)
    return tuple(sum(source[row] * matrix[row*4+column] for row in range(4)) for column in range(4))


def _rasterize(sample, matrix, width, height, translate_y=0.0):
    world = [_transform(point, sample.component_to_world)[:3] for point in sample.positions()]
    projected = []
    for point in world:
        clip = _transform((point[0], point[1] + translate_y, point[2]), matrix)
        if clip[3] <= 0:
            raise ValueError('mesh crosses or lies behind the camera plane')
        projected.append(((clip[0]/clip[3]+1)*width/2, (1-clip[1]/clip[3])*height/2, clip[3]))
    indices = list(sample.topology.indices())
    mask = bytearray(width*height)
    depths = [math.inf] * (width*height)
    for offset in range(0, len(indices), 3):
        a,b,c = [projected[index] for index in indices[offset:offset+3]]
        area = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])
        if area >= -1e-9:
            continue
        x0=max(0, math.ceil(min(a[0],b[0],c[0])-.5)); x1=min(width-1, math.floor(max(a[0],b[0],c[0])-.5))
        y0=max(0, math.ceil(min(a[1],b[1],c[1])-.5)); y1=min(height-1, math.floor(max(a[1],b[1],c[1])-.5))
        for y in range(y0,y1+1):
            for x in range(x0,x1+1):
                u=((b[0]-x-.5)*(c[1]-y-.5)-(b[1]-y-.5)*(c[0]-x-.5))/area
                v=((c[0]-x-.5)*(a[1]-y-.5)-(c[1]-y-.5)*(a[0]-x-.5))/area
                w=1-u-v
                if min(u,v,w) < -1e-8: continue
                depth=1/(u/a[2]+v/b[2]+w/c[2]); index=y*width+x
                if depth < depths[index]: mask[index]=1; depths[index]=depth
    return mask, depths


def _iou(first, second):
    intersection = sum(bool(a) and bool(b) for a,b in zip(first,second))
    union = sum(bool(a) or bool(b) for a,b in zip(first,second))
    if not union: raise ValueError('silhouette union is empty')
    return intersection/union


def verify(mesh, surface, view, *, mesh_only):
    completion, sample = _mesh(mesh)
    result = {'format':'native_gpu_mesh_verification', 'schema_version':1,
              'mode':'mesh-only' if mesh_only else 'mesh-raster',
              'mesh':{'vertex_count':sample.topology.vertex_count,
                      'index_count':len(sample.topology.index_data)//4,
                      'section_count':len(sample.topology.sections), 'lod':sample.topology.lod,
                      'topology_sha256':sample.topology.identity,
                      'positions_sha256':hashlib.sha256(sample.position_data).hexdigest(),
                      'indices_sha256':hashlib.sha256(sample.topology.index_data).hexdigest(),
                      'producer_id':sample.producer_id, 'engine_frame':sample.pose.frame_id,
                      'pose_revision':sample.pose.revision}}
    if mesh_only:
        if surface is not None or view is not None: raise ValueError('mesh-only does not accept surface or view sidecars')
        result['passed'] = True
        return result
    if surface is None or view is None: raise ValueError('mesh-raster verification requires explicit surface and view paths')
    raster = _surface(surface); metadata = _view(view)
    if metadata['engine_frame'] != sample.pose.frame_id or raster['frame'] != sample.pose.frame_id:
        raise ValueError('engine frame differs across mesh acquisition, view and surface')
    if metadata['producer_id'] != sample.producer_id: raise ValueError('producer id differs between mesh and view')
    if metadata['topology_id'] != sample.topology.identity: raise ValueError('topology id differs between mesh and view')
    if metadata['configuration_id'] != sample.configuration_id: raise ValueError('configuration id differs between mesh and view')
    if metadata['lod'] != sample.topology.lod: raise ValueError('lod differs between mesh and view')
    if metadata['render_pass_id'] != RENDER_PASS: raise ValueError(f'render pass id must be {RENDER_PASS}')
    if metadata['label'] != 7: raise ValueError('label must be the qualified value 7')
    # Other enrolled primitives may carry their own labels; this oracle qualifies label 7 only.

    expected, depth = _rasterize(sample, metadata['world_to_clip_row_major'], raster['width'], raster['height'])
    observed = bytearray(value == metadata['label'] for value in raster['labels'])
    silhouette_iou = _iou(expected, observed)
    errors_scene=[]; errors_label=[]; width=raster['width']; height=raster['height']
    for y in range(1,height-1):
        for x in range(1,width-1):
            index=y*width+x
            neighbors=(index,index-1,index+1,index-width,index+width)
            if all(expected[item] and observed[item] for item in neighbors):
                if not math.isfinite(raster['scene'][index]) or not math.isfinite(raster['labelled'][index]):
                    raise ValueError('shared labelled interior has clear or nonfinite depth')
                errors_scene.append(abs(depth[index]-raster['scene'][index]))
                errors_label.append(abs(depth[index]-raster['labelled'][index]))
    if not errors_scene: raise ValueError('no shared interior pixels exist for depth comparison')
    shifted, _ = _rasterize(sample, metadata['world_to_clip_row_major'], raster['width'], raster['height'], 20.0)
    wrong_iou = _iou(shifted, observed)
    result.update({'identity':{'engine_frame':sample.pose.frame_id, 'renderer_frame':metadata['renderer_frame'],
                               'render_pass_id':metadata['render_pass_id'], 'view_sha256':metadata['_sha256'],
                               'surface_sha256':raster['sha256']},
                   'silhouette':{'iou':silhouette_iou, 'minimum':.99},
                   'depth':{'interior_samples':len(errors_scene), 'max_scene_error_cm':max(errors_scene),
                            'max_label_error_cm':max(errors_label), 'maximum_cm':.05},
                   'wrong_pose':{'translation_world_y_cm':20.0, 'iou':wrong_iou, 'maximum_exclusive':.9}})
    result['passed'] = silhouette_iou >= .99 and max(errors_scene) <= .05 and max(errors_label) <= .05 and wrong_iou < .9
    return result


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh', required=True, help='explicit schema-1 mesh bundle directory')
    parser.add_argument('--surface', help='explicit SURFACE1 file')
    parser.add_argument('--view', help='explicit gpu_mesh_raster_view JSON file')
    parser.add_argument('--mesh-only', action='store_true')
    parser.add_argument('--output', required=True, help='qualification JSON output')
    arguments=parser.parse_args(argv)
    try:
        result=verify(arguments.mesh, arguments.surface, arguments.view, mesh_only=arguments.mesh_only)
        encoded=json.dumps(result,sort_keys=True,separators=(',',':'),allow_nan=False)+'\n'
        output=Path(arguments.output)
        if not output.parent.is_dir(): raise ValueError('output parent directory must already exist')
        with output.open('x', encoding='utf-8') as stream:
            stream.write(encoded)
        print(encoded,end='')
        return 0 if result['passed'] else 1
    except (OSError,ValueError,TypeError,OverflowError) as error:
        sys.stderr.write(f'native GPU mesh verification failed: {error}\n')
        return 1


if __name__ == '__main__': raise SystemExit(main())
