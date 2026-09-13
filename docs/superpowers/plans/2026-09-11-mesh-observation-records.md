# Mesh Observation Records Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. Steps use checkbox syntax for tracking. No delegation is required.

**Goal:** Deliver a portable, versioned mesh-observation contract with explicit capability eligibility and bounded replay validation before connecting Unreal sampling.

**Architecture:** Reuse `ClockStamp`, `PoseKey` and existing artifact/integrity helpers.
Keep immutable geometry/topology, acquisition identity and completion status separate.
Unreal emits explicit records later; the Python reader neither discovers components
nor infers missing deformation coverage. This plan delivers the first independently
testable production slice, not the complete native sampler.

**Tech Stack:** Python 3.11+, standard library, existing unittest suite and isolated-wheel verifier. No geometry-library, Unreal or consumer dependency.

**Spec:** [Surface capabilities and fidelity](../../SURFACE_CAPABILITIES.md), informed
by [animated readiness evidence](../../research/2026-09-11-animated-surface-readiness.md).

**Status:** Tasks 1–3 completed; [delivery evidence](../../MESH_RECORD_DELIVERY.md).
The following native slices remain open. Review added retained failure-request
identity, whole-record section coverage, pre-decode combined limits, nonregular-file
rejection and a complete external replay manifest.

## Global constraints

- Keep geometry, rendered visibility and interpretation distinct. No universal final-surface claim.
- CPU/GPU is a backend choice; neither determines achieved deformation coverage.
- Preserve original acquisition `PoseKey`, clocks and component/configuration generations.
- Unknown capability fields must not be ignored when deciding eligibility.
- Admit within explicit limits before retaining or decoding variable-size payloads.
- Keep existing motion/RGB/depth schemas and defaults compatible.
- Consumer assets, adapters, dependency pins and integration remain Katana-owned.
- Commit verified slices locally. Publishing requires authorization.

## Task 1: Immutable mesh records and capability eligibility

**Files:**

- Create `Python/src/animation_analysis/mesh_records.py`.
- Create `Python/tests/test_mesh_records.py`.
- Reuse `Python/src/animation_analysis/contracts.py`; avoid changing its existing meanings.
- Create `docs/MESH_OBSERVATIONS.md` alongside the implementation.

**Interfaces to implement:**

- `MeshTopology`: immutable vertex count, triangle indices, explicit section ranges
  and material identifiers, LOD, asset/configuration generation and content identity.
- `MeshObservation`: component identity/generation, topology identity, owned positions,
  coordinate units/convention, transform, acquisition `PoseKey`/`ClockStamp`, producer
  identity and per-feature coverage.
- `MeshCompletion`: request identity, terminal status/reason and completion clock;
  optional observation retains its original acquisition fields.
- `MeshRequest`: component/configuration/generation and acquisition witness even
  when completion has no geometry; both pose and clock are absent for early failures.
- `FeatureCoverage`: feature identifier, state (`observed`, `inactive`, `unsupported`,
  `unknown`, `excluded`), producer/evidence identity and reason. Inactive requires a
  witness; absence from a record means unknown.
- `MeshRequirement`: explicit required features, allowed reference/exclusion policy,
  component/topology/pose expectations and supported feature vocabulary.
- `assess_mesh_coverage(observation, requirement) -> MeshEligibility`: eligible or
  insufficient, with structured reasons. This never evaluates contact/artistic quality.

Finalize field spellings in the schema document and keep reader/writer tests using
that exact schema. Deep-copy incoming sequences to immutable storage. Specify the
canonical topology hash input, including section mapping, so material/topology changes
cannot preserve an incompatible identity. Keep identity distinct from buffer precision.

- [x] Write failing tests for a rigid prop and bone reference with explicit units,
  section mapping, clocks and different component identities.
- [x] Add meaningful negative cases: mutable inputs changed after construction;
  nonfinite coordinates; invalid/boolean indices; incomplete or overlapping section
  coverage; wrong topology hash; a different component generation; same engine frame
  with a different pose revision; attempted latency subtraction across unrelated
  clock domains. Keeping distinct acquisition/completion clocks is valid; subtracting
  them without a supplied relationship is not.
- [x] Add eligibility tests showing that an explicitly requested bone reference may
  exclude cloth, while a requirement including cloth is insufficient. Missing,
  unsupported, unknown and explicitly excluded coverage must remain distinguishable.
  An unfamiliar feature/state cannot silently disappear or produce an eligible result.
- [x] Run `python -m unittest discover -s Python/tests -p test_mesh_records.py -v`
  in the development environment with this package installed; confirm the new tests
  fail because behavior is missing, then implement the smallest complete contract.
- [x] Rerun targeted tests, review schema/examples against them, and commit the
  contract with its verification evidence. These tests qualify record handling,
  **not** Unreal's ability to detect active features.

## Task 2: Bounded mesh replay format and reader

**Files:**

- Create `Python/src/animation_analysis/mesh_replay.py`.
- Create `Python/tests/test_mesh_replay.py`.
- Reuse `Python/src/animation_analysis/integrity.py` and `artifacts.py`.
- Extend `docs/MESH_OBSERVATIONS.md` with the exact binary/metadata layout and limits.

**Interfaces:**

- Reuse `MeshRecordLimits`: required limits for metadata bytes, vertex/index/section
  counts, payload bytes and combined encoded record bytes. These are not process RSS
  limits. Check source lengths before decoding.
- `read_mesh_observation(root, relative_record, *, limits) -> MeshCompletion`.
- `write_mesh_observation(root, relative_record, completion, *, limits) -> dict`:
  returns an explicit file/hash manifest; does not overwrite a completed bundle.

Use an independent `mesh_observation` schema version 1. Specify fixed little-endian
position/index encodings, element counts, exact lengths, units and SHA-256 identities.
Keep topology reusable by content identity without introducing an unbounded cache.
Serialize acquisition and completion independently. Reject unknown schema/encoding
versions; do not attempt legacy visual-evidence decoding under this format. Preserve
unsupported-feature evidence without upgrading it to eligible geometry.

- [x] Write a neutral two-triangle/rigid-prop round-trip test with delayed completion,
  explicit topology and two distinct poses. Assert the restored values/identities.
- [x] Add corrupted and truncated buffers, mismatched hashes/counts, oversized
  metadata/payloads, directory escape/symlink escape, duplicate keys/sections and
  unsupported-version cases. Verify failure before large allocation where applicable.
- [x] Define the atomic completion marker and test interrupted output: incomplete
  bundles cannot be read as completed evidence. Reuse existing publication/path
  utilities where their documented semantics fit.
- [x] Run the targeted replay tests, implement bounded decoding/writing, rerun and
  commit. Retain a small neutral replay fixture generated by the tests under their
  temporary directory; no generated mesh captures enter Git.

## Task 3: Public package and handoff verification

**Files:**

- Modify `Python/src/animation_analysis/__init__.py` for deliberate public exports.
- Update `Python/README.md`, `README.md`, `docs/MIGRATION.md`,
  `docs/DEVELOPMENT_HANDOFF.md` and `docs/MESH_OBSERVATIONS.md`.
- Extend `Python/verify_distribution.py` only if its existing staged tests/import
  isolation do not exercise the new public module; avoid duplicate verification.

- [x] Document runnable explicit-input examples, bounds, schema version, insufficient
  evidence behavior and immutable acquisition identity. State that native capture is
  still pending; do not announce morph or final-rendered-surface support.
- [x] Run all Python tests and
  `python Python/verify_distribution.py --output Saved/MeshRecords-<unique-run>`.
  Require both core and optional-image installations to pass and existing CLI/import
  isolation to remain intact. Record exact counts and archive applicable evidence.
- [x] Confirm no Unreal module/header or consumer wiring changed. Native/consumer
  tests are not claimed for this Python-only slice.
- [x] Commit the verified delivery and update the handoff with exact commit, tests,
  compatibility and the native implementation boundary. Do not update Katana's pin.

## Following native slices

These follow this record delivery and need their own implementation details before
coding; their acceptance requirements already live in the capability design:

1. **Rigid and CPU reference producer:** add mesh observation types/serialization
   and explicit component enrollment to `Source/AnimationCapture`; extend the existing
   host with the real animation/rigid fixture. Qualify multiple sections, finalization,
   invalidation and unsupported states without driving live components.
2. **Bounded GPU producer:** retain safe pre-query Skin Cache ordering, immutable
   identities, topology/source ownership and exactly-once terminal results. Share
   admission/accounting with existing `AnimationCaptureReadback` and image export;
   do not create an independent quota that hides combined memory use.
3. **Combined qualification:** promote independent raster replay; add cancellation,
   timeout, component/world destruction, replacement/streaming rejection and saturation
   controls; freeze benchmark workloads before repeated measurement. Run distribution
   and rendered native isolation. Deliver exact compatibility notes for Katana's owner
   to rebuild and run finisher/counter integration.

The small morph experiment remains an early extension check, not a prerequisite for
record delivery or an unsupported morph claim. Full cloth/deformer/material fidelity,
broader renderer/RHI support and geometric contact analysis remain deferred. No further
broad research pass is a prerequisite for Task 1.
