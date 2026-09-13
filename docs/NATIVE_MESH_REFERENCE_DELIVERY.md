# Native mesh reference delivery

Recorded 2026-09-11. Native implementation commit:
`4a2975a97b23271f17b1d48ce2ccb470de6074b5`.
This delivery adds explicit CPU bone/rigid acquisition, immutable retained snapshots,
shared CPU admission, conservative effective-feature inventory and Python-compatible
mesh replay. [API, supported inputs and limits](NATIVE_MESH_REFERENCE.md) define its
scope. It does not deliver cached-GPU mesh acquisition or full rendered surfaces.

## Verification

Environment: UE 5.6.1 CL 44394996, Windows 11 25H2, Intel Core Ultra 9 275HX,
NVIDIA RTX 5090 Laptop GPU using D3D11, Python 3.11.9. The neutral host builds from
34 staged files with Engine and AnimationCapture dependencies only.

| Check | Result |
|---|---|
| Final independent host build and rendered suite | 18/18 tests pass; no missing, duplicate, unexpected or skipped controls |
| New mesh reference control | Real single-node animation; two LODs/two sections; rigid attachment; analytic positions; observer state unchanged; repeated same-frame revision; effective-weight identity; missing weighted/socket bone; material/asset replacement; reference-pose/leader rejection; stale update; retirement; shared count/byte saturation and release; interactive create/sample/stop |
| Canonical topology control | Independent Python SHA-256 value matches native Unicode/DEL escaping, null material and multiple-section topology |
| Native-to-Python replay | Installed Python 0.3.0 reads fine/coarse/rigid bundles from both final-source runs; all eight bundles, including interactive samples, also replay from ZIP entries alone |
| Distribution isolation | 51 core tests pass plus one optional-image skip; all 52 pass with images; four CLI entry points pass |
| Source and review | All 34 native source hashes match the final run; all 24 package source files match the wheel; scoped review has no outstanding blockers |

The independent scalar animation oracle uses authored quaternion interpolation,
quantized effective weights and the nonzero bind pivot, without sampled engine
matrices. Maximum error across the final-source runs is **0.00001307 cm**, below
the fixed 0.001 cm limit. Live pose changes exceed 36 cm while retained data remains
unchanged. This proves the qualified bone reference, not later deformation or contact.

Reproduction commands, from the repository root with fresh output names:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/NativeMeshHost --output Saved/NativeMesh-Verification --rendered --run-tests --delete-pngs
python Python/verify_distribution.py --output Saved/NativeMeshDistribution
```

Run `Python/verify_mesh_reference.py <MeshReference-directory>` from an installed
package environment with `--max-vertices 20000 --max-indices 100000 --max-sections 16
--max-payload-bytes 2000000 --max-metadata-bytes 65536 --max-record-bytes 2065536`.
Its report preserves native oracle results and independently checks replay integrity.

## CPU measurements

Fixed topology: 10,302 vertices, 60,000 indices (20,000 triangles), two sections.
Each process records three batches of 30 timed acquisitions after five warmups per
batch; pose stays fixed within the run. The two final-source processes retain all
180 samples. Times include geometry validation, CPU skinning, identity hashing and
inventory construction; they exclude file export and total rendering.

| Run | Batch-median range | Batch p95 range | Batch p99 range |
|---|---:|---:|---:|
| Focused final-source controls | 2.185–2.197 ms | 2.528–2.716 ms | 2.570–2.774 ms |
| Full rendered regression | 2.075–2.172 ms | 2.549–2.681 ms | 2.617–2.820 ms |

Percentiles use nearest rank; with 30 samples, p99 is the batch maximum. Peak shared
reserved capacity is **7,774,840 bytes** in both runs. Expected admission failures are
tested explicitly. This synchronous service has no pending queue or asynchronous
latency distribution. These measurements are a local CPU baseline, not an image/GPU
throughput comparison, RSS measurement or long-running production benchmark.

## Evidence and cleanup

`Saved/NativeMeshDelivery-20260911-evidence.zip` retains 372 verified entries,
including source snapshots, commands, successful/failed run logs, raw timings,
distribution wheel, replay ZIPs and the review/cleanup receipts. SHA-256:

`083048ec0304d2ed1972eeae12c0468268eed90316d09d69e9d6d5a3ba186ebb`

The final native replay ZIP independently contains 107 verified entries; SHA-256:
`8c6bb53836990bd7dcbed772a9328a3d4cefebb14b67e9eb7e08a6effec46312`.
Its 22 generated PNGs were archived and verified before deleting both output and
retained-host copies: **44 loose images removed**. Mesh replay remains independently
readable. Earlier evidence archives were preserved.

The final wheel SHA-256 is
`22bfcbc54293286faf4fbad55d4d1f52af480e9ec5d7c83ce7e15266785c1496`.
Early runs retain compile API errors, a shared UBT log collision, compiler memory
pressure and missing fixture finalization after configuration changes. The final
source passes without relaxing observer rejection or geometric tolerances.

## Compatibility and next boundary

The C++ API is additive; existing image/session defaults, records and module
dependencies remain unchanged. Only the neutral host adds procedural mesh/animation
build dependencies. Python remains 0.3.0 and mesh replay remains schema 1. Existing
consumers need no edits until explicitly adopting this capability.

Katana's owner owns adapter changes, pins, rebuild and affected gameplay integration;
this delivery makes no Katana runtime-pass claim and edits no consumer assets.
No remote publication is part of this delivery.

Next implement bounded cached-GPU sampling with combined image/mesh admission,
source lifetime, cancellation/timeout and matching-renderer qualification. General
animation graph/IK/physics ordering, morphs, cloth, deformers, material displacement,
visibility, additional RHIs and physical-contact analysis remain separately scoped.
