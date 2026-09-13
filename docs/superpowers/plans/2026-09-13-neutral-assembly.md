# Neutral Assembly Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for
> task implementation, scoped reviews and one final whole-branch review.

**Goal:** Qualify a reusable native capture-to-replay-to-sampled-analysis workflow.

**Architecture:** A bounded rigid reference batch establishes shared acquisition;
the neutral host exports paired cube observations. Installed-package examples
consume explicit manifests/criteria and measure ordinary and difficult workloads.

**Tech Stack:** UE 5.6.1/D3D11, C++, Python >=3.11 standard library, unittest.

**Spec:** `docs/superpowers/specs/2026-09-13-neutral-assembly-design.md`

## Global Constraints

- All changes stay in AnimationAnalysis; Katana owns consumer qualification and pins.
- No deformation extension, containment, engine patch, push or merge to main is included.
- Python >=3.11 with no new package dependency. UE 5.6.1/D3D11 native qualification.
- Reuse mesh replay schema 1 and Python 0.4.0 analysis APIs.
- Preserve acquisition/completion identity; unsupported evidence stays insufficient.
- Retain replay archives, failed-run evidence and existing worktrees; generated files stay ignored.

### Task 1: Synchronous rigid batches and native assembly

**Files:** Modify `Source/AnimationCapture/Public/AnimationCapture/AnimationCaptureMeshReference.h`,
`Source/AnimationCapture/Private/AnimationCaptureMeshReference.cpp`,
`Source/AnimationCapture/Private/AnimationCaptureMeshReplay.cpp`, `Python/host_tools.py`;
create `Python/UnrealHost/Source/AnimationCaptureHost/Private/AnimationCaptureHostAssemblyTests.cpp`.

**Interface:**
```cpp
static bool CaptureRigidBatch(
    TConstArrayView<FAnimationCaptureMeshReference*> Samplers,
    const FString& RequestId, int32 MaxComponents,
    TArray<TSharedPtr<const FAnimationMeshSnapshot, ESPMode::ThreadSafe>>& OutSnapshots,
    FString& Error);
```
The private `Prepare` path accepts an optional native acquisition stamp; all old
callers retain their original behavior. Only registered ordinary unparented static
components in one non-ticking world, without physics, qualify for this batch API.
Validate count (1..MaxComponents, MaxComponents 1..64) and duplicates before output
allocation. Boundaries and transactional release follow the spec.

- [x] Add host controls first, initially exercising individual captures: their
  acquired stamps differ, demonstrating why paired time cannot be assumed.
- [x] Add batch assertions with independent literal identities and known geometry:
  ```cpp
  TestEqual(TEXT("Shared acquisition"), Pair[0]->Data().AcquiredSeconds, Pair[1]->Data().AcquiredSeconds);
  TestTrue(TEXT("Completion follows acquisition"), Pair[1]->Data().CompletedSeconds >= Pair[1]->Data().AcquiredSeconds);
  TestEqual(TEXT("Failed batch leaves no snapshots"), Pair.Num(), 0);
  TestEqual(TEXT("Failed batch releases admission"), Budget->LiveBytes(), int64(0));
  ```
- [x] Implement batch preparation with native common stamp, per-input completion
  and existing budget ownership. Never rewrite exported observations.
- [x] Preserve double precision when exporting native replay numbers; assert exact
  acquisition/completion and non-six-decimal transform round trips. The observed
  `LexToString(double)` six-decimal output cannot satisfy explicit pair identities.
- [x] Implement latent five-sample cube fixture and the exact `assembly.json` contract
  from the spec. Example role IDs: component/subject `fixed-part` and `moving-part`,
  stream `assembly`, generation 1, regions `fixed-surface` and `moving-surface`.
- [x] Add `AnimationAnalysis.Capture.Mesh.RigidAssembly` to default rendered tests.
  Build and run the exact new test through `Python/verify_unreal_host.py`, retaining
  failure and successful evidence directories. Run affected tooling checks.
- [x] Commit the native slice and submit its source diff plus runtime evidence for review.

### Task 2: Installed replay-to-report example

**Files:** Create `Python/examples/neutral_assembly.py`,
`Python/examples/neutral_assembly_criteria.json`,
`Python/tooling_tests/test_neutral_assembly.py`.

**Interfaces:** `evaluate_run(observations, criteria) -> dict`; `main(argv=None) -> int`.
CLI requires `--observations`, `--criteria`, `--output`. Criteria JSON is explicit:
format `neutral_assembly_criteria`, schema_version 1, units `centimetres`,
coordinate_system `unreal-left-handed-z-up`, vector_convention `row`,
required_features `[rigid,pose_ordering]`, known_features and allowed_exclusions
cover the native five omissions, allowed_producers `[unreal-rigid-reference-v1]`.
Fields also include `record_limits`, `analysis_limits`, `max_samples`,
`max_gap_seconds`, `tolerance`, `expected_samples`. Native omissions are morph,
cloth, mesh_deformer, material_displacement and raster_visibility.

Record limits: 64 vertices, 128 indices, 4 sections, 8192 payload bytes,
65536 metadata bytes, 73728 record bytes. Analysis limits: 12 triangles per side,
256 pair tests, 1024 node visits, 256 coordinate bits. Max samples 8; gap 1.0 seconds;
tolerance 0.0. Each expected sample has `sample_id`, `distance`, `intersection`:
`step-00`..`step-04`, distances `[40,10,0,0,40]`, flags `[false,false,true,true,false]`.

- [x] Write tests using explicit synthetic cube records and literal expected distances;
  run and retain the missing-feature failure before implementation.
  ```python
  report = evaluate_run(run_directory, criteria)
  self.assertEqual(report['status'], 'verified')
  self.assertEqual([r['minimum_distance'] for r in report['measurements']], [40,10,0,0,40])
  self.assertEqual(report['interval']['between_samples'], 'not_evaluated')
  ```
- [x] Implement bounded strict inputs and explicit replay reads, then public region,
  requirement, pair measurement and interval calls. Role declarations and sample
  frame/revisions bind requirements. Preserve completion separately and hash inputs.
- [x] Verify missing middle bundle cannot yield overall success even when the
  surviving interval gaps meet criteria. Verify wrong configuration/topology/pose,
  malformed criteria, nonfinite/duplicate fields and exclusive output handling.
- [x] Add labelled in-memory negative controls from the spec; source files remain
  unchanged. Controls may not turn primary observed failures into success.
- [x] Run the example on fresh native output through an installed wheel; commit
  after focused tests and independent review of the manifest/report boundary.

### Task 3: Bounded difficult-workload measurements

**Files:** Create `Python/examples/benchmark_mesh_analysis.py`,
`Python/tooling_tests/test_mesh_analysis_benchmark.py`.

**Interfaces:** `run_benchmarks(output) -> dict`; `main(argv=None) -> int` requires
`--output` pointing to a new directory. Default recipe, shapes, exact squared
expectations, counts/repetitions and limits are fixed in the spec. A testable case
builder may accept an explicit smaller count, but production defaults remain fixed.

- [x] Write tests for literal exact squared distances, insufficient results without
  numeric verdicts at an exhausted limit, deterministic input identity and no overwrite.
  ```python
  self.assertEqual(result.minimum_squared_distance, Fraction(2))
  self.assertIsNone(exhausted.minimum_distance)
  ```
- [x] Implement public-API fixtures and deterministic rotated repeats. Retain input
  hashes/recipes, source hash, raw times/counters/status, environment and settings.
- [x] Run fixed workloads using the final installed wheel in a separate process.
  Report completed and exhausted cases distinctly; do not modify the search kernel.
- [x] Commit verified tooling and review its independent expectations and timing scope.

### Task 4: Whole-delivery qualification and documentation

**Files:** Create `docs/NEUTRAL_ASSEMBLY.md`, `docs/NEUTRAL_ASSEMBLY_DELIVERY.md`;
update README, `docs/DEVELOPMENT_HANDOFF.md`, `docs/MIGRATION.md`,
`docs/NATIVE_MESH_REFERENCE.md`, `docs/HOST_WORKFLOW.md` and plan completion ledger.

- [x] Run full core/tooling tests and `Python/verify_distribution.py` with retained output.
- [x] Run a fresh independent native host build and rendered suite, including
  `AnimationAnalysis.Capture.Mesh.RigidAssembly` and the explicit GPU combined control.
- [x] Execute the example and benchmark with a fresh installed wheel; archive native
  replay before restoration and rerun the example against the restored bundles.
- [x] Complete scoped reviews and one whole-branch review; fix substantiated defects.
- [x] Record source identities, exact tests, performance, gaps, budgets and consumer
  limits. Archive/hash needed artifacts without changing old archives.
- [x] Commit verified docs and report the completed feature branch; no publication.

## Execution ledger

Baseline `2fb0dc9`: clean root; new ignored worktree `Saved/AssemblyWorkflowWorktree`,
branch `feature/neutral-assembly-workflow`; all 105 Python tests passed.

| Boundary | Preflight finding |
|---|---|
| Task 1 internal | Synchronous batching must precede common acquisition metadata; rigid scope avoids unqualified live pose extension. |
| Task 2 internal | Missing reads affect overall status independently of interval coverage; known fixture expectations are supplied separately. |
| Task 3 internal | Exhausted searches retain counters/timing but no partial minimum; hard cases are not real-game performance claims. |
| Task 4 internal | Fresh native build is required for public native changes; Katana remains owner of its rebuild. |
| Tasks 1/2 | Exact assembly manifest and sample/role field names are fixed in the spec. |
| Tasks 2/3 | Both use existing public analysis APIs; no shared mutable fixture helper or production math change. |
| Tasks 1/4 | Default rendered test list gains the assembly test; final suite must enumerate it exactly. |
| Tasks 2/4, 3/4 | Examples run against installed package; tracked examples stay outside the portable wheel. |

Ruling: the approved end-to-end task includes the minimal rigid batch prerequisite;
without it, independent native observations cannot meet the current pairing contract.
This adds an opt-in native API and requires a rebuild, while preserving old calls.
Ruling: preserve ignored development/review artifacts and replay worktrees as evidence;
archive them before any future cleanup. No cleanup or merge is needed to finish this slice.

Completed 2026-09-13. All three scoped reviews and the final whole-branch review
at6505fec approved without open findings. Full verification, local timings,
compatibility and archive identity are recorded in docs/NEUTRAL_ASSEMBLY_DELIVERY.md.
The local feature branch and evidence worktree remain preserved; no publication.
