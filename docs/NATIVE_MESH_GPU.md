# Cached GPU bone sampling and shared admission

`AnimationCaptureMeshGPU.h` adds an explicit asynchronous producer for UE 5.6
D3D11 Skin Cache positions. The producer ID is `unreal-skin-cache-bone-v1`.
Completed snapshots use the existing schema-1 mesh replay contract and Python
0.3.0 reader. This is a bone surface before material effects. Morph, cloth,
deformer, material displacement and raster visibility remain excluded capabilities.

## Enrollment and requests

Supply a live world, its viewport, one registered ordinary skeletal component,
opaque enrollment identities, mesh limits, GPU limits and a shared budget. Create
after registration and before the next ordinary single-node pose finalization.
The project must compile Skin Cache shaders and the component must actually use
the cache. The producer changes no component, pose, LOD, CVar or renderer policy.
There is no automatic CPU fallback. CPU bone and rigid references remain separate
options for callers who explicitly choose them.

```cpp
auto Shared = MakeShared<FAnimationCaptureBudget, ESPMode::ThreadSafe>(
    FAnimationCaptureBudgetLimits{32, 128ll * 1024 * 1024});
FAnimationMeshLimits MeshLimits{4, 20000, 100000, 16, 256, 16ll * 1024 * 1024};
FString Error;
auto GPU = FAnimationCaptureMeshGPU::Create(World, Viewport, Component,
    Enrollment, MeshLimits, FAnimationMeshGPULimits{}, Shared, Error);
FAnimationMeshGPUTicket Ticket;
if (GPU) { GPU->Request(TEXT("sample-1"), Ticket, Error); }
// Subsequent game-thread updates:
if (GPU)
{
    GPU->Pump();
    FAnimationMeshGPUResult Result;
    while (GPU->Collect(Result)) { /* Handle every status; retain Snapshot if needed. */ }
}
```

An admission rejection returns `false` without creating a terminal result.
An admitted envelope can subsequently become `Unavailable` when acquisition
cannot reserve geometry/source storage or prove compatibility. Each admitted
request emits one terminal result: completed, unavailable, failed, cancelled or
timed out. A result without acquisition keeps `bHasAcquisition=false`; completion
and collection stamps never replace acquisition identity. Pose revisions belong
to their observer enrollment; do not compare unrelated observers' counters.

## Renderer and lifetime contract

The supported view is one full-resolution perspective view without AA or scaling.
The game-thread view callback validates resident topology, weights and the current
pose without computing CPU-skinned vertices. An exact view-family token carries
that acquisition into the renderer. The renderer checks reference-to-local bone
matrices, transform, selected render LOD, exhaustive sections and the typed float3
position layout before copying. One shared position buffer is supported; other
layouts remain unavailable. UE 5.6's public Skin Cache setup wait occurs before
the cached-geometry query and is timed separately from GPU completion latency.

Pending weights, CPU rendering, active/external morphs, cloth mappings and deformer
instances reject this backend. The CPU reference's pose/asset/attachment/streaming
guards also apply. Resident CPU topology and effective weights remain required.
No claim covers arbitrary animation graphs, leader pose, physics blending, Nanite,
cloth, deformers, WPO/PDO, masked visibility or other RHIs.

Pump queues at most one nonblocking render poll. Requests and polling never wait
for GPU completion. Source RHI references and staging stay owned through copy
retirement, even after cancellation collection. Resize, component retirement and
world cleanup cancel pending work. Destroy the producer on the game thread;
immutable snapshots may outlive it and its world. Shutdown is the sole explicit
GPU drain and reports its elapsed time. Device hangs follow Unreal's device policy.

## Combined capacity and compatibility

Pass the same `FAnimationCaptureBudget` to mesh budgets, image readback/viewport
adapters, PNG writers and optional session `SharedBudget` settings. Existing
constructors/settings keep their previous defaults when no shared budget is used.
Public native types changed: rebuild adapters and dependent modules together.
No consumer pin or asset changes are made by this delivery.

The common budget counts allocation reservations, not inferred logical requests.
A GPU request reserves a 64 KiB envelope, mesh payload/work capacity, then the
whole retained engine position allocation plus copy staging. Source/copy limits
are owner totals, including cancelled copies. Immutable snapshots and collected
image results retain their leases. PNG admission occurs before taking pixels or
scheduling work. Pipeline stages can conservatively overlap charges, so budget
headroom is necessary. Caller-created copies, allocator/driver overhead, shader
compilation and other engine allocations are outside these capacity figures;
they are not process/GPU RSS measurements. Caller-created producer enrollment
objects are also outside request capacity. Optional shared session settings reserve
synchronous screenshot pixels before readback as well as asynchronous/PNG capacity.

The mesh metadata/topology/configuration is validated and hashed on every request.
Cached positions avoid CPU vertex skinning but do not eliminate this game-thread
work. Performance results must include it. See the delivery report for measured
workloads and the remaining qualification boundary.
