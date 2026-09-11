# Native mesh references

`AnimationCapture/AnimationCaptureMeshReference.h` adds synchronous, explicit CPU
bone and rigid references. These are selected analysis-LOD triangles, **not final
rendered surfaces**. No existing session, image, RGB or depth defaults change.

## Enrollment and acquisition

Create a sampler on the game thread after registration and before the next skeletal
finalization. Supply the exact component, opaque component/asset/subject/stream
identities, generations, configuration identity, analysis LOD and one material ID
per material slot. An empty material ID means unknown. No discovery, skeleton
defaults, asset paths or gameplay classes enter the portable record automatically.

```cpp
#include "AnimationCapture/AnimationCaptureMeshReference.h"

FAnimationMeshLimits Limits{4, 100000, 600000, 32, 512, 64ll * 1024 * 1024};
auto Budget = MakeShared<FAnimationMeshBudget, ESPMode::ThreadSafe>(Limits);
// Enrollment and Component are supplied by the adapter.
FString Error;
auto Sampler = FAnimationCaptureMeshReference::Create(Component, Enrollment, Budget, Error);
// After an ordinary world tick has completed and the enrolled pose finalized:
auto Snapshot = Sampler ? Sampler->Capture(TEXT("request-1"), Error) : nullptr;
if (Snapshot)
{
    AnimationCaptureMeshReplay::Write(ExistingTrustedRoot, TEXT("request-1"), *Snapshot, Error);
}
```

The sampler does not tick/refresh animation, force LOD, switch skinning paths, enable
CPU access, wait for compilation or change component settings. Destroy the sampler
on the game thread. Failed enrollment/acquisition returns no geometry and a specific
`Error`; callers retain their request ID and handle that unavailable result. This
synchronous API does not create terminal failure bundles or an asynchronous queue.

## Qualified capability and rejection

- Ordinary `USkeletalMeshComponent`, single-node animation, fixed effective weights
  with at most 12 influences, complete resident triangles and section/bone mappings.
  The current finalization must match engine frame and world time. Every weighted
  bone must belong to the evaluated `RequiredBones`; finer-than-predicted analysis
  LODs are conservatively rejected. Analysis LOD is not a render-LOD witness.
- Ordinary `UStaticMeshComponent`, resident non-Nanite triangles, independently
  transformed or directly attached to a qualified skeletal component. A socket's
  bone must have evaluated-pose coverage and its attachment transform must match.
- Specialized component subclasses, other parent chains, leader poses, animation
  graphs, post-process animation, physics blending, reference-pose overrides,
  unqualified absolute attachment transforms, compiling/streaming assets, missing
  data and stale poses return unavailable. Broader support requires qualification.

Asset, parent/socket and effective material replacement require re-enrollment.
Use a new component/configuration generation for changed enrollment. Effective
weight bytes/layout, inverse binds, source positions, bone visibility and topology
contribute to the acquired configuration fingerprint. Complete section/material
mapping and indices contribute to the Python-compatible topology SHA-256.

Coverage explicitly observes `bone` or `rigid` and `pose_ordering`. It deliberately
excludes `morph`, `cloth`, `mesh_deformer`, `material_displacement` and
`raster_visibility`. Reasons retain effective morph-weight counts, cloth section
mapping, deformer-instance presence and effective material relevance counts across
included sections. These inventories do not prove active/inactive rendered effects.
Only a caller requirement explicitly allowing these exclusions can accept this
reference; a full-deformation or visibility requirement remains insufficient.

## Ownership, budgets and replay

Snapshots own positions, global indices, metadata and the acquisition transform;
they contain no UObject references. A retained shared pointer keeps its reservation
until its last owner releases it, including after sampler/component retirement.
Callers can read retained immutable data on other threads. Copying `Data()` creates
caller-owned allocations outside producer accounting.

Share `FAnimationMeshBudget` between CPU samplers. All limits are explicit and zero
is invalid; sections are capped at 64 and bones at 65,536. Admission reserves
`3*(24*vertices + 4*indices) + 12*vertices + 128*bones + 1 MiB` before variable
producer allocations. This conservatively covers payload, skin/matrix work and
bounded metadata/serialization capacity. It is not process RSS: engine storage,
allocator overhead, sampler enrollment and caller copies are outside it. There is
no topology cache. This budget is **not yet shared with image/GPU capture**.

The Windows writer runs synchronously on the game thread, preventing overlapping
exports against one reservation. It writes [mesh replay schema 1](MESH_OBSERVATIONS.md)
with centimetres, Unreal left-handed Z-up coordinates and a row-vector matrix.
Both clocks use `unreal-monotonic`; publication does not replace acquisition or
CPU completion time. Python 0.3.0 reads the same schema without changes.

Supply an existing stable trusted root and one portable ASCII child name. The writer
rejects reparse roots, unsafe names and existing bundles, flushes exclusive files,
then publishes `complete.json` with an exclusive hard link. Interrupted output stays
incomplete. Combined encoded JSON is capped at 64 KiB; an otherwise valid snapshot
can exceed the export envelope. Other platforms/filesystems remain unqualified.

## Neutral host inspection

Prepare/launch the existing host as described in [HOST_WORKFLOW.md](HOST_WORKFLOW.md).
In PIE, run `AnimationAnalysis.Host.InspectMesh`, then
`AnimationAnalysis.Host.SampleMesh` after the animation has ticked. The sample is
written beneath `Saved/Observations/sample-<id>`. Use
`AnimationAnalysis.Host.StopMesh` to restore/remove the fixture. The fixture owns
its animation, camera, diagnostic settings and forced LOD; the sampler does not.

`AnimationAnalysis.Capture.Mesh.Reference` checks the same producer and command
routes. `AnimationAnalysis.Capture.MeshReplay.PythonCanonicalTopology` checks the
wire identity against an independent Python value. For a retained `MeshReference-*`
directory, run the repository tool `Python/verify_mesh_reference.py` using an
installed package and explicit limits. Its geometry error fields are retained
native analytic-test evidence; it independently validates bundle integrity/schema.

The next slice is bounded cached-GPU acquisition and combined image/mesh admission,
then renderer comparison and cancellation/teardown qualification. Morph, cloth,
deformer, material displacement and broader pose/RHI coverage remain later work.
