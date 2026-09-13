"""Bounded offline measurements of explicitly eligible triangle surfaces."""
from dataclasses import asdict, dataclass, field
from fractions import Fraction
import hashlib
import json
import math
import struct
import time

from .contracts import ClockStamp, PoseKey
from .mesh_records import FeatureCoverage, MeshObservation, MeshRequirement, assess_mesh_coverage
from ._mesh_search import SearchLimitError, nearest_triangles
from ._triangle_geometry import is_degenerate


METHOD_ID = 'exact-triangle-surfaces-v1'


def _name(value):
    if not isinstance(value, str) or not value.strip() or len(value) > 256:
        raise ValueError('Expected an explicit name of at most 256 characters')


def _count(value, minimum=1):
    if type(value) is not int or not minimum <= value <= 2**53-1:
        raise ValueError('Expected a bounded integer count')


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode()


@dataclass(frozen=True)
class MeshAnalysisLimits:
    """Count/operation limits, not a wall-time or interpreter RSS guarantee."""
    max_triangles: int
    max_pair_tests: int
    max_node_visits: int
    max_coordinate_bits: int = 256

    def __post_init__(self):
        for name in ('max_triangles', 'max_pair_tests', 'max_node_visits'):
            _count(getattr(self, name))
        if type(self.max_coordinate_bits) is not int or not 8 <= self.max_coordinate_bits <= 256:
            raise ValueError('Coordinate bit limit must be an integer in 8..256')


@dataclass(frozen=True)
class MeshRegion:
    region_id: str
    topology_id: str
    triangle_ids: tuple
    limits: MeshAnalysisLimits = field(repr=False, compare=False)
    identity: str = field(init=False)

    def __post_init__(self):
        _name(self.region_id)
        if not isinstance(self.topology_id, str) or len(self.topology_id) != 64 or any(c not in '0123456789abcdef' for c in self.topology_id):
            raise ValueError('Region requires a canonical topology SHA-256')
        if not isinstance(self.limits, MeshAnalysisLimits):
            raise ValueError('Region requires explicit analysis limits')
        if not isinstance(self.triangle_ids, (tuple, list)) or not 0 < len(self.triangle_ids) <= self.limits.max_triangles:
            raise ValueError('Region requires an explicit bounded nonempty triangle selection')
        for value in self.triangle_ids:
            _count(value, 0)
        ids = tuple(sorted(self.triangle_ids))
        if any(a == b for a, b in zip(ids, ids[1:])):
            raise ValueError('Duplicate region triangle')
        object.__setattr__(self, 'triangle_ids', ids)
        identity = hashlib.sha256(_canonical(dict(region_id=self.region_id, topology_id=self.topology_id, triangle_ids=ids))).hexdigest()
        object.__setattr__(self, 'identity', identity)


@dataclass(frozen=True)
class MeshSelection:
    observation: MeshObservation
    region: MeshRegion
    requirement: MeshRequirement

    def __post_init__(self):
        if not isinstance(self.observation, MeshObservation) or not isinstance(self.region, MeshRegion) or not isinstance(self.requirement, MeshRequirement):
            raise ValueError('Selection requires a mesh observation, region and requirement')


@dataclass(frozen=True)
class MeshPairSample:
    sample_id: str
    acquired: ClockStamp
    first: MeshSelection
    second: MeshSelection

    def __post_init__(self):
        _name(self.sample_id)
        if not isinstance(self.acquired, ClockStamp) or not isinstance(self.first, MeshSelection) or not isinstance(self.second, MeshSelection):
            raise ValueError('Pair requires an acquisition stamp and two explicit selections')
        _name(self.acquired.domain)


@dataclass(frozen=True)
class MeshInputIdentity:
    """Retained provenance without a reference to mesh position/index buffers."""
    component_id: str
    component_generation: int
    topology_id: str
    configuration_id: str
    producer_id: str
    pose: PoseKey
    acquired: ClockStamp
    units: str
    coordinate_system: str
    region_id: str
    region_identity: str
    region_topology_id: str
    observation_identity: str
    coverage: tuple[FeatureCoverage, ...]
    requirement: MeshRequirement
    criteria_identity: str

    def to_mapping(self):
        return asdict(self)

    def configuration_key(self):
        """Stable pair criteria; positions, transforms and pose revisions may vary."""
        return (self.component_id, self.component_generation, self.topology_id,
                self.configuration_id, self.producer_id, self.pose.subject_id,
                self.pose.stream_id, self.units, self.coordinate_system,
                self.region_identity, self.region_topology_id, self.criteria_identity)


@dataclass(frozen=True)
class MeshAnalysisStats:
    first_triangles: int
    second_triangles: int
    triangle_tests: int
    node_visits: int
    elapsed_seconds: float


@dataclass(frozen=True)
class MeshPairResult:
    sample_id: str
    acquired: ClockStamp
    status: str
    reasons: tuple
    first: MeshInputIdentity
    second: MeshInputIdentity
    tolerance: float
    limits: MeshAnalysisLimits
    stats: MeshAnalysisStats
    minimum_distance: float | None = None
    minimum_squared_distance: Fraction | None = None
    first_point: tuple | None = None
    second_point: tuple | None = None
    first_triangle: int | None = None
    second_triangle: int | None = None
    surface_intersection: bool | None = None
    within_tolerance: bool | None = None
    containment: str = field(default='not_evaluated', init=False)

    def to_mapping(self):
        result = asdict(self)
        square = self.minimum_squared_distance
        result['minimum_squared_distance'] = None if square is None else dict(numerator=str(square.numerator), denominator=str(square.denominator))
        return dict(format='mesh_pair_measurement', schema_version=1, method_id=METHOD_ID, **result)


def _input_identity(selection):
    observation, region, requirement = selection.observation, selection.region, selection.requirement
    digest = hashlib.sha256(_canonical(observation.metadata()))
    digest.update(b'\0')
    digest.update(observation.position_data)
    criteria = asdict(requirement)
    criteria['pose'] = {name: criteria['pose'][name] for name in ('subject_id', 'stream_id')}
    for name in ('required_features', 'known_features', 'allowed_exclusions', 'allowed_producers'):
        criteria[name] = sorted(criteria[name])
    return MeshInputIdentity(observation.component_id, observation.component_generation,
                             observation.topology.identity, observation.configuration_id,
                             observation.producer_id, observation.pose, observation.acquired,
                             observation.units, observation.coordinate_system, region.region_id,
                             region.identity, region.topology_id, digest.hexdigest(), observation.coverage, requirement,
                             hashlib.sha256(_canonical(criteria)).hexdigest())


class _Unavailable(Exception):
    pass


def _bounded_fraction(value, bits):
    result = Fraction(value)
    if result.numerator.bit_length() > bits or result.denominator.bit_length() > bits:
        raise _Unavailable('coordinate_bit_limit')
    return result


def _world_triangles(selection, bits):
    observation = selection.observation
    matrix = tuple(_bounded_fraction(v, bits) for v in observation.component_to_world)
    cache, triangles = {}, []
    for triangle_id in selection.region.triangle_ids:
        vertices = struct.unpack_from('<3I', observation.topology.index_data, triangle_id*12)
        for index in vertices:
            if index in cache:
                continue
            point = tuple(_bounded_fraction(v, bits) for v in struct.unpack_from('<3d', observation.position_data, index*24)) + (Fraction(1),)
            if observation.vector_convention == 'row':
                world = tuple(sum(point[r]*matrix[r*4+c] for r in range(4)) for c in range(3))
            else:
                world = tuple(sum(matrix[r*4+c]*point[c] for c in range(4)) for r in range(3))
            cache[index] = tuple(_bounded_fraction(v, bits) for v in world)
        triangle = tuple(cache[index] for index in vertices)
        if is_degenerate(triangle):
            raise _Unavailable(f'degenerate_triangle:{triangle_id}')
        triangles.append((triangle_id, triangle))
    return triangles


def _distance_float(square):
    if square == 0:
        return 0.0
    # Scale before converting: square may underflow while its root is representable.
    exponent = (square.numerator.bit_length()-square.denominator.bit_length())//2
    scale = Fraction(2)**(2*exponent)
    try:
        result = math.ldexp(math.sqrt(float(square/scale)), exponent)
    except (OverflowError, ValueError):
        raise _Unavailable('distance_presentation_range') from None
    if not math.isfinite(result) or result <= 0:
        raise _Unavailable('distance_presentation_range')
    return result


def measure_mesh_pair(sample, *, tolerance, limits):
    """Measure eligible sampled surfaces; tolerance is proximity, never penetration."""
    if not isinstance(sample, MeshPairSample) or not isinstance(limits, MeshAnalysisLimits):
        raise ValueError('Expected a mesh pair and explicit analysis limits')
    try:
        valid_tolerance = type(tolerance) in (int, float) and math.isfinite(tolerance) and tolerance >= 0
    except OverflowError:
        valid_tolerance = False
    if not valid_tolerance:
        raise ValueError('Tolerance must be finite and nonnegative in observation units')
    started = time.perf_counter()
    first, second = _input_identity(sample.first), _input_identity(sample.second)
    nfirst, nsecond = len(sample.first.region.triangle_ids), len(sample.second.region.triangle_ids)
    tested = visited = 0

    def finish(reasons=(), **measurements):
        return MeshPairResult(sample.sample_id, sample.acquired,
                              'insufficient' if reasons else 'measured', tuple(reasons), first, second,
                              tolerance, limits, MeshAnalysisStats(nfirst, nsecond, tested, visited,
                                                                   time.perf_counter()-started), **measurements)

    reasons = []
    for label, selected in (('first', sample.first), ('second', sample.second)):
        observation, region = selected.observation, selected.region
        eligibility = assess_mesh_coverage(observation, selected.requirement)
        reasons.extend(f'{label}:coverage:{reason}' for reason in eligibility.reasons)
        if observation.acquired != sample.acquired:
            reasons.append(f'{label}:acquisition_mismatch')
        if region.topology_id != observation.topology.identity:
            reasons.append(f'{label}:region_topology_mismatch')
        if region.triangle_ids[-1] >= len(observation.topology.index_data)//12:
            reasons.append(f'{label}:region_triangle_out_of_range')
        if len(region.triangle_ids) > limits.max_triangles:
            reasons.append(f'{label}:triangle_limit')
    if first.units != second.units or first.coordinate_system != second.coordinate_system:
        reasons.append('coordinate_convention_mismatch')
    if (first.pose.subject_id, first.pose.stream_id) == (second.pose.subject_id, second.pose.stream_id) and first.pose != second.pose:
        reasons.append('shared_stream_pose_mismatch')
    if reasons:
        return finish(reasons)
    try:
        triangles = []
        for label, selected in (('first', sample.first), ('second', sample.second)):
            try:
                triangles.append(_world_triangles(selected, limits.max_coordinate_bits))
            except _Unavailable as error:
                return finish((f'{label}:{error}',))
        found = nearest_triangles(*triangles, max_pair_tests=limits.max_pair_tests, max_node_visits=limits.max_node_visits)
        tested, visited = found.triangle_tests, found.node_visits
        distance = _distance_float(found.distance_squared)
        return finish(minimum_distance=distance, minimum_squared_distance=found.distance_squared,
                      first_point=tuple(float(v) for v in found.first_point), second_point=tuple(float(v) for v in found.second_point),
                      first_triangle=found.first_triangle, second_triangle=found.second_triangle,
                      surface_intersection=found.distance_squared == 0,
                      within_tolerance=found.distance_squared <= Fraction(tolerance)**2)
    except SearchLimitError as error:
        tested, visited = error.triangle_tests, error.node_visits
        return finish(('work_limit:'+str(error),))
    except _Unavailable as error:
        return finish((str(error),))
