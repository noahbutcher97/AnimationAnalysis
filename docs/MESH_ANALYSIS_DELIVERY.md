# Sampled mesh analysis delivery

Verified 2026-09-13. Final executable implementation:
`89f0087` on `feature/mesh-surface-analysis`, based on `0813894`.
This additive Python 0.4.0 slice stays in AnimationAnalysis. Native capture,
consumer adapters, assets and dependency pins are unchanged. No remote publication
is part of this delivery.

## Delivered behavior

The [public API](MESH_ANALYSIS.md) measures explicit topology-bound triangle regions
from existing immutable mesh observations. It reports global surface distance,
exact squared distance, closest witnesses, sampled intersection and proximity under
caller-supplied criteria. Coverage, clock, pose and topology checks precede numerical
results. Exact arithmetic on stored numbers distinguishes a tiny positive gap from
intersection, including interior triangle crossings and coplanar overlap.

Bounded interval summaries retain acquisition gaps and failed samples. Incompatible
units, regions, requirements or temporal identities suppress aggregate numbers.
Incomplete but comparable samples can retain clearly partial observed minima/counts.
Neither a complete interval nor a nonintersecting sample proves between-sample behavior.
Containment is always `not_evaluated`; penetration depth and volume are not outputs.

## Verification

| Fresh check | Result |
|---|---|
| Isolated final wheel without image extra | 104 passed; one optional-image test skipped |
| Same wheel with image extra | 105/105 passed |
| Repository tooling suite | 26/26 passed |
| Package isolation and existing four CLI help commands | Passed; no new dependency or CLI added |
| Native isolation staging | 42 allowlisted files prepared; build and runtime explicitly not run |
| Native source comparison against `0813894` | All 42 match after line-ending normalization; staged copies match checkout bytes |
| Final wheel source comparison | All 28 Python modules match checkout bytes |
| Installed-wheel replay of archived native records | Three schema-1 GPU bundles passed at two LODs; same-region intersection controls also passed |

The 53 new controls comprise 11 triangle-kernel, ten bounded-search, twenty public
measurement and twelve interval tests. They include independent analytic distances,
interior crossings, coplanar/edge cases, nested cubes, reversed winding, affine
transforms, degenerate selected geometry, exhausted limits, replay round trips,
source-buffer independence, incompatible identities and exact clock-gap decisions.
Synthetic omitted cloth/morph/material effects that would change the answer remain
insufficient when required; explicit bone-reference exclusions remain in provenance.

Independent reviews covered geometry, eligibility, bounded search, result storage,
serialization and intervals. Fixed findings include conflicting poses on a shared
stream, premature squared-distance underflow, and loss of required subject/stream
identity during interval criteria comparison. Regression controls passed after fixes.

The final distribution is `Saved/MeshAnalysisWorktree/Saved/MeshAnalysisDistribution-02/`.
Wheel: `animation_analysis-0.4.0-py3-none-any.whl`; SHA-256:
`225646f8a0366b5e6621fcc73021ab01b2266be6fdaa76375273acb67b310f66`.
The earlier `-01` distribution predates the last review fix and is retained as
superseded evidence, not the delivery candidate.

Reproduction from the repository root:

```powershell
python Python/verify_distribution.py --output Saved/MeshAnalysisDistribution
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/MeshAnalysisHost --stage-only --output Saved/MeshAnalysisNativeIdentity
```

Tooling ran with `Python/src` and `Python` explicitly on `sys.path`, using unittest
discovery at `Python/tooling_tests`; its log is archived. The qualification script
ran with `-I` through a fresh virtual environment installed from the final wheel,
and asserts its imports are inside that environment. Its exact inputs, script,
source identities, native record hashes and measurement outputs are retained.

## Offline performance and replay

Environment: CPython 3.11.9, Windows build 26200, 24 logical processors, uncontrolled
desktop workload. Fixed synthetic parallel panels contain three unique vertices per
triangle, separated by exactly 3 cm. Each case has one warmup and three measurements
with rotated order. Timings include observation hashing, transforms/validation, tree
construction and search; they exclude input construction and result serialization.

| Triangles per side | Median | Range | Triangle tests / node visits |
|---|---:|---:|---:|
| 64 | 16.31 ms | 15.63–16.59 ms | 1 / 25 |
| 512 | 136.23 ms | 134.79–138.51 ms | 1 / 37 |
| 4,096 | 1,234.08 ms | 1,224.42–1,244.95 ms | 1 / 49 |

Every repeat returns exact squared distance 9. These favorable layouts prune to one
triangle test; they do not establish worst-case mesh performance or real-time use.
An overlapping-AABB/disjoint-triangle control reaches its eight-pair limit after
22 node visits in 15.01 ms and returns insufficient with no partial minimum.
Limits for normal runs were 50,000 triangles per side, 100,000 pair tests,
200,000 node visits and 256 coordinate bits. These bound work counts, not wall time
or process RSS. No native capture performance or speedup was measured in this slice.

Three archived native records were replayed with explicit bone/pose-ordering
requirements and accepted exclusions for morph, cloth, mesh deformer, material
displacement and raster visibility. First-32 versus last-32 triangle regions give:

| Record | Total triangles | Distance | Pair tests / node visits | One offline call |
|---|---:|---:|---:|---:|
| Fine GPU | 20,000 | 77.352721 cm | 7 / 41 | 25.88 ms |
| Coarse GPU | 5,000 | 85.810692 cm | 13 / 111 | 40.51 ms |
| Matching-raster GPU | 20,000 | 77.352721 cm | 7 / 41 | 24.23 ms |

These values demonstrate installed analysis of retained native geometry; they are
not independent acquisition-accuracy measurements or fresh renderer/consumer tests.
Prior native qualification remains in [the GPU delivery](NATIVE_MESH_GPU_DELIVERY.md).

## Compatibility, evidence and remaining work

Python advances from 0.3.0 to 0.4.0 through additive exports. Mesh replay remains
schema 1; existing native and image defaults are unchanged. New measurement and
interval mappings each declare their own format/schema 1 and
`exact-triangle-surfaces-v1` method. Consumers opt in through callable APIs and
provide their own region mappings, requirements, clocks and criteria.

Replay came from `Saved/GpuMeshDelivery-20260913-evidence.zip`, verified SHA-256
`9859576bb976eef5e98b28129fc3c5e2e264b0e457f15c97ca7e0b88d1b9cd9b`.
New archive: `Saved/MeshAnalysisDelivery-20260913-evidence.zip`; SHA-256
`b7390e00b5d9b29ca92037d2e174ebde813763b80f4c24c29d11267c38ef5c10`.
All 147 payload entries passed size/SHA-256 readback; the ZIP has 148 members
including its inventory. It contains both distribution attempts, the qualification
script and outputs, twelve replay input files, tests, Python/native source snapshots
and the implementation patch. The receipt is
`Saved/MeshAnalysisDelivery-20260913/archive-verification.json`.
This slice generates no images and performs no image cleanup; previous replay
archives remain unchanged.

Next, the consumer owner should select one useful region-pair workflow, provide
topology-bound mappings and criteria, inventory effective pose/deformation coverage,
and run the same measurements on its retained acquisitions. Katana owns pin updates,
project builds and integration tests; none were run here. Reassess the
[conditional deferral](research/2026-09-13-deformation-deferral-review.md) against that
evidence before claiming the bone subset is adequate. Neutral omitted-effect tests
protect the contract but do not qualify live feature detection or final rendered surfaces.

Containment needs its own closedness/self-intersection rules and controls. Swept
intersection, duration, penetration depth/volume, support/sliding and artistic or
physical-contact judgments remain separate work. Broader live pose paths, morphs,
cloth, deformers, material effects and other RHIs retain their documented limitations.
