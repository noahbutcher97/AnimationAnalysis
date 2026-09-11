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

## Replay schema 1

`animation_analysis.mesh_replay.write_mesh_observation(root, relative_record,
completion, limits=...)` exclusively reserves a new bundle directory. Its companion
`read_mesh_observation(root, relative_record, limits=...)` returns a `MeshCompletion`.
The root and any intermediate directories must already exist. Both APIs raise
`EvidenceError` (a `ValueError`) for invalid/incomplete evidence or I/O failure.

| File | Meaning |
|---|---|
| `record.json` | `format: "mesh_observation"`, integer `schema_version: 1`, request, status, completion clock, reason, observation metadata, topology metadata and buffer descriptors. |
| `positions.bin` | Float64 XYZ tuples in component space; exactly `vertex_count * 24` bytes. Present only for completed geometry. |
| `indices.bin` | Global uint32 vertex indices; exactly four bytes per index in the section ranges. Present only for completed geometry. |
| `complete.json` | `format: "mesh_observation_commit"`, integer `schema_version: 1`, and `files` mapping each preceding file to `size_bytes` and lowercase SHA-256. |

The request object contains the exact `MeshRequest` fields. Clocks have exactly
`domain` and `seconds`; poses have exactly `subject_id`, `stream_id`, `frame_id`,
`revision`. Observation metadata is `MeshObservation.metadata()`; topology metadata
is `MeshTopology.metadata()`. The observation's topology identity must equal the
recomputed topology hash. Its acquisition/component/configuration must match the
request. `buffers` duplicates the committed position/index descriptors and must match
them exactly. Failure records use `null` observation/topology and empty buffers.

The reader rejects duplicate JSON keys, unknown schema fields/versions/encodings,
nonfinite geometry, malformed counts, incomplete sections, inconsistent identities,
oversized/truncated data and hash mismatches. It caps actual file reads and checks
the exact bytes it parses. It validates declared counts and combined payload limits
before loading geometry. Replay metadata limits include **both JSON files**; combined
record bytes include metadata plus both binary buffers. A constructor-valid object
can therefore exceed a tighter replay limit once envelope overhead is included.

The writer checks limits before reserving the directory, writes/flushed payloads and
metadata, then publishes a complete marker with an exclusive atomic hard link from
`complete.pending`. The filesystem must support hard links. A competing writer cannot
reuse the directory; an existing marker is never replaced. Interrupted writes leave
an incomplete directory for inspection and cannot be read as completed evidence.
A leftover `complete.pending` after successful publication is harmless and ignored.
This supplies atomic visibility, not a guarantee against machine/filesystem failure.

Paths use portable ASCII components with forward slashes. Absolute/escaping paths,
drive or alternate-stream syntax, reserved Windows names, symlinks and reparse points
are rejected. The caller must provide a **stable trusted root**: protection against
hostile concurrent filesystem path replacement is outside this API. Hashes establish
integrity within the bundle, not authenticity of the producer's claims. Only listed
files are consumed; the reader does not discover adjacent assets or cache topology.

## Explicit-input example

```python
import struct
from pathlib import Path
from animation_analysis import ClockStamp, PoseKey
from animation_analysis.mesh_records import (
    FeatureCoverage, MeshCompletion, MeshObservation, MeshRecordLimits,
    MeshRequest, MeshRequirement, MeshSection, MeshTopology, assess_mesh_coverage,
)
from animation_analysis.mesh_replay import read_mesh_observation, write_mesh_observation

limits = MeshRecordLimits(100, 300, 10, 4096, 16384, 20480)
topology = MeshTopology("panel-asset", 1, 0, 3, struct.pack("<3I", 0, 1, 2),
                        (MeshSection("face", 0, 3, "steel"),), limits)
sample = MeshObservation(
    component_id="panel", component_generation=1, topology=topology,
    position_data=struct.pack("<9d", 0, 0, 0, 1, 0, 0, 0, 1, 0),
    units="metres", coordinate_system="right-handed-z-up", vector_convention="row",
    component_to_world=(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1),
    pose=PoseKey("assembly", "inspection", 12, 8), acquired=ClockStamp("simulation", 1.25),
    producer_id="rigid-reference-v1", configuration_id="opaque-lod0",
    coverage=(FeatureCoverage("rigid", "observed", "fixture-v1", "sample-12", ""),),
    limits=limits,
)
request = MeshRequest("request-12", "panel", 1, sample.configuration_id,
                      sample.pose, sample.acquired, topology.identity)
result = MeshCompletion(request, "completed", ClockStamp("wall", 300), "", sample)
root = Path("mesh-evidence")
root.mkdir(exist_ok=True)
# Use a fresh directory name for each observation; reusing this one rejects.
write_mesh_observation(root, "sample-12", result, limits=limits)
replayed = read_mesh_observation(root, "sample-12", limits=limits)
criteria = MeshRequirement("panel", 1, topology.identity, "opaque-lod0", sample.pose,
                           "metres", "right-handed-z-up", ("rigid",), ("rigid",), (),
                           ("rigid-reference-v1",))
assert assess_mesh_coverage(replayed.observation, criteria).eligible
```
