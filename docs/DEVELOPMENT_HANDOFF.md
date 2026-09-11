# Shared suite development handoff

Recorded 2026-09-11. Read [repository instructions](../AGENTS.md), the
[API and verification guide](../README.md), then [whole-suite migration](MIGRATION.md).
This document defines the next bounded delivery and the consumer integration boundary.

## Verified starting point

Implementation baseline: `ba13149d3318f80d3098958cd1d2cd52bba3e5d1`.
KatanaCombat consumes that exact revision through its dependency lock. Later
documentation commits do not change the consumer pin or its installed code.

| Surface | Evidence at extraction | Limit |
|---|---|---|
| Installed Python package | 24 core tests pass without Pillow; all 25 pass with the image extra; four CLI entry points exercised | Offline services only |
| Independent Unreal host | Editor build and two native lifecycle/extension controls pass | Runs with `NullRHI` and image cadence zero; does not exercise GPU acquisition |
| Katana consumer | Editor build, 77 Python tests and six dependency tests pass | Focused integration verification, not a full combat baseline |
| Rendered Katana controls | Four D3D11 PIE/surface tests plus a completed finisher capture and offline evaluation pass | Existing synchronous capture; no continuous skeletal surface or penetration claim |

These are recorded results, not tests rerun for this documentation commit.
Reproduction commands are in the README. The existing native verifier has no
rendered-mode switch; adding a supported rendered host run is part of the next task.

## First delivery: neutral rendered controls, then asynchronous readback

1. Establish a rendered D3D11 fixture in the neutral host using explicitly supplied
   primitives and engine content. Reuse the known-geometry controls described below.
   Give it a reproducible launcher, deadlines, exact test-result checks and retained
   evidence. A headless skip must remain distinguishable from a rendered pass.
2. Record the current synchronous baseline in that host before changing acquisition.
   Keep existing lifecycle tests. Verify RGB/label/depth alignment independently of
   gameplay telemetry and preserve the control geometry, view policy and tolerances.
3. Implement bounded asynchronous acquisition for RGB and scene/custom depth through
   reusable producer APIs. The existing surface diagnostic and session RGB recorder
   should share appropriate mechanisms. Keep subject discovery, skeleton choices,
   scenarios and quality criteria in consumers. Deliver this scope before moving
   skeletal sampling or geometric penetration work.

Current implementation entry points:

- `Source/AnimationCapture/Private/ViewportSurfaceCapture.cpp`: `ReadDepth` creates a
  GPU readback, blocks until GPU idle and immediately locks it. `Collect` also takes
  a synchronous RGB screenshot. Destruction currently flushes rendering commands.
- `Source/AnimationCapture/Private/AnimationCaptureSession.cpp`: `PostDraw` acquires
  RGB synchronously; PNG encoding uses the bounded `AnimationCaptureImageWriter`.
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

Current supported surface path: UE 5.6, D3D11 D32F/S8, one perspective view, no AA
and declared full-resolution diagnostic rendering. Physical staging layout is
decoded explicitly; the existing five-byte logical format size is not its eight-byte
staging stride. Preserve rejection of unsupported formats/views. Broader renderer
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
