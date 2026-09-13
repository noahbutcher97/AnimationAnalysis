# Shared suite development handoff

Updated 2026-09-13. Read [repository instructions](../AGENTS.md), the
[API and verification guide](../README.md), then [whole-suite migration](MIGRATION.md).
This document records the accepted first-delivery scope and consumer integration
boundary. Current implementation, commands and results are in
[delivery evidence](RENDERED_READBACK_DELIVERY.md), [host workflow](HOST_WORKFLOW.md)
and [asynchronous API compatibility](ASYNC_READBACK.md).

The next implemented slice is [native CPU bone/rigid reference sampling](NATIVE_MESH_REFERENCE.md).
Its [delivery report](NATIVE_MESH_REFERENCE_DELIVERY.md) records verification and limits.
The next slice, [bounded GPU bone sampling and shared image/mesh admission](NATIVE_MESH_GPU.md),
is implemented; its [delivery report](NATIVE_MESH_GPU_DELIVERY.md) gives exact
tests, performance and archived replay. Python 0.4.0 now implements
[sampled mesh surface analysis](MESH_ANALYSIS.md); its
[delivery report](MESH_ANALYSIS_DELIVERY.md) separates fresh offline qualification
from historical native evidence. Katana's
owner retains consumer edits, dependency pins, rebuilds and gameplay verification.

## Verified starting point

Implementation baseline: `ba13149d3318f80d3098958cd1d2cd52bba3e5d1`.
At extraction, KatanaCombat consumed that exact revision through its dependency
lock. Shared-suite delivery does not update the consumer pin or its installed code.

| Surface | Evidence at extraction | Limit |
|---|---|---|
| Installed Python package | 24 core tests pass without Pillow; all 25 pass with the image extra; four CLI entry points exercised | Offline services only |
| Independent Unreal host | Editor build and two native lifecycle/extension controls pass | Runs with `NullRHI` and image cadence zero; does not exercise GPU acquisition |
| Katana consumer | Editor build, 77 Python tests and six dependency tests pass | Focused integration verification, not a full combat baseline |
| Rendered Katana controls | Four D3D11 PIE/surface tests plus a completed finisher capture and offline evaluation pass | Existing synchronous capture; no continuous skeletal surface or penetration claim |

These are recorded results, not tests rerun for this documentation commit.
Reproduction commands are in the README. The original verifier used NullRHI only;
the first delivery adds `--rendered`, retained prepare/launch, and explicit readback
performance controls. Consult the delivery report for the new standalone results.

The next delivery now has a [skeletal sampling research report](research/2026-09-11-skeletal-surface-sampling.md).
It records UE 5.6 source findings, a neutral CPU/GPU geometry experiment, repeated
measurements, retained replay evidence and the remaining contract/lifetime work.
Its prototype remains research evidence. The separate native reference API now
qualifies a narrow subset; no consumer dependency change is part of that delivery.

Product direction is recorded in [migration and reference-consumer guidance](MIGRATION.md#product-direction-and-reference-consumer).
Target broad, robust capabilities with explicit user choices for fidelity and cost.
Use Katana's accessible workflows and assets to inform requirements and first-consumer
validation, alongside broader capability coverage and neutral fixtures. The bone-only
experiment is a baseline, not evidence that the product's surface requirements are met.

The [surface capability design](SURFACE_CAPABILITIES.md) now records the broader
coverage matrix, independent user choices, proposed first production slice and
qualification criteria. Its [Katana assessment](research/2026-09-11-katana-surface-requirements.md)
uses a hashed working-tree snapshot, including the owner's integration report for
`3fd91eb`. This is documentation/source evidence; no consumer tests were rerun.
Body/weapon surfaces, authored/live provenance and combined budgets are concrete
needs. The [focused readiness checks](research/2026-09-11-animated-surface-readiness.md)
now load the selected assets: no stored morph/clothing/default-deformer/post-process
assignments, but masked materials and connected pixel-depth offset. They also pass
real-animation, rigid-attachment, delayed-readback and matching-raster probe controls,
with a source-supported Skin Cache setup-ordering mitigation for UE 5.6.1.
Runtime component/material overrides and complete production acceptance remain open.

The [mesh record/replay slice](MESH_RECORD_DELIVERY.md) is now implemented in Python
0.3.0: immutable geometry and request identities, explicit coverage eligibility and
bounded replay with exclusive publication. Its [contract guide](MESH_OBSERVATIONS.md)
and [completed plan](superpowers/plans/2026-09-11-mesh-observation-records.md) define the
native producer's wire contract. The CPU/rigid reference and conservative effective
inventory and bounded GPU/shared-budget slice are implemented. The first core mesh
analysis slice now provides topology-bound regions, surface distance, sampled
intersection and interval/gap reporting. Containment remains explicitly not evaluated.
Next obtain consumer-owned integration evidence alongside the neutral controls:
select useful region pairs and criteria, inventory their required surface coverage,
and qualify additional live pose acquisition only where it blocks that workflow.

Priority clarified 2026-09-13: morphs, cloth and material deformation are stretch
goals after core functionality, offering optional higher fidelity and broader use.
The [deferral review](research/2026-09-13-deformation-deferral-review.md) makes this
ordering conditional on the selected workflow's required surface and live pose
support. An early experiment needs a concrete coverage or contract question;
defer production extensions without deferring that assessment. Reassess at the
first usable analysis slice with consumer evidence. Keep omitted coverage
explicitly unsupported or unknown where appropriate. No further broad
tool audit is needed. No engine patch or consumer dependency update is included
in either native delivery.

## Development host and production integration

Use the existing `Python/UnrealHost/AnimationCaptureHost.uproject` as the shared
suite's small UE development/sample host. It is a real UE 5.6 editor project with
an Engine/plugin-only module. Extend this one host to serve both repeatable
automation and interactive inspection. The verifier supports temporary isolated
copies and a retained, ignored `--prepare`/`--launch` workflow. Both use one source
allowlist and the same fixture definitions; see the host workflow.

| Test location | Responsibility |
|---|---|
| Portable Python tests | Known geometry, clocks, identities, file integrity, decoders and analysis without Unreal |
| Shared Unreal host | Real renderer/viewport/world behavior, controlled RGB/depth/label truth, readback performance and lifecycle failures, independently of Katana |
| KatanaCombat integration | Production animations, motion warping, paired interaction/death/interruption, telemetry and project adapter compatibility |

Start the rendered host with the existing camera/primitive controls. As supported
features expand, add small neutral skeletal/animation fixtures, moving props,
occluders, multiple subjects and reproducible teardown/resize cases. Use the same
fixture definitions for interactive inspection and automated capture so their
expectations cannot diverge. Keep fixture source and necessary authored inputs
tracked; build products, prepared host copies and capture output remain generated.
Test fixture actors and expected layouts belong in host/test modules, outside the
capture implementation. Add content only when it is needed to exercise a capability.

Neutral controls provide known expected outcomes but cannot establish compatibility
with every real game. Katana remains the production consumer check, owned by its
developer under the integration process below. The shared developer can inspect
its source and recorded evidence, then provide a candidate commit and requested
integration scenarios. Performance runs on this workstation are scheduled to avoid
simultaneous GPU workloads. There is no automatic update of Katana's live dependency.

## First delivery: neutral rendered controls, then asynchronous readback

1. Establish a rendered D3D11 fixture in the neutral host using explicitly supplied
   primitives and engine content. Reuse the known-geometry controls described below.
   Give it reproducible automation and interactive prepare/launch workflows,
   deadlines, exact test-result checks and retained evidence. A headless skip must
   remain distinguishable from a rendered pass.
2. Record the current synchronous baseline in that host before changing acquisition.
   Keep existing lifecycle tests. Verify RGB/label/depth alignment independently of
   gameplay telemetry and preserve the control geometry, view policy and tolerances.
3. Implement bounded asynchronous acquisition for RGB and scene/custom depth through
   reusable producer APIs. The existing surface diagnostic and session RGB recorder
   should share appropriate mechanisms. Keep subject discovery, skeleton choices,
   scenarios and quality criteria in consumers. Deliver this scope before moving
   skeletal sampling or geometric penetration work.

Implementation entry points:

- `Source/AnimationCapture/Private/ViewportSurfaceCapture.cpp`: `ReadDepth` creates a
  GPU readback, blocks until GPU idle and immediately locks it. `Collect` also takes
  a synchronous RGB screenshot. Destruction currently flushes rendering commands.
- `Source/AnimationCapture/Private/AnimationCaptureReadback.cpp` and
  `ViewportAsyncCapture.cpp`: bounded reusable producer, renderer witnesses,
  fixed-buffer RGB/depth packing, nonblocking polling and explicit teardown drain.
- `Source/AnimationCapture/Private/AnimationCaptureSession.cpp`: `Draw` preserves
  synchronous RGB by default and supports explicit async opt-in; PNG encoding uses
  the bounded `AnimationCaptureImageWriter` with a combined pipeline reservation.
- `Python/UnrealHost/Source/AnimationCaptureHost/Private/AnimationCaptureHostTests.cpp`
  and `Python/verify_unreal_host.py`: independent lifecycle controls and launcher.
- Public capture types, Python producer adapters and evidence readers are consumers
  of the observation contract; update compatibility and tests with any format change.

### Acceptance criteria

- Each admitted request retains immutable acquisition identity: session/request,
  viewport/view, renderer frame, time and clock, projection, dimensions and observed
  pose/subject revision where available. Completion time is separate. A later poll
  cannot relabel an old image as a current frame or refresh its pose from live state.
- RGB, labels and depth either belong to the same declared observation or report
  incomplete/mismatched evidence. Cover delayed completion, missing results, viewport
  resize/replacement and destroyed subjects. Missing/hidden data stays unknown.
- Bound pending requests, staging bytes and queued output across acquisition,
  decoding and export. Report rejection, failure, cancellation, gaps and peak use.
  Exercise backpressure and timeout paths; moving work to an unbounded queue is not
  a performance improvement.
- The steady-state asynchronous path does not serialize capture with GPU-idle waits,
  blocking readiness polling or per-frame rendering flushes. Any compatibility or
  teardown wait is explicit, measured and has a documented lifetime/failure policy.
- Repeated start/stop, cancellation and world/viewport destruction with requests in
  flight preserve resource lifetime, exactly-once completion or cancellation, label
  restoration and safe extension cleanup. Completion after teardown cannot access
  stale world objects. Exercise independent sessions and shared-resource ownership.
- Preserve supported legacy API/record meanings through explicit compatibility.
  Declare schema changes and required reader updates. Keep decoder/source identities
  accurate; the two calibration-sensitive Python files have deliberate byte-preserving
  Git attributes. Do not normalize them incidentally or reuse stale calibration.
- Compare capture disabled, synchronous baseline and asynchronous capture under the
  same warmed renderer, scene, resolution, cadence and output policy. Retain raw
  samples and report frame-time p50/p95/p99, acquisition/completion latency, queue
  occupancy, peak bytes and losses. Separate readback cost from rendering/encoding.
  Repeated comparable measurements must support any performance claim.
- Pass the neutral controls and applicable Python regression/isolation checks. The
  delivery includes exact commits, commands, environment, evidence hashes, capability
  limits and consumer changes needed. Katana verification follows through its owner.

## Rendered controls and supported baseline

Supported surface paths: UE 5.6, D3D11 D32F/S8, one perspective view, no AA
and declared full-resolution diagnostic rendering. Physical staging layout is
decoded explicitly; the existing five-byte logical format size is not its eight-byte
staging stride in the synchronous path. The asynchronous path packs fixed GPU
buffers and has its own decoder identity. Preserve rejection of unsupported formats/views. Broader renderer
support is a separate validated capability.

The existing fixture uses engine cubes and checks a front plane at 350 cm, separated,
touching, intersecting, fully label-occluded and ordinary scene-occluded subjects.
Recorded pixel-centre gaps are 66, 1, 1 and indeterminate for the two occluded cases
under that exact fixture view. These values are fixture expectations, not universal
contact thresholds. Touching and intersecting silhouettes are deliberately not
distinguished by adjacency alone.

An independent RGB/label outline check uses an explicit black background and unlit
material. Its recorded combined silhouette IoU is 1.0 against a fixed 0.99 floor
for the first four controls. It does not prove separate subject identity where
same-material subjects touch. The earlier lit fixture lacked sufficient contrast;
do not address that by relaxing alignment thresholds.

The earlier measured depth/label readback cost was 12.38-13.47 ms at 640x480. This
is a historical diagnostic measurement excluding total render/RGB/export cost.
Re-establish a controlled local baseline; it is not the target or a neutral-host
performance result.

Katana source references for reading/reuse, relative to that repository root:

- `Source/KatanaCombatTest/Private/ViewportSurfaceCaptureTests.cpp`: geometry and
  label controls; separate the neutral fixture from its project test harness.
- `docs/audits/VIEWPORT_SURFACE_ANALYSIS_2026-09-11.md` and
  `docs/guides/VIEWPORT_SURFACE_ANALYSIS.md`: evidence semantics and control details.
- `docs/audits/ANIMATION_ANALYSIS_REPOSITORY_2026-09-11.md`: extraction verification.
- `Saved/Logs/AnalysisRepository-20260911-113408`: original extraction evidence.
  Its `capture-evidence.zip` SHA-256 is
  `ea0ef372c1b12761534e6b9a673448694a72a28cdc2204c737c0eb2e145297c8`.
  Image-dependent replay requires restoring archive entries; loose capture folders
  have had their 176 generated PNGs removed after verified retention.

Source fixtures and tracked descriptions establish reproducible controls; historical
Saved artifacts are optional evidence, not dependencies of the new test launcher.

## Parallel ownership and integration

The shared-suite developer owns changes and commits in AnimationAnalysis. Katana's
developer owns its gameplay, adapters, assets, test harness and dependency updates.
Read Katana source as needed; proposed consumer edits travel with the shared delivery
for coordinated application. Existing Katana fixtures remain until their owner
updates their consumers and documentation. Avoid simultaneous edits to those files.

Katana keeps its verified pin while shared development proceeds. Deliver a verified
commit and compatibility notes; the Katana owner updates the pin, runs dependency
setup, rebuilds and executes the affected project checks. A standalone pass establishes
readiness for consumer integration, not a Katana runtime pass.

Coordinate GPU profiling runs with other editor/rendered work on the workstation.
Use unique bounded evidence directories. Archive required replay artifacts and verify
entry hashes before removing generated images; cleanup targets must come from the
run's own explicit inventory. Keep builds/caches/captures out of Git.

The full suite migration remains in [MIGRATION.md](MIGRATION.md). Paired evaluation,
preview calculations, stream/report orchestration and general retention remain open.
These families migrate in scoped deliveries with their consumers; asynchronous
readback is the first delivery, not a replacement for that inventory.
