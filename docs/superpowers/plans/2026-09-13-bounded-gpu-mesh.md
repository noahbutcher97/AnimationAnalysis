# Bounded GPU mesh delivery

Spec: `SURFACE_CAPABILITIES.md`, `NATIVE_MESH_REFERENCE.md` and the documented
UE 5.6.1 Skin Cache ordering investigation. Continue from `5b716a0`; no consumer
edits, pin changes or remote publication. Preserve explicit reference exclusions.

## Task 1: Shared admission across producers

Add `AnimationCaptureBudget.h/.cpp` with thread-safe RAII reservations, explicit
maximum bytes/reservations, live/peak/admitted/rejected stats and no discovery.
Wire an optional shared budget through image readback, its viewport wrapper, PNG
export and session settings without changing existing defaults. Collected image
arrays retain their lease; submitted GPU resources keep the same lease until
retirement even when cancellation was collected first. PNG reserves before moving
pixels or scheduling encoding. Separate stages may conservatively overlap charges.
Wire CPU mesh budgets to this same shared admission object. Count reservations,
not inferred logical requests; engine/allocator overhead remains outside capacity.

## Task 2: Cached-GPU bone producer

Add `AnimationCaptureMeshGPU.h/.cpp`. Explicit component/world/viewport enrollment,
request identity, per-owner pending/source/copy/deadline limits and required shared
budget. Reuse reference topology/pose/configuration preparation without CPU skinning.
Acquire metadata in the game-thread view-family callback; consume the mesh object
only in that exact queued renderer view. Validate LOD, transform, complete section
and vertex mapping before any copy. UE 5.6 pre-query Skin Cache setup wait is explicit
and measured. Reject missing cache, active morph/cloth/deformer, unsupported views
and incompatible geometry; never substitute a CPU surface.

Retain the actual RHI source and staging allocations with shared admission before
copy. Poll readiness asynchronously, at most one queued poll per producer. Preserve
acquisition identity in completed immutable schema-1 snapshots. Exactly one terminal
result per admitted request, including cancellation, timeout, resize and destruction.
Keep resources charged until fences retire; only shutdown may use a measured GPU
drain. No UObject or mesh-object access after renderer acquisition.

## Task 3: Neutral controls and measurements

Extend the existing procedural mesh fixture/host. Test two LODs, multiple sections,
analytic CPU/GPU agreement, delayed consumption after live changes and retirement,
unavailable cache/LOD/feature paths, cancellation before/after submission, timeout,
resize/world cleanup and independent owners. Exercise real combined mesh/image/PNG
admission and retained-result backpressure. Preserve matching raster controls with
0.99 silhouette IoU and 0.05 cm interior-depth tolerance; wrong pose must fail.
Freeze benchmark counts, cadence, view and output policy before collecting disabled,
CPU, GPU and combined measurements with at least three comparable repetitions.
Report frame p50/p95/p99, acquisition work, latency, actual cadence, bytes and losses.

## Task 4: Verification and delivery

Run exact new controls, existing rendered suite and distribution/tooling isolation.
Replay archived native output through installed Python. Review lifetime, identity and
accounting, fix findings and commit verified slices. Retain source hashes, raw results
and archives before cleaning inventoried images. Document supported scope, measured
limits and pending consumer integration; fast-forward the local research branch.

## Interface and execution ledger

- Task 1 supplies `FAnimationCaptureBudget::Reserve(int64)` returning
  `TSharedPtr<FAnimationCaptureReservation, ESPMode::ThreadSafe>` and `GetStats()`.
  Limits fields: `MaxReservations`, `MaxBytes`; stats fields: `LiveReservations`,
  `PeakReservations`, `LiveBytes`, `PeakBytes`, `Admitted`, `Rejected`.
- Tasks 1/2 share that admission API; task 1 owns existing image/session files,
  task 2 owns mesh files. Task 3 consumes both APIs. No conflicting file ownership.
- Ruling: require one shared Skin Cache position buffer with exhaustive sections
  in this first backend; reject other layouts rather than reinterpret offsets.
- Ruling: restrict this backend to the previously qualified single-node bone subset.
  Full deformation and broader renderer support remain explicit later capabilities.
- Shared admission, GPU acquisition, neutral controls and archive replay are verified.
- Implementation committed as `2201b32`; final delivery evidence is in
  `../../NATIVE_MESH_GPU_DELIVERY.md`. Consumer integration remains owner-controlled.

### Frozen comparison workload

Before measurement: `Mesh.GPUCombined` uses two 10,302-vertex/two-section animated
components frozen at animation time 0.25 seconds, plus the directly attached rigid
prop. View 640x480, diagnostic unscaled perspective/no AA, requested cap 60 Hz.
Rotate disabled/CPU/GPU/combined order over three repetitions. Each mode gets 30
warmup and 90 measured automation updates. CPU samples both meshes and prop; GPU
samples both caches and CPU rigid prop; combined additionally requests RGB/depth
every update and encodes every tenth request when completed. Preserve all PNGs.
Drain/teardown is separate from measured frame intervals. Record every rejection
and terminal outcome; no throughput claim from a shortened successful subset.
These first measurements have other editors open. 30 Hz, 960x540 and larger
meshes/subject counts remain separate qualification workloads, not inferred results.
