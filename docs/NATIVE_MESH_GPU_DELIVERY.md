# Bounded GPU bone delivery

Verified 2026-09-13. Implementation commit:
`2201b32104a7083f7de5d44cb84121412ae2b9cb`.
Changes stay in AnimationAnalysis; no Katana assets, adapters, dependency pins or
engine files changed. This delivery is committed locally, without remote publication.

## Delivered behavior

The [GPU API](NATIVE_MESH_GPU.md) reads actual renderer Skin Cache positions for
an explicitly enrolled, ordinary single-node skeletal component. CPU vertex
skinning is not its acquisition source and there is no automatic fallback.
The existing CPU producer supplies explicit reference snapshots and reusable
topology/configuration validation. GPU results identify the acquired pose, view,
LOD and configuration separately from completion and collection.

One common admission object can constrain retained mesh results, RGB/depth
readback, synchronous session pixels and PNG export. Submitted GPU resources stay
charged after cancellation collection until retirement. Retained immutable mesh
and image results continue to consume their reservations. Renderer setup work,
copy submission, decode, latency and shutdown are measured separately.

## Verification evidence

Primary run: `Saved/MeshRecordsWorktree/Saved/GpuMesh-Final-03`.
Its exact command list, UE identity, 42 staged source hashes, test log, raw outputs
and replay inventory are retained in the delivery archive.

| Check | Result |
|---|---|
| Neutral UE 5.6.1 editor build, D3D11, CL 44394996 | Passed |
| Full default native suite plus explicit `Mesh.GPUCombined` | 24/24 passed, no missing/extra/duplicate results |
| Repository tooling tests through fresh installed wheel | 26/26 passed |
| Isolated Python 0.3.0 distribution without image extra | 51 passed, one optional-image test skipped |
| Same distribution with image extra | 52/52 passed |
| Native records read from the verified replay archive using a fresh wheel installation | Seven completed bundles passed: three GPU, three CPU reference, one interactive CPU sample |
| Two GPU LODs, two sections, nonuniform transform, delayed consumption after pose change | Maximum CPU/GPU difference 0.00001538 cm; CPU/analytic difference below 0.00000920 cm |
| Matching independent mesh/raster projection | IoU 0.99995344, 20,904 interior pixels, maximum scene/label depth error 0.00213598 cm |
| Deliberately wrong pose, translated 20 cm in world Y | IoU 0.56722537; fails the matching threshold |

The raster acceptance thresholds are IoU at least 0.99 and interior depth error
at most 0.05 cm. The qualification CLI also rejects mismatched frame, LOD,
configuration and pass metadata before computing metrics. Other labelled objects
and positive-infinity clear depth are supported by the isolated oracle.

Native controls exercise unavailable Skin Cache, rendered/enrolled LOD mismatch,
retained snapshot capacity and readmission, before/after-scheduling cancellation,
tiny pre-render timeout, separate owners, resize, component destruction, actual
EndPIE/world cleanup, stale tickets, shared image/PNG leases and session cleanup.
Previously submitted copies retain their source and staging charges until retired.
The teardown controls return all shared bytes to zero. Maximum measured explicit
mesh shutdown across those final controls was 6.54 ms.

## Performance

The workload was fixed before measurement: two 10,302-vertex, two-section subjects
and an attached rigid prop; 640x480 unscaled perspective/no AA; requested 60 Hz;
three rotated mode repetitions with 30 warmup and 90 measured updates each.
Combined mode adds one RGB/depth request per update and PNG export for request
IDs divisible by ten. All raw per-frame and per-result arrays are retained.

UE selected an NVIDIA GeForce RTX 5090 Laptop GPU. Katana's UE 5.6 editor and a
separate UE 5.8 editor remained open. These are capped workstation observations;
they do not establish uncontended throughput or another machine's frame budget.

| Mode | Frame p50 / p95 / p99, ms | Achieved Hz | Peak reserved MiB |
|---|---|---:|---:|
| Disabled | 16.725 / 17.898 / 18.406 | 59.94 | 0 |
| CPU references | 16.696 / 17.745 / 18.335 | 59.97 | 2.51 |
| GPU meshes and rigid reference | 16.674 / 18.557 / 19.766 | 59.96 | 16.39 |
| GPU meshes, rigid, images and PNG | 16.611 / 18.480 / 19.511 | 60.00 | 66.56 |

CPU mode's game-thread capture work p50/p95/p99 was 5.501/6.545/6.856 ms for both
meshes plus the prop. GPU mode's request/poll loop was 0.093/0.120/0.252 ms, but
**this excludes game-thread view-callback preparation**. Each GPU mesh still cost
1.935/2.454/2.979 ms of preparation. Combined mode's corresponding per-mesh
preparation was 1.998/2.856/3.250 ms. Topology/configuration validation and hashing
remain material costs; a small request-call duration does not mean cheap acquisition.

Combined mode's render capture setup p50/p95/p99 was 0.0028/0.0083/0.0094 ms per
mesh, copy enqueue 0.0010/0.0015/0.0019 ms, and mesh decode 0.0567/0.5358/0.6694 ms.
The UE 5.6 Skin Cache setup dependency wait was at most 0.0004 ms at p99.
Mesh request-to-completion latency was 33.593/35.976/36.736 ms; image latency was
35.593/38.128/39.143 ms. These are pipeline wall latencies, not GPU kernel timings.

Each GPU mode admitted and completed 720 mesh requests including warmup, with
540 measured results. Combined mode also completed all 360 image requests and
36 PNG exports. There were no request, shared-budget or PNG admission losses,
CPU failures or unsuccessful mesh/image terminals in the comparison. Combined
peak was 69,796,432 bytes and 20 reservations against 128 MiB/64 limits. Final
pipeline drain reached 62.85 ms and is excluded from measured frame intervals;
already-drained mesh-owner shutdown stayed below 1.21 ms.

## Replay retention and cleanup

Delivery archive: `Saved/GpuMeshDelivery-20260913-evidence.zip`.
All **643 entries** were read back and SHA-256 verified before image cleanup.

SHA-256: `9859576bb976eef5e98b28129fc3c5e2e264b0e457f15c97ca7e0b88d1b9cd9b`

Receipts, replay results and performance summary are in
`Saved/GpuMeshDelivery-20260913/`. The archive preserves final and failed attempts,
raw geometry/raster/PNG evidence, source and engine hashes, the implementation
patch, tooling and the isolated wheel. Its final inner replay archive independently
verified 163 entries with SHA-256
`cf940220f87fd740e54ab21f539ef717336a13ae16fb5395dc176e419b412bc5`.

Cleanup removed 546 inventoried PNG copies: 182 output images, their 182 retained
host counterparts and 182 archive-staging copies. Every deletion matched a verified
archive entry and its current hash. Binary replay records and archived images remain.
The fresh wheel SHA-256 is
`ed477095242ec47ddb44993bf87350069511f509a1f480a80ce6b3184949d24b`.

Earlier failures remain recorded: missing host shader configuration, fixture label
ownership/view normalization, API/layout assumptions, teardown-test timing, Windows
PCH memory exhaustion and an unexpected prefix-selected benchmark. The final run
has a verified result; failed attempts were not relabelled as passes.

## Compatibility and remaining limits

Python stays at 0.3.0 and mesh replay schema 1. Native public types changed and
dependent modules require rebuilding. Existing capture defaults remain unchanged;
GPU sampling and common admission require explicit opt-in. Shader compilation and
component Skin Cache policy belong to the host/consumer, not the producer.

This is qualified bone geometry before material effects, not a final rendered
surface or evidence of physical contact, penetration or artistic quality. Arbitrary
animation graphs, leader/post-process/physics pose ordering, authored morph positive
controls, cloth, deformers, material effects, Nanite, other RHIs and GPU device-loss
recovery remain unqualified. Feature rejection guards do not replace positive
deformation tests. GPU failure envelopes are returned to the caller; the native
mesh bundle writer still writes completed snapshots only.

30 Hz, 960x540, larger meshes/subject counts and uncontended performance remain
separate qualification workloads. Shared reservation figures exclude driver/
allocator overhead, enrollment objects and caller-created copies. Metadata work
still runs per request; immutable topology caching is a possible next optimization.

Katana's owner retains adapter edits, dependency pins, consumer rebuilds and real
gameplay/asset qualification. The next step is that integration evidence, then a
focused positive experiment for the next chosen pose/deformation capability.
