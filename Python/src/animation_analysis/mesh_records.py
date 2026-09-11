"""Immutable mesh evidence; coverage is declared by producers, never inferred here."""
from dataclasses import dataclass, field
import hashlib
import json
import re
import struct

from .contracts import ClockStamp, PoseKey, _finite


def _text(value, label, *, empty=False):
    if not isinstance(value, str) or len(value) > 256 or (not empty and not value.strip()):
        raise ValueError(f"Expected {label} text of at most 256 characters")
    try:
        value.encode('utf-8')
    except UnicodeError as error:
        raise ValueError(f"Invalid {label} text") from error


def _integer(value, label, minimum=0):
    if type(value) is not int or not minimum <= value <= 2**53-1:
        raise ValueError(f"Expected bounded integer {label}")


def _hash(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('Expected SHA-256 identity')


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('utf-8')


def _pose(value):
    if not isinstance(value, PoseKey):
        raise ValueError('Expected PoseKey')
    _text(value.subject_id, 'subject'); _text(value.stream_id, 'stream')
    _integer(value.frame_id, 'pose frame'); _integer(value.revision, 'pose revision')


def _clock(value):
    if not isinstance(value, ClockStamp):
        raise ValueError('Expected ClockStamp')
    _text(value.domain, 'clock domain')


def _buffer(value, length, limits):
    if not isinstance(limits, MeshRecordLimits):
        raise ValueError('Explicit MeshRecordLimits required')
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise ValueError('Expected contiguous binary buffer')
    view = memoryview(value)
    if not view.c_contiguous or (length is not None and view.nbytes != length) or view.nbytes > limits.max_payload_bytes:
        raise ValueError('Buffer length exceeds limits or differs from declared count')
    return view


@dataclass(frozen=True)
class MeshRecordLimits:
    """Encoded counts/bytes, not a process RSS or total interpreter allocation limit."""
    max_vertices: int
    max_indices: int
    max_sections: int
    max_payload_bytes: int
    max_metadata_bytes: int
    max_record_bytes: int

    def __post_init__(self):
        for name in self.__dataclass_fields__:
            _integer(getattr(self, name), name, 1)

    def check(self, vertices, indices, sections, payload_bytes, metadata_bytes):
        for value, maximum in ((vertices,self.max_vertices),(indices,self.max_indices),
                               (sections,self.max_sections),(payload_bytes,self.max_payload_bytes),
                               (metadata_bytes,self.max_metadata_bytes),
                               (payload_bytes+metadata_bytes,self.max_record_bytes)):
            if value > maximum:
                raise ValueError('Mesh record exceeds explicit limits')


@dataclass(frozen=True)
class MeshSection:
    section_id: str
    first_index: int
    index_count: int
    material_id: str | None

    def __post_init__(self):
        _text(self.section_id, 'section identity')
        _integer(self.first_index, 'first index'); _integer(self.index_count, 'index count', 3)
        if self.first_index % 3 or self.index_count % 3:
            raise ValueError('Sections require complete triangles')
        if self.material_id is not None:
            _text(self.material_id, 'material identity')

    def to_mapping(self):
        return dict(section_id=self.section_id,first_index=self.first_index,
                    index_count=self.index_count,material_id=self.material_id)


@dataclass(frozen=True)
class MeshTopology:
    asset_id: str
    configuration_generation: int
    lod: int
    vertex_count: int
    index_data: bytes
    sections: tuple
    limits: MeshRecordLimits = field(repr=False, compare=False)
    identity: str = field(init=False)

    def __post_init__(self):
        _text(self.asset_id, 'asset identity')
        _integer(self.configuration_generation, 'topology generation')
        _integer(self.lod, 'LOD'); _integer(self.vertex_count, 'vertex count', 1)
        if not isinstance(self.sections,(tuple,list)) or not self.sections:
            raise ValueError('Explicit nonempty sections required')
        view = _buffer(self.index_data, None, self.limits)
        if view.nbytes == 0 or view.nbytes % 12:
            raise ValueError('Index buffer requires uint32 triangles')
        count = view.nbytes // 4
        self.limits.check(self.vertex_count,count,len(self.sections),view.nbytes,0)
        sections = tuple(self.sections)
        end, names = 0, set()
        for section in sections:
            if not isinstance(section,MeshSection) or section.section_id in names or section.first_index != end:
                raise ValueError('Sections must uniquely and exhaustively partition the index buffer')
            names.add(section.section_id); end += section.index_count
        if end != count:
            raise ValueError('Incomplete section coverage')
        object.__setattr__(self,'sections',sections)
        metadata = _json(self.metadata())
        self.limits.check(self.vertex_count,count,len(sections),view.nbytes,len(metadata))
        data = bytes(view)
        if any(i[0] >= self.vertex_count for i in struct.iter_unpack('<I',data)):
            raise ValueError('Index outside vertex buffer')
        object.__setattr__(self,'index_data',data)
        digest = hashlib.sha256(metadata + b'\0' + data).hexdigest()
        object.__setattr__(self,'identity',digest)

    def metadata(self):
        return dict(asset_id=self.asset_id,configuration_generation=self.configuration_generation,lod=self.lod,
                    vertex_count=self.vertex_count,index_encoding='uint32-le-global',
                    sections=[s.to_mapping() for s in self.sections])

    def indices(self):
        return (row[0] for row in struct.iter_unpack('<I',self.index_data))


@dataclass(frozen=True)
class FeatureCoverage:
    """State applies to every included section. A witness identifier is provenance, not proof by itself."""
    feature: str
    state: str
    producer_id: str
    evidence_id: str
    reason: str

    def __post_init__(self):
        for name in ('feature','producer_id','evidence_id'):
            _text(getattr(self,name),name)
        if self.state not in ('observed','inactive','unsupported','unknown','excluded'):
            raise ValueError('Unsupported coverage state')
        _text(self.reason,'coverage reason',empty=self.state == 'observed')

    def to_mapping(self):
        return {name:getattr(self,name) for name in self.__dataclass_fields__}


@dataclass(frozen=True)
class MeshObservation:
    component_id: str
    component_generation: int
    topology: MeshTopology
    position_data: bytes
    units: str
    coordinate_system: str
    vector_convention: str
    component_to_world: tuple
    pose: PoseKey
    acquired: ClockStamp
    producer_id: str
    configuration_id: str
    coverage: tuple
    limits: MeshRecordLimits = field(repr=False,compare=False)

    def __post_init__(self):
        for name in ('component_id','units','coordinate_system','producer_id','configuration_id'):
            _text(getattr(self,name),name)
        _integer(self.component_generation,'component generation'); _pose(self.pose); _clock(self.acquired)
        if not isinstance(self.topology,MeshTopology):
            raise ValueError('Expected MeshTopology')
        if not isinstance(self.coverage,(tuple,list)) or len(self.coverage) > 64:
            raise ValueError('Coverage must contain at most 64 features')
        coverage = tuple(self.coverage)
        if any(not isinstance(c,FeatureCoverage) for c in coverage) or len({c.feature for c in coverage}) != len(coverage):
            raise ValueError('Duplicate or invalid feature coverage')
        matrix = self.component_to_world
        if not isinstance(matrix,(tuple,list)) or len(matrix) != 16 or not all(_finite(v) for v in matrix):
            raise ValueError('Expected finite 4x4 transform')
        if self.vector_convention not in ('row','column'):
            raise ValueError('Explicit row/column vector convention required')
        zero = (3,7,11) if self.vector_convention == 'row' else (12,13,14)
        if matrix[15] != 1 or any(matrix[i] != 0 for i in zero):
            raise ValueError('Expected affine component-to-world transform')
        object.__setattr__(self,'component_to_world',tuple(matrix)); object.__setattr__(self,'coverage',coverage)
        view = _buffer(self.position_data,self.topology.vertex_count*24,self.limits)
        payload = view.nbytes + len(self.topology.index_data)
        self.limits.check(self.topology.vertex_count,len(self.topology.index_data)//4,len(self.topology.sections),
                          payload,len(_json(self.metadata()))+len(_json(self.topology.metadata())))
        data = bytes(view)
        if any(not _finite(v[0]) for v in struct.iter_unpack('<d',data)):
            raise ValueError('Positions must be finite')
        object.__setattr__(self,'position_data',data)

    def positions(self):
        return struct.iter_unpack('<3d',self.position_data)

    def metadata(self):
        return dict(component_id=self.component_id,component_generation=self.component_generation,
                    topology_id=self.topology.identity,units=self.units,coordinate_system=self.coordinate_system,
                    vector_convention=self.vector_convention,component_to_world=list(self.component_to_world),
                    pose={name:getattr(self.pose,name) for name in self.pose.__dataclass_fields__},
                    acquired=dict(domain=self.acquired.domain,seconds=self.acquired.seconds),
                    producer_id=self.producer_id,configuration_id=self.configuration_id,
                    coverage=[c.to_mapping() for c in self.coverage],position_encoding='xyz-float64-le')


@dataclass(frozen=True)
class MeshRequest:
    request_id: str
    component_id: str
    component_generation: int
    configuration_id: str
    pose: PoseKey | None
    acquired: ClockStamp | None
    topology_id: str | None

    def __post_init__(self):
        for name in ('request_id','component_id','configuration_id'):
            _text(getattr(self,name),name)
        _integer(self.component_generation,'component generation')
        if (self.pose is None) != (self.acquired is None):
            raise ValueError('Acquisition pose and clock must both be known or both absent')
        if self.pose is not None:
            _pose(self.pose); _clock(self.acquired)
        if self.topology_id is not None:
            _hash(self.topology_id)


@dataclass(frozen=True)
class MeshCompletion:
    request: MeshRequest
    status: str
    completed: ClockStamp
    reason: str
    observation: MeshObservation | None = None

    def __post_init__(self):
        if not isinstance(self.request,MeshRequest):
            raise ValueError('Immutable request identity required')
        _clock(self.completed)
        acquired = self.request.acquired
        if acquired is not None and acquired.domain == self.completed.domain and self.completed.seconds < acquired.seconds:
            raise ValueError('Completion precedes acquisition in the same clock domain')
        if self.status not in ('completed','failed','cancelled','timed_out','unavailable'):
            raise ValueError('Unsupported terminal status')
        _text(self.reason,'completion reason',empty=self.status == 'completed')
        if self.status == 'completed':
            sample = self.observation
            if not isinstance(sample,MeshObservation):
                raise ValueError('Completed result requires observation')
            for name in ('component_id','component_generation','configuration_id','pose','acquired'):
                if getattr(self.request,name) != getattr(sample,name):
                    raise ValueError('Observation differs from request identity')
            if self.request.topology_id is not None and self.request.topology_id != sample.topology.identity:
                raise ValueError('Observation topology differs from request')
        elif self.observation is not None:
            raise ValueError('Unsuccessful completion cannot contain an observation')


def _names(value, label, *, empty=False):
    if not isinstance(value,(tuple,list)) or len(value) > 64 or (not value and not empty):
        raise ValueError(f'Expected explicit bounded {label}')
    for item in value:
        _text(item,label)
    if len(set(value)) != len(value):
        raise ValueError(f'Duplicate {label}')
    return tuple(value)


@dataclass(frozen=True)
class MeshRequirement:
    component_id: str
    component_generation: int
    topology_id: str
    configuration_id: str
    pose: PoseKey
    units: str
    coordinate_system: str
    required_features: tuple
    known_features: tuple
    allowed_exclusions: tuple
    allowed_producers: tuple

    def __post_init__(self):
        for name in ('component_id','configuration_id','units','coordinate_system'):
            _text(getattr(self,name),name)
        _integer(self.component_generation,'component generation'); _pose(self.pose); _hash(self.topology_id)
        for name in ('required_features','known_features','allowed_exclusions','allowed_producers'):
            object.__setattr__(self,name,_names(getattr(self,name),name,empty=name=='allowed_exclusions'))
        if not set(self.required_features+self.allowed_exclusions) <= set(self.known_features):
            raise ValueError('Requirements reference unknown features')
        if set(self.required_features) & set(self.allowed_exclusions):
            raise ValueError('Required features cannot be excluded')


@dataclass(frozen=True)
class MeshEligibility:
    eligible: bool
    reasons: tuple


def assess_mesh_coverage(observation, requirement):
    """Evaluate explicit evidence requirements; never infer physical contact or quality."""
    if not isinstance(observation,MeshObservation) or not isinstance(requirement,MeshRequirement):
        raise ValueError('Expected mesh observation and requirement')
    reasons = []
    for name in ('component_id','component_generation','configuration_id','pose','units','coordinate_system'):
        if getattr(observation,name) != getattr(requirement,name):
            reasons.append(name+':mismatch')
    if observation.topology.identity != requirement.topology_id:
        reasons.append('topology:mismatch')
    if observation.producer_id not in requirement.allowed_producers:
        reasons.append('producer:unsupported')
    coverage = {c.feature:c for c in observation.coverage}
    for feature in requirement.required_features:
        if feature not in coverage:
            reasons.append(feature+':missing')
    for feature,item in coverage.items():
        if feature not in requirement.known_features:
            reasons.append(feature+':unfamiliar')
        elif item.state not in ('observed','inactive') and not (item.state == 'excluded' and feature in requirement.allowed_exclusions):
            reasons.append(feature+':'+item.state)
    return MeshEligibility(not reasons,tuple(reasons))
