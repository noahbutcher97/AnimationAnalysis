# Animated surface sampling: focused readiness checks

Recorded 2026-09-11 against UE **5.6.1, CL 44394996**, Windows, D3D11. This
extends the [initial feasibility probe](2026-09-11-skeletal-surface-sampling.md)
and [consumer requirements review](2026-09-11-katana-surface-requirements.md).
It supports starting the [capability-aware bone/rigid delivery](../SURFACE_CAPABILITIES.md);
it does **not** deliver a production mesh API or pass its complete acceptance matrix.

## Decisions

- Start production work with versioned capability/mesh records, rigid attachments
  and an explicit CPU bone reference; then qualify bounded cached GPU observations.
- UE 5.6.1 can order Skin Cache setup before the geometry query through exported
  APIs. No engine patch or consumer dependency update was needed for this probe.
- The selected Katana meshes have no stored morph, clothing, default-deformer or
  post-process animation assignments. Production morph support can follow the
  baseline. Keep a small positive morph experiment early in extension work.
- Katana has masked materials and connected pixel-depth offset. Triangle geometry
  remains useful, but cannot claim complete rendered visibility/depth coverage.
- No further broad reference survey or unfinished FBX-tool audit is needed before
  the first implementation slice. Remaining uncertainty calls for specific tests
  against the implemented contracts and actual consumer components.

## Katana asset inventory

An input-driven automation test loaded four explicitly named assets in the neutral
host with NullRHI. It temporarily mounted Katana's Content directory, queried asset
configuration, and saved observations only under AnimationAnalysis. It did not load
a Katana map, run its AnimGraphs, instantiate its gameplay classes or save packages.

All paths below are beneath `/Game/Assets/Characters/`:

| Asset | Resident render data | Material findings |
|---|---|---|
| `CyberpunkRunner/Meshes/SKM_CyberpunkRunnerr_B` | 32,451 vertices, one LOD, four sections | Three opaque materials without connected WPO/PDO. Material slot 3 is null **and used** by a 95-vertex, 64-triangle section; effective fallback/overrides require component inspection. |
| `FuturisticMercenary/Meshes/SKM_FuturisticMercenary_FullBodyC` | 38,374 vertices, one LOD, nine sections | Facial-hair sections 6 and 8 are masked and their base material has PDO connected. Other materials are opaque; no base material has WPO connected. |
| `Mannequins/Meshes/SKM_Manny_Simple` | 48,705 / 15,096 / 7,793 vertices, three LODs, two sections per LOD | Both material instances are masked; their base material has neither WPO nor PDO connected. |
| `CyberpunkRunner/Meshes/SKM_Katana` | **StaticMesh**, one LOD | Opaque sword material without connected WPO/PDO. The asset-name prefix does not identify its class. |

The three skeletal assets each report zero morph targets, zero clothing assets,
no clothing section mappings, no default Mesh Deformer and no post-process animation
class. These are asset facts, not proof of absent runtime overrides or procedural
bone animation. Material input connectivity is not proof of a nonzero active effect:
UE's `UMaterial::HasVertexPositionOffsetConnected` and
`HasPixelDepthOffsetConnected` query property connectivity. Effective component
materials, parameters, static switches, visibility, leader pose, weight overrides,
render LOD and dynamic deformation still need consumer qualification.

The before/after comparison found identical paths, sizes and modification times
for all **7,948 Content files**. This is a file-metadata preservation check, not a
before/after byte hash of every asset. Consumer code, assets and pins were not edited;
the consumer owner's concurrent working tree remains outside this change.

## Skin Cache task ordering

Epic's [UE-334281 issue](https://issues.unrealengine.com/issue/UE-334281) describes
a race between the Skin Cache setup task and `GetCachedGeometry`; it lists a fix
target of 5.7 and CL 46434643. Its public page does not provide reproduction steps
or an affected-version list. The corresponding authenticated Epic GitHub commit
`4bf286590545df6f25e1a7bf388c3195913a8e5a` was inspected locally.

Installed 5.6.1 has the earlier ordering: the section loop can read `SkinCacheEntry`
before `GetPositionBuffer` performs its setup wait. Epic's relevant change moves
`AddAsyncComputeWait(GraphBuilder)` ahead of that loop and makes the helper static.
The commit also contains an unrelated shader-precision change; it is not part of
this mitigation.

In 5.6.1, `FSceneInterface::GetGPUSkinCache()` and the instance
`FGPUSkinCache::AddAsyncComputeWait` are exported. The probe calls that wait on the
render thread **before** `GetCachedGeometry`. The helper waits for a pending CPU
setup task and establishes any required GPU queue transition; it does not perform
a device-wide idle drain. Its installed implementation does not dereference instance
state. The chosen deferred-renderer `PostRenderView_RenderThread` hook also follows
the renderer's own wait, which explains the negligible additional observed time.

Source anchors relative to `Engine/Source/Runtime`:

- `Engine/Private/SkeletalRenderGPUSkin.cpp`: `FSkeletalMeshObjectGPUSkin::GetCachedGeometry`.
- `Engine/Private/GPUSkinCache.cpp`: `GetPositionBuffer`, `AddAsyncComputeWait`, setup-task creation.
- `Engine/Public/GPUSkinCache.h` and `Engine/Public/SceneInterface.h`: exported access.
- `Renderer/Private/DeferredShadingRenderer.cpp` and `SceneRendering.cpp`: renderer
  wait and post-render view-extension ordering.

This is source-supported ordering plus a successful qualified probe, **not** a
reproduction of the race or proof that all hooks, rendering paths and RHIs are safe.
D3D11 does not qualify hardware asynchronous-compute scheduling. Production code
must retain explicit ordering and resource ownership rather than depend on observed
timing or project Skin Cache settings.

## Animated component and rigid attachment experiment

The neutral fixture uses a real `USkeletalMeshComponent`, two bones with a nonzero
bind pivot, 31 authored animation keys at 30 Hz, and normal single-node animation
evaluation. A finalization delegate records acquisition and subsequent finalizations.
An engine cube is attached to the moving bone. The skeletal component has a rotated,
nonuniform world transform and two LODs: 10,201 and 2,601 vertices. The fixture owns
its playback and LOD setup; the observer reads without forcing evaluation or changing
render skinning mode, LOD or pose revision.

An independent scalar oracle reconstructs normalized quaternion interpolation of
the authored keys, applies the bind pivot and actual quantized skin weights, and
compares it with CPU skinning. The fixture explicitly uses bitwise-only animation
compression with `ACF_None` formats and no frame stripping. This keeps the authored
oracle meaningful without assuming the default compressor preserves an ideal curve.

GPU positions are copied from cached geometry, with one outstanding copy and a
fixed 1 MiB copy cap. Polling is deliberately deferred for five subsequent view
callbacks. Live animation changes after acquisition; completion consumes the old
snapshot. The final case destroys the source actor/component before result
consumption, retaining the RHI source reference and readback resources separately.

Three final runs use identical experimental source hashes:

| Run | Native result | Geometry cases | Additional evidence |
|---|---|---|---|
| `animation-05` | Exact research automation test passed | 8 GPU copies | Matching synchronous raster; one source destruction before consumption. |
| `animation-async-01` | Exact research automation test passed | 8 GPU copies | Image capture disabled; one source destruction before consumption. |
| `animation-cache-off-01` | Exact research automation test passed | 8 explicit cache-unavailable results | CPU reference and matching inline-skinned raster remain available. No GPU substitution. |

Across these **24 cases**, maximum CPU/oracle error is **0.00001706 cm** against
the predeclared **0.001 cm** tolerance. Maximum GPU/CPU error is **0.00002289 cm**
against **0.005 cm**. The live pose differs from each retained snapshot by at least
**46.72 cm** before consumption. All cases witness a subsequent finalization and
unchanged observer-controlled state; copied position buffers contain every vertex.

The independent replay rasterizer projects recorded triangles and the rigid prop
through the acquired matrix, with perspective-correct depth and declared winding.
All **16** raster observations pass: per-subject silhouette IoU is at least
**0.9992857** (floor **0.99**), RGB/label foreground IoU is **1.0**, and maximum
interior depth error is **0.015203 cm** (limit **0.05 cm**). Each comparison has over
21,000 interior samples. A deliberate 20 cm geometry translation fails alignment,
with IoU at most **0.55912** against a required negative-control result below 0.9.
Frame identity and raw binary dimensions/length are checked during replay.

The raster run uses the existing **synchronous** diagnostic capture and therefore
contains rendering waits. The separate image-disabled run establishes delayed
geometry completion without those per-frame waits. Neither proves that the GPU was
physically still executing a copy at the instant of destruction; both prove safe
retention and consumption while a logical request remains outstanding. World teardown,
replacement, cancellation, saturation and sustained concurrent captures remain open.

## Performance observations and limits

These are warmed CPU microbenchmarks and deliberately delayed readiness observations,
not a disabled/CPU/GPU/combined frame-time comparison:

| Measurement | Observed range across final runs |
|---|---|
| CPU reference, 10,201 vertices | p50 0.513–0.515 ms; p95 0.638–0.642 ms |
| CPU reference, 2,601 vertices | p50 0.130 ms; p95 0.143–0.190 ms |
| Explicit setup-wait call at the selected hook | Maximum 0.0003 ms; no meaningful blocking task observed |
| Copy payload | 122,412 bytes at LOD 0; 31,212 bytes at LOD 1 |
| Image-disabled enqueue-to-ready | p50 98.04 ms, p95 98.93 ms, **including five deliberately deferred polls** |

The copy cap is not a combined memory budget: CPU arrays, retained topology, source
references, raster data and export also occupy memory. The probe uses an explicit
teardown GPU-idle drain whose duration was not measured here. Production qualification
must account for all ownership, measure teardown, and collect at least three comparable
repetitions of the workloads specified in the capability design. No speedup, GPU kernel
duration, 60 Hz throughput or production latency claim follows from these measurements.

## Remaining implementation work

Begin with the [record and replay implementation plan](../superpowers/plans/2026-09-11-mesh-observation-records.md).
Then promote CPU/rigid sampling, qualified GPU capture and combined admission into
the shared module and existing host. Keep the broader capability model, but defer
production morph/cloth/deformer/material-effect support and later penetration,
containment, volume and swept-contact analysis to independently qualified slices.

Required baseline work still includes multiple sections; component/topology/weight
generations; immutable completion without raw UObject access; actual supported pose
ordering; unknown/unsupported capability handling; strict replay limits; shared
capture/export budgets; exactly-once termination; and fault/lifetime controls.
Single-node finalization does not qualify parallel worker execution, repeated
finalizations within one frame, stale/skipped poses, post-process or physics blending.
The single-section fixture does not establish multi-section correspondence.

## Reproduction, failed setup evidence and compatibility

The ignored research directory is `Saved/SkeletalProductionProbe-20260911/`.
Recreate its host with the existing verifier's `--stage-only --prepare` workflow,
restore `animation-05/experimental-source/` into that copy, including Build.cs and
renderer configuration, and use fresh run names:

```powershell
python Saved/SkeletalProductionProbe-20260911/run_probe.py repeat-raster --build
python Saved/SkeletalProductionProbe-20260911/run_probe.py repeat-async --raster 0
python Saved/SkeletalProductionProbe-20260911/run_probe.py repeat-unavailable --cache 0
python Saved/SkeletalProductionProbe-20260911/analyze_raster.py repeat-raster repeat-unavailable
```

`analyze_production.py` validates the three named final archives, exact test results,
geometry, topology bounds, identity, timing observations, baseline hashes and consumer
file metadata. The consumer inventory is reproducible with `--inventory` and explicit
`asset-input.json`; it requires access to those consumer assets. The neutral animation
experiment itself requires no Katana content or FBX tooling.

Failed setup runs are retained: initial compile/name errors, playback assigned before
the registered single-node instance existed, a logging dereference after destruction,
incorrect authored triangle winding, and a default-compression/ideal-curve oracle
mismatch. The replay analyzer's initial front-face sign was corrected against the
declared projection/winding convention. Geometry and raster tolerances were not relaxed.
The default-compression run's successful raster comparison is retained separately
from its unsuccessful analytic checks; it is not included in the final pass count.

The final three probe source manifests are identical, and all **29 original
repository native/host source hashes** still match the staging baseline. Public
headers, plugin/module wiring, Python package behavior, existing record formats,
consumer code and dependency pins are unchanged. Existing package/native regression
results are historical and were not rerun for these documentation-only repository
changes. The modified experimental host was freshly built and ran the four exact
automation invocations described here (inventory plus three animation runs).

The complete local archive is `Saved/SkeletalProductionProbe-20260911-evidence.zip`,
SHA-256 `d3f966be216042e116a15fa0092ceec1c548d3a54cb1dc81c31b11ebaad7f137`.
All **168 payload entries** were read and hash-verified. Its nested replay archives
were verified separately, and all **40 raw raster bundles**, including failed-run
evidence, passed binary-layout decoding. The **80 inventoried loose copies** in the
host and retained run folders were then hash-checked and removed. No PNGs were
generated. Raw RGB/labels/depth remain recoverable from the archives; earlier research
archives were untouched. `image-cleanup-result.json`, `evidence-retention.json` and
the explicit manifests record the checks. Generated evidence stays out of Git.
