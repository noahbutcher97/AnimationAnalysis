# Surface capabilities and fidelity

Recorded 2026-09-11. This is the proposed production direction following the
[skeletal feasibility experiment](research/2026-09-11-skeletal-surface-sampling.md)
and [Katana requirements investigation](research/2026-09-11-katana-surface-requirements.md).
The subsequent [animated readiness checks](research/2026-09-11-animated-surface-readiness.md)
add loaded-asset findings, real animation/rigid-prop evidence and qualified task ordering.
It defines requirements and qualification criteria. The [portable mesh record/replay
slice](MESH_RECORD_DELIVERY.md) implements the first contract layer; the
[native reference delivery](NATIVE_MESH_REFERENCE_DELIVERY.md) qualifies explicit
single-node CPU bone and rigid sampling. The [GPU delivery](NATIVE_MESH_GPU_DELIVERY.md)
now qualifies bounded cached bone sampling. Broader deformation backends remain
pending. Existing capture defaults and formats remain unchanged.

## Product contract

Offer explicit choices for the evidence a user needs, with the most complete
practical deformation coverage as the destination. Katana informs useful first
workflows; neutral fixtures and additional use cases establish broader capability.

Priority clarified 2026-09-13: finish core mesh analysis before extending deformation
fidelity. Morphs, cloth and material deformation are stretch goals for optional
higher fidelity and broader use after core functionality. This supersedes the
earlier early-morph-experiment recommendation. The immediate analysis work is
regions, surface distance, sampled intersection, separate containment rules and
interval reporting using explicitly supported observations.

Keep three outputs distinct: **deformed geometry**, **rendered visibility/appearance**,
and **analysis against supplied criteria**. A mesh can include hidden triangles;
an image records a view and rendering policy. Neither alone establishes physical
contact or artistic quality. CPU/GPU selection is a backend choice, not a quality
level. A geometric reference is valuable without claiming final-surface equivalence.

"Final rendered surface" needs a declared component, time, view, pass and supported
effects. UE applies material world-position offset after vertex-factory positions;
pixel-depth offset separately changes fragment depth. Thus a cached position buffer
is not a universal representation of everything visible. This follows from installed
UE 5.6 `Shaders/Private/BasePassVertexShader.usf` and Epic's
[material inputs documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/material-inputs-in-unreal-engine?application_version=5.6).
View-dependent effects can preclude a single view-independent final mesh.

## Capability matrix

The **current evidence** column is deliberately narrower than the target product.
"Required" below means a design/qualification requirement, not implemented support.

| Capability / use case | Observation required | Current evidence and next qualification |
|---|---|---|
| Motion, pose and explicit proxies | Caller-nominated points, transforms and clocks | Existing foundation; retain as a useful inexpensive option without surface claims. |
| Rigid props and attachments | Resident triangles, section identity and acquisition transform | Native ordinary static meshes and direct qualified skeletal attachments now have explicit enrollment, immutable triangles, transform/pose checks and replacement rejection. Broader component/parent types remain unqualified. |
| Bone-deformed meshes | Effective weights, reference mapping, finalized pose, LOD and triangles | Native CPU and cached-GPU single-node references pass two LODs, multiple sections and retained-budget controls. The GPU delivery adds matching raster, combined admission and teardown evidence. General pose/deformation coverage remains open. |
| Morphs and facial correctives | Effective deformed positions at the required stage; active state/provenance | Stretch goal after core functionality, beginning with an independent positive control. A bone reference cannot serve as the expected final morph result. |
| Cloth and simulated clothing | Mapped render vertices after simulation/blending, simulation identity and section coverage | Stretch goal after core functionality; qualify mixed cloth/non-cloth sections and frozen/skipped updates. Bone geometry cannot substitute for cloth coverage. |
| Control Rig, IK, post-process and physics blending | The pose actually consumed by skinning, plus any later deformation | Source-driven previews do not establish live parity. Qualify ordering with a real skeletal component; separately identify simulation-driven surface changes. |
| Mesh Deformer / ML deformation | Actual graph output, mapping and output-generation identity | Cached deformer access was source-inspected only. Requires independent expected displacements and execution/lifetime controls. |
| Modular characters and LOD/weight changes | Explicit components, leader mapping, effective weights, topology/configuration generations | Required admission and invalidation contract; qualify supported cases or return precise missing coverage. Never mix old topology with new positions. |
| Material vertex displacement | Positions after supported material evaluation for a declared view/pass | Stretch goal after core functionality, requiring renderer-specific qualification. WPO/first-person transformations cannot be silently omitted from a final-surface request. |
| Masking, pixel-depth effects and transparency | Pass-specific raster evidence and declared visibility semantics | Separate from triangle geometry; holes or depth offsets in pixels are not automatically geometric holes/displacement. Ordinary RGB is not a complete depth/identity observation. |
| Hair, geometry caches, changing topology and virtualized geometry | Representation-specific acquisition, topology correspondence and visibility | Separate adapters/qualification; unsupported today. Do not flatten all renderables into the skeletal triangle contract. |
| Multi-view, temporal AA and other RHIs | Per-view/sample projection, temporal and backend identities | Existing depth path is narrowly UE 5.6 D3D11, single perspective, no AA. Extend by independently tested capabilities. |

UE supports distinct inline skinning, Skin Cache and Mesh Deformer paths. Skin Cache
can fall back under memory pressure and per-LOD settings; ray-traced and raster LODs
can differ. Therefore availability must be checked for the actual observation,
not inferred from a project setting. See Epic's
[UE 5.6 rendering paths](https://dev.epicgames.com/documentation/en-us/unreal-engine/skeletal-mesh-rendering-paths-in-unreal-engine?application_version=5.6).

Broader reference workflows include garment/body intersection, facial corrective
review, hand/tool grip, feet on moving supports, non-humanoid rigs and many-subject
batch regression. These are coverage targets, not claims of customer demand or
validated implementations. Neutral fixtures must avoid combat assets and anatomy
defaults while exercising the same public services.

## User choices and evidence eligibility

| Independent choice | Required behavior |
|---|---|
| Components and regions | Caller selects participants and surfaces; record omitted regions. Explicit triangle sets/region mappings are tied to topology and LOD. |
| Deformation coverage | Record required features and each feature's observed, inactive, unsupported, unknown or deliberately excluded state, with evidence and producer identity. |
| Spatial precision | Declare render LOD versus selected analysis LOD, coordinate precision and approximation. Decimation requires an explicit method/error contract; no automatic cross-LOD equivalence. |
| Temporal coverage | Select cadence/intervals; record actual acquisition gaps, staleness and held poses. Faster export cannot create missing simulation poses. |
| Channels | Geometry, motion, RGB and depth may be selected independently. Disabled imagery does not prevent geometry analysis; geometry does not imply visibility. |
| Latency and budgets | Bound outstanding requests, CPU/GPU bytes, topology caches, staging, completion queues and export. Offer explicit backpressure or reduced workload; record every loss/change. |
| Failure/fallback policy | Default to insufficient evidence when required coverage is unavailable. Allow a separately identified fallback only when explicitly accepted; never rewrite the original request. |

Presets can group these choices into motion review, sampled geometry review and
geometry-with-visual-comparison workflows. Each must show its effective settings
and unsupported features. A "highest quality" label must not conceal missing
deformation support or promise automatic artistic assessment.

Acquisition success and analysis eligibility are separate. For example, a user may
explicitly request a bone-only reference while cloth is active; the record is a
valid reference with cloth excluded. It cannot satisfy a later requirement for
the clothed rendered surface. If cloth support is required and unavailable, the
result remains insufficient even when other components succeeded.

The versioned record design must preserve subject/component generation, immutable
topology identity, LOD/section/material mapping, coordinate convention, world
transform, acquisition `PoseKey`/clock, producer/configuration identity and achieved
coverage. Completion time remains separate. Raster links require the same witnessed
observation; matching frame numbers alone are insufficient. Unknown capability
fields in future records must not be ignored when deciding eligibility.

## Recommended first production slice

Deliver **capability-aware rigid and skeletal mesh observations**, with replayable
positions/topology and an explicitly qualified renderer path. Keep CPU bone skinning
as an independent reference and opt-in geometry capability. Use cached GPU geometry
for the initial renderer-deformed path, subject to qualification; do not rename it
"final surface". Morph, cloth and material-deformation experiments and production
support follow core functionality. Deformer graphs also remain separate extension
work. Each extension needs positive controls before support is offered; the API
must already represent missing coverage without silent degradation.

The [record/replay implementation plan](superpowers/plans/2026-09-11-mesh-observation-records.md)
is complete for the portable contract. Native CPU/reference controls and conservative
effective inventory are now in the existing host. [Bounded GPU acquisition and
combined admission](NATIVE_MESH_GPU_DELIVERY.md) now have native qualification;
complete production acceptance remains open. Complete the live consumer inventory described
in the [Katana assessment](research/2026-09-11-katana-surface-requirements.md#required-consumer-qualification)
before claiming that the qualified subset satisfies its actual needs. An unsupported
required feature leaves that use case insufficient. Qualify a pose acquisition path
if it blocks a selected core workflow; defer use cases requiring stretch deformation
coverage until those extensions are delivered.

### Acceptance criteria

1. **Observation integrity:** immutable positions/topology/transform at acquisition;
   independent analytic geometry agreement within a declared 0.001 cm neutral-fixture
   tolerance; explicit units. Different scale/precision regimes require their own
   documented tolerances. Include nonuniform transforms, multiple sections, both
   LODs, rigid attachment and changing pose after acquisition.
2. **Production pose identity:** exercise actual skeletal animation finalization,
   including parallel evaluation, repeated finalization in one frame, skipped/stale
   poses and post-process/physics ordering where claimed. Do not force live pose,
   LOD or CPU-rendering state. Unsupported ordering must yield unknown coverage.
3. **Deformation coverage:** supported bone/rigid cases have independent expected
   positions; inactive versus unknown must differ. Detect cache-unavailable/invalid-section
   conditions and capability changes. Test unsupported-result handling and reject
   uncertain feature detection as unknown. Comprehensive cloth, graph and material-effect
   fixtures are deferred with those capabilities; do not imply their detection is
   qualified through record-validation tests alone. Positive morph qualification is
   required before offering morph coverage, rather than for the bone/rigid baseline.
4. **Topology and provenance:** asset/LOD/skin-weight changes invalidate affected
   records and region mappings; validate index ranges, finite values and section
   completeness. Leader-pose and streaming cases either pass declared controls or
   produce explicit unsupported/unavailable results. No substitute actor/component.
5. **Renderer agreement:** independently compare mesh projection with matching
   RGB/labels/depth on the declared diagnostic pass. Retain the existing 0.99
   silhouette IoU floor where applicable; declare interior depth and edge tolerances
   for each fixture before runs. Include moving/occluded subjects and negative
   pose/LOD/pass mismatches. Numerical CPU/GPU agreement alone is insufficient.
6. **Bounded lifecycle:** admit before allocation; account for combined mesh, RGB,
   depth, retained topology and export memory without duplicating existing budgets.
   Exercise saturation, cancellation, timeouts, resize, asset replacement and
   component/world destruction with work in flight. Every request terminates once;
   steady-state acquisition has no GPU-idle wait or render-thread flush. Measure
   any required teardown drain and retained resources separately.
7. **Replay and compatibility:** standalone Python consumes explicit records without
   Unreal/game imports; reject corrupted topology, mismatched poses, missing required
   features and truncated buffers. Old motion/RGB/depth paths preserve defaults and
   meanings. Version new mesh records/readers explicitly. Pass distribution isolation
   and native host verification; consumer owner builds and tests affected integration.

These are production acceptance requirements, not a passed delivery checklist. The
animated probe passes a narrower subset and supplies reusable controls. Its asset
inventory supports deferring production morph support; runtime assignments and
masked/PDO materials still preclude a complete Katana surface-coverage claim.

### Performance qualification

Compare capture disabled, CPU geometry, asynchronous GPU geometry and combined
geometry/RGB/depth under identical warmed scenes, rendering and output policies.
Use at least three comparable repetitions. The primary neutral workload should use
two 10k-vertex subjects plus an attached rigid prop at 640x480; additionally measure
960x540, 30/60 Hz requests, higher vertex counts and more subjects. Freeze exact
workload counts, duration, budgets and acceptance limits before collecting results.

Report frame-time p50/p95/p99, game/render-thread acquisition work, completion latency,
actual cadence, occupancy, peak owned bytes and losses. Separate copies, GPU execution
where instrumented, encoding and writing. Ready polling latency is not GPU kernel
time. A selected workload must meet its declared budgets without silent degradation;
no universal performance target or GPU speedup is established by this design.

The earlier probe measured CPU skinning on approximately 10k/2.6k vertices and GPU
readiness for one outstanding copy. It did not benchmark this combined production
pipeline. Katana's reported intermittent PNG saturation is a reason to test the
combined budget, not evidence that a larger default allowance fixes the issue.

## Next core analysis and delivery boundaries

Use qualified acquisition to add explicit regions and offline triangle
distance/intersection with separate containment rules, then interval reporting
that preserves sampling gaps. Validate the shared services with neutral controls
and consumer-owned integration. Intersection and containment
are different predicates: a wholly enclosed mesh need not cross another surface.
CGAL's established [polygon mesh processing reference](https://doc.cgal.org/latest/Polygon_mesh_processing/index.html)
distinguishes intersection from bounded-side tests and states the closed-mesh and
self-intersection assumptions. It is a research reference, not a selected dependency.

Define behavior for open garments, nonmanifold meshes, self-intersections, degenerate
triangles and numerical tolerance. Do not silently repair/weld input geometry.
Penetration depth, intersection volume, support/sliding and swept intersection each
need separately defined quantities and controls. Sampled positions alone do not
prove absence of between-sample contact. Broad-phase proximity is only a candidate
filter, and improved geometry does not confer artistic authority.

Shared changes stay in AnimationAnalysis. Katana owns roles, assets, criteria,
gameplay interpretation, pin updates and integration runs. No consumer code or assets
were edited. The original requirements pass used documentation/source checks and
generated no images; its source snapshot, collection/verification scripts and
hash manifest are retained in `Saved/SurfaceCapabilityReview-20260911/`; its verified
archive is `Saved/SurfaceCapabilityReview-20260911-evidence.zip`.
The later [readiness report](research/2026-09-11-animated-surface-readiness.md) separately
records fresh prototype runtime/raster results, image retention and remaining limits.
