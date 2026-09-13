# Sampled mesh surface analysis

Python 0.4.0 adds portable offline triangle measurements and sampled interval
summaries. Existing mesh replay schema 1 and native capture APIs remain unchanged.
No engine, image decoder or third-party geometry library is required.

## Explicit observations and regions

Use existing `MeshObservation` and `MeshRequirement` records. Requirements state
which producer, pose, topology, units and deformation coverage are acceptable.
`MeshRegion` selects global triangle ordinals, each referring to three consecutive
indices in the topology's index buffer. It owns a sorted, unique, nonempty selection
and hashes its name, topology identity and ordinals. Changing LOD/topology requires
a new mapping; no material section or bone weight is inferred to be an anatomical
region. Triangle sets sharing a boundary will report intersection at that boundary.

Given caller-supplied observations, requirements, triangle selections and acquisition
stamp:

```python
from animation_analysis import (
    MeshAnalysisLimits, MeshPairSample, MeshRegion, MeshSelection, measure_mesh_pair,
)

limits = MeshAnalysisLimits(max_triangles=50_000, max_pair_tests=100_000,
                            max_node_visits=200_000, max_coordinate_bits=256)
first = MeshSelection(first_observation,
    MeshRegion("surface-a", first_observation.topology.identity, first_triangle_ids, limits),
    first_requirement)
second = MeshSelection(second_observation,
    MeshRegion("surface-b", second_observation.topology.identity, second_triangle_ids, limits),
    second_requirement)
result = measure_mesh_pair(MeshPairSample("sample-1", acquisition_stamp, first, second),
                           tolerance=0.05, limits=limits)
document = result.to_mapping()
```

Tolerance uses the declared observation units; this example does not choose units
or acceptable artistic criteria. Both inputs must match the explicit acquisition
stamp, units and coordinate-system identity. Their row/column affine transforms
are applied separately. Two observations claiming the same subject/stream must
also have identical pose keys. Different streams retain their own pose identities;
the caller supplies the shared clock relationship.

## Result meanings

`measured` returns global minimum surface distance, exact rational squared distance,
closest points and global triangle ordinals, `surface_intersection` and
`within_tolerance`. Shared vertices/edges, coplanar overlap and crossings all count
as surface intersection. Tolerance affects proximity only; a small positive gap
remains nonintersecting. Only one closest pair is returned, with deterministic
selection among tied witnesses.

`insufficient` preserves structured reasons and both input identities, requirements
and achieved coverage, but contains no numeric verdict. This includes unsupported
required effects, stale region/pose/clock identity, invalid selected geometry and
exhausted work. An explicitly accepted bone reference records its omitted effects;
it cannot satisfy a requirement for cloth or another missing rendered surface.
Malformed API arguments raise `ValueError`.

Every result records its settings, operation counts and elapsed service time. Its
input hashes bind original observation metadata and position bytes; topology and
region hashes separately bind indices and selection. Results retain immutable
metadata and coverage, not source position/index buffers. `to_mapping()` returns
detached JSON-compatible data with format `mesh_pair_measurement`, schema 1 and
method `exact-triangle-surfaces-v1`. Exact squared-distance numerator/denominator
are decimal strings; presentation distance/points are rounded finite numbers.

Containment is always `not_evaluated`. A mesh wholly inside another can have
positive surface distance with no surface crossing. Open, nonmanifold or
self-intersecting inputs are treated as unions of triangles, not certified solids.
Any degenerate selected triangle makes the result insufficient; inputs are not
welded, repaired or silently filtered. Unselected geometry is outside that region.
Penetration depth, volume, physical contact and artistic acceptance are not outputs.

## Numerical and resource limits

The reference implementation uses exact rational arithmetic on stored input numbers
and affine transforms, with an exact AABB search. It avoids rounding-based intersection
decisions; it does not improve capture precision or prove agreement with effects
excluded from acquisition. Floating-point unsquared distance is scaled before
conversion to avoid premature squared-distance underflow.

Limits apply to each region's triangle count, tested triangle pairs, visited node
pairs and coordinate numerator/denominator bits before and after transformation.
The supported coordinate bit cap is 8..256. A result with an unrepresentable positive
presentation distance is insufficient. Search stops only at a certified global
minimum or returns insufficient when its work budget is exhausted.

Preparation and tree storage grow with selected triangles; source hashing scans the
input position buffer. The entire selected region is validated before search,
including when the first pair intersects. Count limits do not bound wall time,
process RSS, source-record storage or caller-created copies. This is an offline
correctness path; use measured workload evidence when choosing region sizes.

## Sampled intervals

```python
from animation_analysis import summarize_mesh_interval
from animation_analysis.temporal import TimeInterval

summary = summarize_mesh_interval(results, TimeInterval(start_stamp, end_stamp),
    max_gap_seconds=0.05, max_samples=1_000)
interval_document = summary.to_mapping()
```

Supply a bounded list/tuple in acquisition order with exact endpoint samples.
`complete` means the inputs satisfy the declared sample coverage, identity and gap
requirements. It does not describe what happened between samples. Gaps compare
exact differences of the stored clock numbers; overflowed presentation seconds
remain null while exact gap values are preserved.

Missing samples or failed measurements produce `insufficient`, retaining individual
results and partial observed minima/counts when comparable. Changed pair criteria,
units, topology/regions, duplicate/reversed/unrelated times or out-of-window samples
suppress aggregate measurements. Positions, transforms, pose frame/revision and
per-frame evidence IDs may change within a compatible sequence. No sorting,
interpolation, contact duration or swept-intersection inference occurs.
