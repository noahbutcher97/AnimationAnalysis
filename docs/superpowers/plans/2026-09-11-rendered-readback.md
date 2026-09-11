# Rendered host and bounded readback implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement and verify each slice. Independent tooling work may use superpowers:dispatching-parallel-agents.

**Goal:** Deliver the first scope in the approved development handoff, with reproducible neutral renderer evidence before and after asynchronous acquisition.

**Architecture:** Keep fixtures in the existing host and preserve the synchronous compatibility API. Introduce an explicit, bounded producer that snapshots acquisition metadata, retains GPU resources until safe retirement, and reports completion separately. Session RGB and surface observations share readback mechanisms; consumer discovery and assertions remain outside the plugin.

**Tech stack:** UE 5.6 C++, D3D11 D32F/S8, Python unittest and standard-library launcher/retention tooling.

**Spec:** [Development handoff](../../DEVELOPMENT_HANDOFF.md), especially First delivery and Acceptance criteria.

## Constraints

- All edits and commits stay in AnimationAnalysis; no consumer assets, adapters, pins or publication.
- Preserve schema 2 and SURFACE1 compatibility or explicitly version any new meanings.
- One perspective view, no AA, full resolution; unsupported data remains unknown.
- Bounds cover acquisition, completed results and PNG output; cancellation never frees in-flight GPU resources prematurely.
- Archive replay evidence and verify each hash before deleting only inventoried generated images.

## Slice 1: rendered host and synchronous baseline

Files: `Python/UnrealHost/Source/AnimationCaptureHost/Private/AnimationCaptureHostFixture.*`, `AnimationCaptureHostSurfaceTests.cpp`, host module/build configuration, `Python/verify_unreal_host.py`, host tooling tests, `docs/HOST_WORKFLOW.md`.

- [x] Add exact-result/deadline/retention tooling tests and observe the missing workflow fail.
- [x] Stage one neutral host for temporary automation or retained interactive use. Retain source identity, commands and unsuccessful results.
- [x] Share camera/cube controls between automation and `AnimationAnalysis.Host.Inspect`; restore labels/view/CVars on stop or teardown.
- [x] Verify five synchronous geometry cases, 350 cm front depth, fixed RGB silhouette IoU >= 0.99, occlusion and viewport/frame mismatch.
- [x] Warm the identical 640x480 fixture and retain repeated disabled/synchronous frame and readback timing samples, excluding file export from readback timing.
- [x] Build/run isolated host and package/tooling checks, record environment/hashes and commit verified host slice.

## Slice 2: bounded asynchronous producer and compatibility

Files: new `Source/AnimationCapture/Public/AnimationCapture/AnimationCaptureReadback.h`, private implementation, existing surface/session APIs, host async tests, explicit Python adapter/reader tests if required.

- [x] Add controls that fail for missing async admission, delayed completion identity, queue/bytes saturation, cancellation and timeouts.
- [x] Implement admission with immutable caller context and renderer-acquired frame/projection, nonblocking fence polling and bounded result ownership.
- [x] Share staging/decode with surface RGB/depth and session RGB, retaining explicit synchronous compatibility settings.
- [x] Test missing draw/planes, resize/replacement, destroyed subject/world, repeated lifetime, independent owners and label restoration.
- [x] Record queue peaks, rejects/cancellations/failures and teardown waits; verify no steady-state GPU-idle wait or flush.
- [x] Build/run neutral rendered and portable isolation controls and commit the verified producer slice.

## Slice 3: comparison and delivery

Files: host performance controls, evidence summarizer, README/migration/handoff and delivery report.

- [x] Repeat disabled/synchronous/asynchronous measurements under the same warmed fixture, cadence and output policy; retain raw samples.
- [x] Report frame p50/p95/p99, submission/decode and completion latency, occupancy/bytes/losses, rendering versus encoding, and measured explicit teardown waits.
- [x] Verify archive inventory hashes and replay before cleaning inventoried PNGs.
- [x] Record exact candidate commits, commands, environment, evidence hashes, compatibility and limits. Supply Katana owner the pin/rebuild/scenario checklist without editing Katana.
- [x] Review diff, rerun checks justified by final changes, commit documentation and leave clean source state.
