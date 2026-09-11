# Skeletal surface sampling: source investigation and neutral experiment

Follow-up evidence: [animated surface readiness](2026-09-11-animated-surface-readiness.md)
adds a real animation sequence, rigid attachment, independent raster replay, source
retirement and a loaded consumer asset inventory. This report retains the original
poseable-fixture evidence and its narrower proof limits.

Recorded 2026-09-11 against AnimationAnalysis `3fd91eb70be340db63778a697b12465ab895cd8a`
and installed UE **5.6.1, CL 44394996**. This is a feasibility report, not a completed
skeletal capture delivery or an adopted public contract. Prototype code is throwaway
research evidence under `Saved/SkeletalSamplingResearch-20260911`; it is not compiled
into the tracked host or plugin.

## Recommendation

Product direction clarified after this experiment: target broadly reusable, robust
surface analysis with explicit user choices for deformation coverage, fidelity and
resource cost. Use Katana as an accessible reference project and first consumer to
discover practical needs and validate usefulness, alongside a broader capability
matrix. Its current asset/rendering configuration does not define the product's
coverage. See [product direction](../MIGRATION.md#product-direction-and-reference-consumer).

The subsequent [capability design](../SURFACE_CAPABILITIES.md) and
[Katana requirements assessment](2026-09-11-katana-surface-requirements.md) now
provide that matrix and a proposed production slice. They do not extend this
experiment's proof or establish active consumer deformation coverage.

Use **explicit CPU bone-skinned geometry** as a correctness reference and a supported
option where its deformation coverage is sufficient. The tested route uses
`GetCurrentRefToLocalMatrices` and `ComputeSkinnedPositions` for a declared, resident
LOD, keeping immutable positions, component transform, topology identity, clock and
pose provenance together. Its successful experiment does not establish that bone-only
sampling satisfies all surface-analysis requirements or the first consumer's needs.

Treat **cached GPU geometry as a separate observation capability**, requiring a
validated acquisition/lifetime contract. The experiment proves that UE 5.6 D3D11
Skin Cache positions can be copied asynchronously and match the
earlier CPU geometry through a subsequent pose change. It does not establish a
general production capture path or complete final rendered-surface coverage. CPU
versus GPU is an implementation choice, not a sufficient fidelity classification.
Do not silently substitute bone-only geometry when a caller requested renderer-deformed
evidence: return unavailable, or a separately labelled
CPU observation when explicitly requested.

Neither path establishes contact, penetration, containment or artistic quality.
Those require later geometry operations, supported topology and explicit criteria.
Keep the existing RGB/depth interfaces and defaults unchanged.

## References and source findings

Installed source is the authority for this engine build. Paths below are relative
to `UE_5.6/Engine`; `source-review.json` retains hashes of 14 inspected source files.

| Source | Finding and consequence |
| --- | --- |
| `Source/Runtime/Engine/Private/Components/SkinnedMeshComponent.cpp:4613`, header `:563` | `GetCPUSkinnedVertices` forces LOD, refreshes the pose and switches CPU skinning on/off. Its documented render-thread flush is implemented through `SetCPUSkinningEnabled` (`:1812`), which can also wait for streaming. Reject it as the normal observing recorder API. |
| Same implementation `:4000`, `:4024`, `:5771`; `Private/SkeletalRender.cpp:474` | `GetCurrentRefToLocalMatrices` follows engine reference-pose, leader and visibility handling; `ComputeSkinnedPositions` applies section bone maps and effective weights to reference positions. No morph, cloth or material displacement is added by that position loop. Validate required data first: finite output alone does not prove a valid current pose. |
| `Private/Components/SkinnedMeshComponent.cpp:3865` | Both `GetSkinnedVertexPosition` overloads call the uncached helper in this installed version, including the overload accepting cached matrices. Do not assume the latter uses the supplied matrices. The probe uses the all-vertex helper instead. |
| `Private/SkeletalRenderGPUSkin.cpp:2434`; `Public/CachedGeometry.h` | `GetCachedGeometry(GraphBuilder, ...)` rejects inline skinning, exposes render LOD and component transform, and can return individual unavailable sections despite an overall successful result. Validate every required section. The API also contains a mesh-deformer branch; that branch was not exercised. |
| `Private/GPUSkinCache.cpp:2560`, `:390` | Position access inserts the Skin Cache async-compute dependency. Its position buffer is three float components per vertex. Copy while the resource is valid; delayed work must own its resources. |
| `Public/GPUSkinCache.h:190` | Deprecated `GetUpdatedFrame` returns zero. It cannot serve as an acquisition witness. |
| `Shaders/Private/BasePassVertexShader.usf:43` | Material world-position offset is applied after vertex-factory world position. Cached skinning positions are not universally the final material-displaced raster surface. |
| `Private/Components/SkeletalMeshComponent.cpp:4903`; `Classes/Components/SkinnedMeshComponent.h:2169` | Skeletal-component finalization broadcasts after queued animation events, which can destroy the component. The base registration method is a no-op unless a subclass implements it. Revalidate weak references and distinguish supported finalization witnesses from missing ones. |

[Epic's UE 5.6 rendering-path overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/skeletal-mesh-rendering-paths-in-unreal-engine?application_version=5.6)
explains inline skinning, Skin Cache and mesh deformers. Cache eligibility is not
guaranteed residency: memory pressure and LOD transitions can cause fallback.
The [ozz-animation skinning sample](https://github.com/guillaumeblanc/ozz-animation/blob/master/samples/skinning/sample_skinning.cc)
provides a useful independent reference for sampling, local-to-model transforms,
joint remapping and inverse-bind matrices. No ozz or FBX dependency was introduced.
An FBX_Tool audit or Autodesk importer investigation is unnecessary for this slice;
revisit those only for an explicit offline acquisition/reuse requirement.

## Experiment and proof boundary

The existing verifier staged an independent host beneath the repository's ignored
`Saved` directory. Only that copy gained the prototype and engine mesh-construction
module dependencies. A transient procedural mesh supplies two bones, a nonzero bind
pivot, continuously varying weights, one section, and two known LODs:

- LOD 0: 10,201 vertices and 20,000 triangles.
- LOD 1: 2,601 vertices and 5,000 triangles.

Each run holds four authored bend angles (0, 30, -30, 60 degrees) at each forced
LOD. Scalar trigonometry supplies independent expected positions using the actual
quantized weights; a separate check bounds their difference from authored weights.
The component has translation, rotation and nonuniform scale. CPU acquisition runs
at `BeginRenderViewFamily`; GPU access/copy runs in the matching renderer view.
After acquisition, the fixture changes the live bend by 70 degrees and waits at
least four engine frames before consuming the saved result.

The final probe checks:

- CPU versus analytic geometry within 0.001 cm, and GPU versus CPU within 0.005 cm.
- Matching render/acquisition LOD and component transform when GPU data exists.
- No CPU sampling change to the observed pose revision, forced LOD, skinning mode
  or mesh object.
- A live-versus-snapshot difference exceeding 10 cm, while copied GPU positions
  still match the earlier snapshot.
- Explicit GPU unavailability with Skin Cache disabled.

Three identical-source Skin Cache runs and one cache-disabled run passed the exact
`AnimationAnalysis.Capture.Research.SkeletalSampling` automation result: **32 pose/LOD
cases**, including 24 successful GPU copies and eight unavailable-GPU controls.
Offline replay independently recomputed the position errors from archived arrays,
rejected a deliberately displaced vertex, rejected acquisition/current pose identity
substitution and rejected subtraction across unrelated clock domains.

Worst observed CPU/analytic error was **0.00001079 cm**; GPU/CPU error was
**0.00001630 cm**. These are numerical agreement results for this fixture, not
universal accuracy guarantees. Mutation occurred later in the same engine frame as
acquisition; an engine-frame number alone therefore does not identify a pose.
The saved `live_pose_revision` is read at consumption, not at the earlier mutation.

The poseable fixture controls bone matrices directly. It does **not** validate an
animation sequence, AnimGraph, parallel evaluation, post-process animation, physics
blending or production finalization-delegate ordering. No rendered silhouette/depth
comparison was performed for this skeletal fixture. The GPU comparison establishes
agreement with the cached skinning buffer, not pixel visibility or final materials.

## Performance and bounds

Environment: Windows build 26200, Intel Core Ultra 9 275HX, RTX 5090 Laptop GPU,
NVIDIA driver 610.74, D3D11, UE Development editor, neutral 640x480/no-AA view.
No other Unreal editor process was running during the probe measurements.

CPU timing includes matrix acquisition, all-vertex evaluation and its output-array
allocation behavior. Per pose, ten warmups precede 120 timed calls, performed after
readback completes. Each LOD has 480 samples per run. The table shows the range of
per-run percentiles across the three Skin Cache runs (2,880 CPU measurements total).

| Vertices | CPU p50 (ms) | CPU p95 (ms) | CPU p99 (ms) | Position copy payload |
| --- | --- | --- | --- | --- |
| 10,201 | 0.606–0.720 | 1.001–1.370 | 1.090–1.701 | 122,412 bytes |
| 2,601 | 0.132–0.189 | 0.188–0.334 | 0.253–0.413 | 31,212 bytes |

Across 24 GPU observations, acquisition-to-ready latency was **17.55–19.30 ms**;
enqueue-to-ready was **14.19–17.29 ms**. Readiness was polled once per rendered view,
so these are observed completion delays, not GPU execution durations. The earlier
exploratory `cache-on-04` run included a CPU benchmark before copying and is excluded
from these latency/performance results.

This is a CPU microbenchmark and a small readback feasibility measurement. There
is no disabled-versus-enabled steady-state frame-time comparison, GPU timing query,
many-subject workload or production frame-budget acceptance yet. Do not extrapolate
from two bones to arbitrary character complexity or call GPU sampling faster.

The prototype admits one copy at a time and caps its copy payload at 1 MiB. Both
LODs fit that cap. Payload sizes exclude renderer-owned buffers, RHI overhead,
reference geometry, matrices, additional CPU arrays and JSON export. Total pipeline
peak memory, saturation/backpressure, cancellation, timeout recovery, asset streaming
and in-flight destruction remain unverified. A teardown-only GPU drain protects the
probe's raw mesh-object reference; it is not a production lifetime design.

## Requirements for the implementation slice

Use existing `ClockStamp`, `PoseKey` and projection conventions rather than creating
another identity system. Define a versioned mesh observation alongside the existing
raster bundle, with these explicit fields and failure distinctions:

1. Subject/component instance, acquisition serial, clock, observed pose/finalization
   provenance, and separately recorded completion. A missing or stale witness is
   missing evidence; sampling must not force animation evaluation to manufacture it.
2. Acquisition component-to-world transform, units, coordinate space, asset/render
   data generation, resident LOD, section ranges, effective weight identity and
   topology identity. Treat render vertices as LOD-local identities, not persistent
   material points. LOD/topology changes invalidate triangle/barycentric references.
3. Declared deformation coverage: bone-only CPU, or a specifically validated cached
   GPU backend. Morphs, cloth, deformers, WPO, hidden sections/bones, leader poses,
   reference-pose overrides and alternate weights need positive and unsupported
   controls before their evidence is accepted. Source support is not runtime proof.
4. Explicit limits on vertices, indices, resident topology caches, pending snapshots,
   staging and queued export, with admission before allocation and observable losses.
   Carry resource ownership through delayed completion and retirement; audit the
   combined RGB/depth/mesh budget rather than duplicating its allowance.

The follow-up capability design supplies a proposed slice and acceptance criteria;
the Katana source assessment identifies useful workflows but leaves active asset
deformation coverage unverified. Complete that inventory before consumer adequacy
claims. Distinguish what users request from what each backend can establish. Preserve CPU geometry as a
correctness baseline; add real skeletal-component finalization controls, renderer
acquisition and the full delayed lifecycle/budget matrix as required by that slice.
Reuse neutral geometry expectations when promoting a fixture into the maintained
host. Katana's developer continues to own consumer adapters, assets, dependency
pins and production integration tests.

## Reproduction, retention and compatibility

The experimental runner and analyzer are in
`Saved/SkeletalSamplingResearch-20260911/run_probe.py` and `analyze_probe.py`.
Every final run includes `experimental-source/` with the exact prototype C++, host
Build.cs, renderer configuration and driver. The original staging manifest records
the baseline plugin/host sources; each run records its actual source hashes.

To reproduce, use the baseline checkout and the existing verifier's `--prepare`
with `--stage-only` to recreate `Saved/SkeletalSamplingResearch-20260911/Host`.
Copy the archived experimental C++ into its host module's `Private/` directory,
replace that copy's host Build.cs, and restore its `Config/DefaultEngine.ini`.
The probe requires no authored asset files or FBX SDK.

```powershell
python Saved/SkeletalSamplingResearch-20260911/run_probe.py repeat-on --build --cache 1
python Saved/SkeletalSamplingResearch-20260911/run_probe.py repeat-off --cache 0
python Saved/SkeletalSamplingResearch-20260911/analyze_probe.py repeat-on repeat-off
```

Use new output names. The driver records commands, deadlines, exact automation
results and source identities, and verifies the replay archive before retaining it.
The final four run archives are named `replay-evidence.zip` beneath
`cache-on-final-01`, `cache-on-final-02`, `cache-on-final-03` and
`cache-off-final-01`. Their hashes and replay checks are in `replay-analysis.json`.

The complete local evidence archive is
`Saved/SkeletalSamplingResearch-20260911-evidence.zip`, SHA-256
`8309192e53d91651333b34085d240c36fedbc436abb47232eadbcae163cf2c8c`.
All **242 inventoried entries** were read and hash-verified. All **27 native-regression
PNGs** were fully decoded from their replay archive before the inventoried loose
copies were removed. The skeletal probe generated no images. The source check
confirmed identical source hashes across the four final probe runs and all 29
baseline native source hashes against the current repository. Inventory and cleanup
results are in `evidence-inventory.json` and `evidence-retention.json` beneath the
research output directory. Generated hosts, binaries, logs and archives remain out
of Git.

Failed setup evidence is retained: the first compile, an unfinished reference
skeleton scope, a missing skeleton object, and an invalid parameterless RDG pass
flag. The last crashed research editor was terminated by its verified process ID;
no unrelated editor was stopped. These are probe construction failures, not
production capture regressions. The replay analyzer also initially assumed mutation
must occur on a later engine frame; recorded frame/revision evidence corrected that
assumption without changing geometry tolerances.

Fresh checks of the unchanged deliverable also passed:

```powershell
python Python/verify_distribution.py --output Saved/SkeletalSamplingResearch-20260911/python-isolation
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/SkeletalSamplingResearch-20260911/native-isolation --rendered
```

The isolated wheel passed 24 core tests with one optional-image skip, and all 25
tests with the image extra. The isolated native host built and passed all 16 exact
rendered/portability controls with the original 29 source files; its temporary host
was removed after replay retention. These existing controls do not extend skeletal
coverage beyond the separate probe described above.

No public headers, plugin/module wiring, Python package behavior, consumer files or
pins changed. Existing capture compatibility remains as documented in
[the rendered readback delivery](../RENDERED_READBACK_DELIVERY.md). This report and
the experimental evidence are the output of the research pass; production skeletal
sampling remains unimplemented.
