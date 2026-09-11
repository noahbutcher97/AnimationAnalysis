# Mesh observation records

Portable records are available in `animation_analysis.mesh_records`. Native mesh
acquisition is still pending. These contracts consume explicit producer evidence;
they do not discover Unreal components, detect deformation or establish contact.

## Geometry and identity

`MeshTopology` owns immutable **little-endian uint32 global vertex indices**. Its
`MeshSection` ranges use index-element offsets/counts, are triangle-aligned, and
uniquely cover the entire index buffer in order. Materials are explicit identifiers
or `None` for unknown. `vertex_count`, asset identity, LOD and topology configuration
generation are required. Empty, truncated and out-of-range topology is rejected.
Degenerate/open/nonmanifold geometry is not repaired or certified by this contract.

Topology identity is SHA-256 of canonical ASCII-escaped, sorted-key, compact JSON
from `MeshTopology.metadata()`, followed by one zero byte and the exact index bytes.
Metadata includes asset/configuration/LOD, vertex count, index encoding and ordered
section/material mapping. Changing any of these invalidates the identity. No asset
path, installation location or inferred anatomy enters the identity automatically.

`MeshObservation` owns immutable **little-endian float64 XYZ positions**, exactly
24 bytes per vertex, and refers to a validated topology. `positions()` and
`topology.indices()` iterate values without expanding the whole payload into Python
objects. Float64 is a storage encoding, not a claim of measurement accuracy.

Supply units, coordinate-system identity, row/column vector convention, finite affine
component-to-world matrix, component generation, producer/configuration identity,
`PoseKey` and acquisition `ClockStamp`. Coordinates remain in component space. The
matrix uses row-major storage; `vector_convention` determines multiplication.
Configuration identity must change when effective weights or deformation settings
change, even if triangle bytes do not. The library cannot discover these changes.

`MeshRequest` preserves request/component/configuration identities and the original
acquisition witness. Pose and acquisition clock may both be `None` when acquisition
never occurred; they cannot be independently missing. An optional topology identity
constrains the result. `MeshCompletion` separates completion time and terminal status
from that request. `completed` requires a matching observation; `failed`, `cancelled`,
`timed_out` and `unavailable` require a reason and contain no geometry. Distinct clock
domains are retained without subtraction. Completion cannot precede acquisition in
the same domain. These records do not implement a queue or enforce exactly-once
termination across multiple records.

## Coverage eligibility

`FeatureCoverage` applies to **every included section**. A producer must report
mixed or uncertain coverage conservatively, or emit separately scoped observations
with their own topology identities. One successful section cannot qualify another.
Each feature has a state, producer identity, evidence identity and reason:

- `observed`: the named feature was acquired at the declared stage.
- `inactive`: a producer witness establishes no active contribution.
- `unsupported`, `unknown`, `excluded`: preserve the specific limitation.

Identifiers are supplied provenance, not independent verification of a producer's
claim. Unfamiliar state values and duplicate features are rejected. Feature names are
producer-neutral strings; readers preserve them rather than infer their meaning.

`assess_mesh_coverage(observation, requirement)` returns `MeshEligibility` with
structured reason strings. A `MeshRequirement` declares the component/generation,
topology/configuration, pose, units/coordinate system, allowed producers, understood
feature vocabulary, required features and allowed exclusions. Required features
must be observed or witnessed inactive. Missing or unfamiliar coverage is insufficient.
An unrequired limitation is acceptable only through an explicit allowed exclusion.
An excluded cloth contribution can support a requested bone reference but cannot
satisfy a requirement including cloth. Eligibility is unrelated to artistic quality
or physical contact, and does not imply rendered visibility.

## Bounds and compatibility

Every geometry constructor requires `MeshRecordLimits`: vertex, index and section
counts, maximum payload bytes, metadata bytes and combined record bytes. Binary
buffer size/count limits are checked before making an owned immutable copy. Metadata
and identities are also bounded; text fields are at most 256 characters, feature
lists at most 64, and integer metadata is limited to exact JSON-safe integers.

These limits describe **encoded data and counts**, not process RSS. Python objects,
metadata parsing/serialization and temporary copies add overhead. Native GPU/CPU,
topology-cache, image/export and concurrency admission remain separate implementation
work and must share a combined budget when delivered. No unbounded topology cache
is introduced by the portable records.

Existing `ClockStamp`, `PoseKey`, visual evidence, motion, RGB and depth meanings
remain unchanged. These types add an explicit mesh contract; they do not upgrade
legacy records or silently fall back from a requested deformation capability.
