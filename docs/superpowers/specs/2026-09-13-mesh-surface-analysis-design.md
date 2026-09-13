# Mesh surface analysis design

## Approved intent and scope

The user approved explicit regions, surface distance and sampled intersection as
the next slice, with conditional deformation deferral. Implement portable offline
measurements and interval evidence using existing mesh records. Keep all changes
in AnimationAnalysis. Katana owns adapters, assets, pins and live qualification.

Python >=3.11; standard library only; mesh replay schema 1 remains readable.
New additive measurement APIs ship as Python 0.4.0. Native acquisition is unchanged.
No push, consumer edit, image generation or archive cleanup is part of this slice.

## Measurements

`MeshRegion(region_id, topology_id, triangle_ids, limits)` owns an explicit sorted,
unique nonempty tuple of global triangle ordinals (index-element offset / 3).
Its content identity includes its name, topology identity and selected ordinals.
No anatomy, section, proximity or skin-weight heuristic creates a region.

`MeshSelection(observation, region, requirement)` uses existing `MeshObservation`
and `MeshRequirement`. `MeshPairSample(sample_id, acquired, first, second)` declares
the shared acquisition clock/time. Each observation must match that stamp; unrelated
subjects/streams keep their own pose identities. Units and coordinate-system names
must match; each affine row/column transform is applied independently.

`measure_mesh_pair(sample, *, tolerance, limits)` first assesses required coverage
and region/topology compatibility. It returns a `MeshPairResult` with `measured` or
`insufficient` status, structured reasons, both original observation/requirement
identities and input content hashes, settings, and work statistics. Malformed API
arguments raise `ValueError`; unavailable coverage, stale identities, invalid
selected geometry or exhausted work produce insufficient results with no numeric
verdict. Results retain metadata, not source mesh buffers.

A measured result contains global minimum triangle-surface distance, exact rational
squared distance, a closest point and global triangle ordinal on each input,
`surface_intersection` and `within_tolerance`. Intersection includes a shared point,
edge, coplanar overlap or crossing and is exactly distance squared == 0. Proximity
compares against the supplied tolerance squared and never changes intersection.
Points and unsquared distance are rounded for presentation; predicates apply to
the exact stored input numbers and affine transformation. This does not improve
the accuracy of the acquisition itself.

Containment is explicitly `not_evaluated`: nested closed meshes can have positive
surface distance without being disjoint volumes. Open/nonmanifold/self-intersecting
triangle sets are usable as surface sets; no topology certification or repair is
implied. Reject any degenerate selected triangle, including transform collapse.
An invalid unselected triangle does not invalidate a separately explicit region.

## Numerical method and bounds

Use `fractions.Fraction` for transformed selected coordinates and triangle geometry.
Closest distance checks projected vertex/face candidates, edge/edge candidates and
edge/face piercing. This handles cases missed by vertex-only sampling. An exact
AABB hierarchy prunes pairs whose lower bound cannot improve the current minimum;
stop at zero, since it is a certified global lower bound. Deterministic tree/order
rules provide repeatable witnesses; tied minima need not enumerate all witnesses.

`MeshAnalysisLimits(max_triangles, max_pair_tests, max_node_visits,
max_coordinate_bits=256)` bounds each region before allocation, exact triangle
tests and visited node pairs. Coordinate bit length is measured on numerator and
denominator before and after transform; supported cap range is 8..256. BVH nodes
and selected vertices are linear in admitted triangles. Work limits are operation/
count bounds, not hard wall-time or interpreter RSS guarantees. Caller-owned record
buffers are outside working-set accounting. A budget stop never publishes a partial
minimum as complete. Nonzero distance unrepresentable as a finite positive float
is insufficient even though the exact predicate remains mathematically defined.

CGAL's [AABB guide](https://doc.cgal.org/latest/AABB_tree/index.html) supplies an
established broad-phase reference. [Shewchuk's robustness discussion](https://www.cs.cmu.edu/~quake/robust.html)
motivates avoiding floating-point sign decisions near degeneracy. We implement
an independent standard-library reference, without adding or copying either library.
Performance must be measured; this is an offline correctness path, not a real-time
or arbitrary-mesh scalability claim.

## Interval reporting

`summarize_mesh_interval(results, interval, *, max_gap_seconds, max_samples)`
accepts a bounded explicit list/tuple of `MeshPairResult` and existing `TimeInterval`.
Preserve input order, failed samples and actual acquisition gaps. Reject duplicate
sample IDs/times, reversed or unrelated clocks and changing pair configuration/
region/requirement/tolerance as insufficient. Pose frame/revision and positions may
change, but the selected subjects/streams and other pair criteria must be stable.
Require samples at the interval endpoints and gaps <= supplied maximum. Retain
successful observed minima/counts as partial evidence when interval coverage fails,
clearly labelled; never infer continuous contact or intersection duration.
The summary reports sampled events only and serializes to JSON-compatible mappings.

## Qualification

Independent literal fixtures cover separated/coplanar/crossing/tangent/edge-edge
cases, nested cubes, reversed winding, rigid/nonuniform and row/column transforms,
small nonzero gaps, selected degeneracy, topology/clock/coverage failure, region
exclusion and work exhaustion. A synthetic omitted deformation that changes the
answer must remain insufficient if the requirement includes it. Different pose
samples exercise interval gaps and configuration changes.

Run the full core suite, distribution isolation with/without image support,
repository tooling, and explicit schema-1 replay round trips. Analyze retained native
GPU geometry through the installed wheel as an offline compatibility control;
it is not a fresh live consumer capture. Record fixed-workload performance, bounded
failure evidence and source identity. Native source identity must remain unchanged;
no native rebuild is needed for unchanged native code. Consumer integration stays open.
