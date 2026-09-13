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

## Opt into finalized animation

Enrollment defaults to `EAnimationMeshPosePolicy::SingleNode`. For an ordinary
compiled AnimBlueprint/slot/montage path, set the policy before creating the sampler:

```cpp
Enrollment.PosePolicy = EAnimationMeshPosePolicy::FinalizedAnimation;
```

Register before the next finalization and sample after world tick completes. The
observer requires a current finalization, stable animation instance/mode, no pending
animation update or parallel/post evaluation, matching update/bone witnesses and
evaluated analysis-LOD bones. It never finishes animation work for the caller.
Stale or replaced state requires a new valid witness; instance/asset changes require
reenrollment with new generations. Observer revisions are local to each enrollment.

Leader poses, linked animation instances, post-process animation, physics blending,
reference overrides and custom animation modes remain unavailable. A rigid attachment
uses its direct skeletal parent's policy and finalization, plus evaluated socket
and synchronized attachment-transform checks. All material/deformation exclusions
remain unchanged. Rebuild native adapters to use the new enrollment field.

## Paired rigid acquisition

Use `CaptureRigidBatch` when multiple independent rigid parts must describe one
game-thread state. Separate `Capture` calls retain separate acquisition times.

```cpp
TArray<FAnimationCaptureMeshReference*> Samplers{Fixed.Get(), Moving.Get()};
TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Pair;
if (FAnimationCaptureMeshReference::CaptureRigidBatch(
        Samplers, TEXT("sample-01"), 2, Pair, Error))
{
    // Pair follows Samplers order. Each snapshot keeps its actual completion time.
}
```

The explicit maximum component count is 1..64 and must admit the supplied nonempty
list. All participants must be different registered ordinary unparented static
components in one world, outside world tick and without physics simulation. The
operation observes existing state synchronously; it does not tick, drive poses,
yield to caller code or combine unrelated acquisitions after the fact. Use the
separate mixed-component API below for skeletal components and direct attachments.

Every output receives the shared native acquisition stamp and frame at preparation,
with its original component identity, observer revision and actual completion time.
Failure leaves no partial output and releases this call's partial reservations.
Retained outputs still consume their existing per-sampler/shared budgets. Rebuild
native callers to use the new opt-in API; existing calls and replay schema stay
compatible. The [neutral assembly example](NEUTRAL_ASSEMBLY.md) exercises this path
through replay, region measurements and interval reporting.

## Paired skeletal and attached references

`CaptureBatch` has the same ordered, bounded interface and supports enrolled
skeletal references plus direct rigid attachments under their selected pose policies:

```cpp
TArray<FAnimationCaptureMeshReference*> Samplers{Body.Get(), AttachedPart.Get()};
TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>> Pair;
const bool Captured = FAnimationCaptureMeshReference::CaptureBatch(
    Samplers, TEXT("pair-01"), 2, Pair, Error);
```

All components must be distinct and share one stable world outside tick. The batch
establishes acquisition before preparing snapshots, validates their witnesses before
publication and preserves different observer revisions and actual completion times.
Failure returns no partial output and releases this call's partial reservations.
No export or consumer callback runs between preparations. The attachment's parent
need not also be an output participant, but its finalization and transform must qualify.

This synchronous CPU/rigid batch does not group independent GPU requests. Existing
individual captures keep independent clocks. Never retime them, or pair them solely
because engine frames match. See [qualification and consumer limits](FINALIZED_POSE_ACQUISITION_DELIVERY.md).

## Qualified capability and rejection

- Ordinary `USkeletalMeshComponent`, single-node animation by default or the explicit
  finalized-animation policy described above, fixed effective weights
  with at most 12 influences, complete resident triangles and section/bone mappings.
  The current finalization must match engine frame and world time. Every weighted
  bone must belong to the evaluated `RequiredBones`; finer-than-predicted analysis
  LODs are conservatively rejected. Analysis LOD is not a render-LOD witness.
- Ordinary `UStaticMeshComponent`, resident non-Nanite triangles, independently
  transformed or directly attached to a qualified skeletal component. A socket's
  bone must have evaluated-pose coverage and its attachment transform must match.
- Specialized component subclasses, other rigid parent chains, leader poses,
  unqualified animation modes, post-process animation, physics blending, reference-pose overrides,
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
no topology cache. An optional second constructor argument accepts the
[common image/GPU capture budget](NATIVE_MESH_GPU.md#combined-capacity-and-compatibility),
so retained CPU references can consume the same aggregate admission.

The Windows writer runs synchronously on the game thread, preventing overlapping
exports against one reservation. It writes [mesh replay schema 1](MESH_OBSERVATIONS.md)
with centimetres, Unreal left-handed Z-up coordinates and a row-vector matrix.
Both clocks use `unreal-monotonic`; publication does not replace acquisition or
CPU completion time. Python 0.3.0 reads the same schema without changes.
Fresh exports use double round-trip precision for JSON clocks and transforms.
Earlier exports used six decimals; they remain readable but cannot recover lost
precision. New record byte hashes therefore differ without a schema change.

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

[Bounded cached-GPU acquisition and combined image/mesh admission](NATIVE_MESH_GPU.md)
now have renderer comparison and cancellation/teardown qualification. Morph, cloth,
deformer, material displacement and broader pose/RHI coverage remain later work.
