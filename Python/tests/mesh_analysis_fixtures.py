"""Explicit synthetic surfaces; no engine, anatomy or producer defaults in the API."""
import struct

from animation_analysis import (ClockStamp, FeatureCoverage, MeshObservation,
                               MeshRecordLimits, MeshRequirement, MeshSection,
                               MeshTopology, PoseKey)
from animation_analysis.mesh_analysis import (MeshAnalysisLimits, MeshPairSample,
                                            MeshRegion, MeshSelection)


LIMITS = MeshAnalysisLimits(50000, 100000, 200000)
RECORD_LIMITS = MeshRecordLimits(150000, 300000, 32, 8_000_000, 65536, 8_065_536)
IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
PANEL = ((0, 0, 0), (2, 0, 0), (0, 2, 0))


def observation(name='a', points=PANEL, triangles=((0, 1, 2),), *, time=0,
                transform=IDENTITY, convention='row', coverage=None, **changes):
    indices = tuple(i for triangle in triangles for i in triangle)
    topology = MeshTopology(name+'-asset', 0, 0, len(points),
                            struct.pack('<'+'I'*len(indices), *indices),
                            (MeshSection('surface', 0, len(indices), 'material'),), RECORD_LIMITS)
    values = dict(component_id=name, component_generation=0, topology=topology,
                  position_data=struct.pack('<'+'d'*(3*len(points)), *(v for p in points for v in p)),
                  units='centimetres', coordinate_system='shared-world', vector_convention=convention,
                  component_to_world=transform, pose=PoseKey(name, 'capture', int(time*100), int(time*100)),
                  acquired=ClockStamp('simulation', time), producer_id='synthetic-v1', configuration_id='config-1',
                  coverage=coverage if coverage is not None else (FeatureCoverage('bone', 'observed', 'fixture', 'known-pose', ''),),
                  limits=RECORD_LIMITS)
    return MeshObservation(**(values | changes))


def selection(obs, ids=None, *, region_id='surface', required=('bone',), excluded=()):
    region = MeshRegion(region_id, obs.topology.identity,
                        tuple(range(len(obs.topology.index_data)//12)) if ids is None else ids, LIMITS)
    requirement = MeshRequirement(obs.component_id, obs.component_generation, obs.topology.identity,
                                  obs.configuration_id, obs.pose, obs.units, obs.coordinate_system,
                                  required, ('bone', 'morph', 'cloth', 'material_displacement', 'raster_visibility'),
                                  excluded, ('synthetic-v1',))
    return MeshSelection(obs, region, requirement)


def pair(first=None, second=None, *, sample_id='sample-0', time=0):
    if first is None:
        first = selection(observation(time=time))
    if second is None:
        second = selection(observation('b', ((0,0,3), (2,0,3), (0,2,3)), time=time))
    return MeshPairSample(sample_id, ClockStamp('simulation', time), first, second)
