# Finalized Pose Acquisition Implementation Plan

> Use superpowers:subagent-driven-development for implementation and independent
> scoped/final reviews. Preserve ignored evidence and worktrees at completion.

Baseline: verified neutral assembly delivery `a49850f`.
Spec: `docs/superpowers/specs/2026-09-13-finalized-pose-acquisition-design.md`.

## Global constraints

- Keep changes in AnimationAnalysis; Katana owns consumer edits, assets and pins.
- Preserve acquisition/completion identity. Never retime historical observations.
- No deformation, containment, GPU group API, engine patch, push or merge is included.
- Keep default single-node behavior; broader animation support is explicit opt-in.
- Use UE5.6.1/D3D11 and Python0.4.0 public APIs; retain replay schema1.
- Retain failed runs/replay archives and verify evidence before image cleanup.

### Task 1: Native finalized animation policy and synchronous component batches

**Files:** Modify mesh reference public/private source, mesh GPU public/private source,
`Python/UnrealHost/Source/AnimationCaptureHost/AnimationCaptureHost.Build.cs`,
`Python/host_tools.py`; add a focused neutral host finalized-animation fixture/test
file and any narrowly needed host-only helper. Do not change consumer, Python core,
existing examples or documentation owned by the controller.

**Requirements:** Read the spec public contract and native qualification sections;
they define exact API, defaults, constraints and controls. All details there apply.
`CaptureBatch` mirrors the existing bounded `CaptureRigidBatch` signature. New enum
values are `SingleNode` and `FinalizedAnimation`; enrollment field is `PosePolicy`.
Keep observer-local revisions, existing producers and default config hashes.

- [ ] Write real neutral AnimBlueprint/slot/montage controls first and retain the
  expected old-policy/API failure before implementation. No consumer assets.
- [ ] Implement opt-in finalized pose witnesses and the mixed CPU/rigid batch API,
  preserving original bounded rigid-only and GPU single-component paths.
- [ ] Export a five-frame coherent native pair manifest and schema1 bundles with
  independent numeric geometry expectations. Tell the controller the exact manifest
  format before Task2; do not invent a replacement replay format.
- [ ] Prove stale/pending/replaced/unsupported pose and attachment, batch admission,
  actual separate completion and delayed GPU collection controls.
- [ ] Run focused fresh host build/rendered tests, retain replay and failure evidence,
  commit the native slice, and return a report for scoped review.

### Task 2: Installed replay qualification controls

**Files:** Add `Python/examples/finalized_pose_pairs.py` and focused tooling tests.
The script consumes Task1's explicit neutral manifest and schema1 bundles through
installed Python0.4.0 APIs. No source-path injection, consumer discovery or retiming.

- [ ] Test the manifest/identity boundary and successful two-component pair results
  with independent fixture expectations. Keep gaps and actual completions distinct.
- [ ] Exercise detached wrong-pose, one-side replacement, excluded coverage,
  exhausted-work and incomplete-interval controls without modifying replay.
- [ ] Run on original and archive-restored fresh native evidence; verify historical
  consumer records still reject. Commit tooling and complete scoped review.

### Task 3: Independent qualification, compatibility report and candidate

**Files:** Update reference/GPU/host guides, README and handoff as needed; add
`docs/FINALIZED_POSE_ACQUISITION_DELIVERY.md` with source-backed consumer response.

- [ ] Build/run the full isolated native D3D11 set including new controls; verify
  exact test inventory, replay integrity and unchanged earlier supported behavior.
- [ ] Run distribution/tooling checks appropriate to changes; compare installed
  package/source identity and report fresh versus reused evidence accurately.
- [ ] Record local performance, compatibility, retained Katana reproduction,
  addressed blockers and remaining consumer/GPU group/material limitations.
- [ ] Archive/hash replay and logs, preserving old archives; complete final review
  and commit a verified local candidate, with no publication or consumer edits.
