# Neutral assembly delivery

Qualification date: 2026-09-13. This slice connects native rigid acquisition,
schema-1 replay, explicit regions, surface measurements and sampled interval
reporting. The runnable commands and criteria are in [the example guide](NEUTRAL_ASSEMBLY.md).

## Change and compatibility

The new opt-in `FAnimationCaptureMeshReference::CaptureRigidBatch` establishes a
shared acquisition time during a synchronous game-thread operation. Independent
calls retain their original independent timestamps. Each batch snapshot preserves
its observer revision, actual completion time and budget ownership. A failed batch
returns no partial snapshots and releases its partial reservations.

The batch currently qualifies different registered ordinary static components,
unparented in one non-ticking world, without physics simulation. Its caller supplies
a component-count limit of 1 through 64. It does not extend skeletal or attached
component acquisition. Existing single-capture and GPU paths retain their behavior.
Native consumers require a rebuild to use the additional API.

Native replay numbers now retain double round-trip precision. The previous writer's
six-decimal output lost timestamp and transform precision; fresh exports consequently
have different record byte hashes. Existing schema-1 records remain readable with
their originally recorded precision. No old recording is rewritten.

Python remains version 0.4.0 and replay remains schema 1. The scripts under
`Python/examples` use the installed public package APIs and add no dependencies or
portable package changes. Inputs supply explicit identities, criteria and limits;
failure controls remain labelled detached data and never change native recordings.

## Verification

Native implementation: `4c930ac733880187ead63b8be552b043327d7be5`, based on merged
`2fb0dc980dbdba1348b02e3a93b4512606aa5d16`. The independent native run is
`Saved/AssemblyNativeFull-20260913-01` in the delivery worktree.
Commit `3ec7044` subsequently strengthens only fixture assertions: all source and
transformed axis minima/maxima and the 24-vertex count are checked explicitly.
Its focused `RigidAssembly` test passed in a fresh host at
`Saved/AssemblyTask1-ReviewFix-20260913-08`; production code is unchanged.

| Check | Result |
|---|---|
| Fresh Engine/plugin-only host build | UE 5.6.1, changelist 44394996, Win64 Development Editor; succeeded |
| Exact rendered automation set | 25/25 passed with D3D11 observed; no missing, extra, duplicate or skipped tests |
| Replay precision regression | Exact acquisition/completion and `0.123456789` transform round trips passed; canonical topology passed |
| Installed replay compatibility | Fresh CPU fine/coarse/rigid and GPU fine records passed; combined GPU/raster comparison passed |
| Assembly boundary controls | Passed; peak retained reservation 10,510,240 bytes |
| Installed Python/distribution | 104 passed plus one optional-image skip without Pillow; 105/105 with images; isolation passed |

The fresh native sample gaps were 0.121778, 0.122474, 0.120568 and 0.120745 seconds,
within the unchanged 0.1–1.0 second fixture bounds. The build took 51.375 seconds;
the editor automation process took 58.625 seconds, including startup and teardown.
These are run durations, not isolated capture-performance measurements.

The installed combined mesh/raster check retained silhouette IoU 0.9999534 and
maximum shared-interior depth error 0.0021552 cm across 20,904 pixels. Its deliberate
20 cm wrong-pose control reduced IoU to 0.5672254. These are existing fixture
compatibility controls, not broader rendered-surface coverage or physical contact.
Exact commands and outputs are retained as `Saved/AssemblyReplayCompatibility.commands.json`
and `Saved/AssemblyReplayCPU.json`, `AssemblyReplayGPU.json`, `AssemblyReplayGPURaster.json`.

The installed wheel's 28 Python modules byte-match the checkout; portable source,
tests and package metadata are unchanged from the baseline. The example (`64d4c56`)
passes 10 focused example tests. The final suite including the benchmark passes all
42 tooling tests; the benchmark's six focused tests also pass. Its installed original and
restored native reports match exactly after excluding elapsed timings. Both retain
all expected distances/intersections, a complete interval, no read errors and seven
negative controls that remain insufficient for their intended reasons.

The retained pre-fix replay from run05 is insufficient under the same strict criteria:
all five measurements with mismatched acquisition clocks have no numeric minimum.
No manifest or recording was rounded to force a match. Commands, complete reports
and comparison receipt are in `Saved/AssemblyExampleQualification-20260913-02`.
The named demo requires its fixed semantic profile; changing its sample count or
acceptance values is rejected. Equivalent formatting and feature-set order remain
accepted. Consumer-specific criteria remain available through the public APIs.

The two 100 cm cubes visit five known configurations. Expected surface distances
are 40, 10, 0, 0 and 40 cm. Touching and crossing samples have surface intersection.
This is a statement about the supplied surfaces, not physical contact or containment.

## Offline workload measurements

The five native assembly pairs each needed one exact triangle test and 8–15 node
visits. In the qualification replay those public API calls took 2.404–4.217 ms each.
This is a favorable five-sample smoke measurement; difficult-layout results below
use a separate fixed benchmark recipe. The workstation has an Intel Core Ultra 9
275HX, 24 logical processors and Windows 11 Pro build 26200; Python is 3.11.9.
Neither workstation load nor thermal state was controlled.

The installed-wheel benchmark (`9fb030f`) retained 9 warmups and 27 measured calls:
15 measured calls completed and 12 exhausted their work budget. All exact expectation
checks passed; no exhausted result contained a numeric minimum. Measured wall times
below are seconds, with three repetitions for each row.

| Layout | Triangles per side | Outcome | Minimum | Median | Maximum |
|---|---:|---|---:|---:|---:|
| Overlapping boxes | 32 | Complete | 1.388349 | 1.399351 | 1.920168 |
| Overlapping boxes | 128 | Exhausted | 2.857332 | 2.970988 | 3.300821 |
| Overlapping boxes | 512 | Exhausted | 2.989231 | 3.047499 | 3.329028 |
| Thin triangles | 32 | Complete | 1.495099 | 1.722227 | 1.743823 |
| Thin triangles | 128 | Exhausted | 2.987670 | 3.416127 | 3.432655 |
| Thin triangles | 512 | Exhausted | 3.084738 | 3.123857 | 3.131380 |
| Near parallel | 32 | Complete | 0.005460 | 0.005814 | 0.005873 |
| Near parallel | 128 | Complete | 0.018757 | 0.019982 | 0.020766 |
| Near parallel | 512 | Complete | 0.079362 | 0.085259 | 0.091560 |

The hard 32-triangle cases used 1024 triangle tests; their larger cases stopped at
2048. Every near-parallel case needed one triangle test. Node visits ranged from
21 to 4114. Raw rows, exact rational input recipes, source/input hashes and copied
script are in `Saved/AssemblyBenchmark-20260913-01`; command, tests and process-state
evidence are in `Saved/AssemblyTask3-20260913-01`. An unrelated UE 5.8 NullRHI
commandlet was active during timing. No processes were closed or changed.

Three deterministic layouts use 32, 128 and 512 triangles per side, one warmup and
three repetitions with rotated execution order. Every measurement includes preparation, input hashing,
transforms, tree construction and exact search. Fixture construction and export
are outside the timed section. The fixed work limits are 2048 triangle tests,
8192 node visits and 256 coordinate bits.

Overlapping-box and long-thin-triangle cases deliberately frustrate pruning. The
near-parallel case has a known positive separation. Completed and exhausted
attempts are reported separately; exhaustion publishes no partial minimum. These
duplicate-triangle controls are narrow stress cases, not representative game-mesh
costs or a worst-case wall-time guarantee. No search optimization is included.

## Replay and retained evidence

The full native archive `Saved/AssemblyNativeFull-20260913-01/replay-evidence.zip`
contains 208 verified entries. SHA-256:
`3f758922b32202f16834eb624a55d66606ecbd407b46a0aa5adbe5d700a0817a`.
The verifier rechecked the inventory and removed 60 generated PNGs only after
archival verification, then removed its owned temporary host. The assembly fixture
itself exports no images. All 41 assembly entries were restored and their sizes and
SHA-256 hashes verified under `Saved/AssemblyReplayRestored-20260913-01`.

The follow-up fixture run retains 45 verified entries, including its separate
precision control, with archive SHA-256
`3fe339a379c556e10a5919edf0029110104ea681bfe3abeed869ffd5a4b90e2c`.
Its 41 assembly entries were independently restored and hash-checked under
`Saved/AssemblyReplayRestored-20260913-02` for the installed example.

Initial compile/setup failures, the two cube-asset mismatches and the demonstrated
precision failure remain retained. The intermediate successful fixture run with
six-decimal replay is retained as intermediate evidence, not end-to-end acceptance.

The complete delivery archive is
`Saved/AssemblyWorkflowWorktree/Saved/AssemblyDelivery-20260913-evidence.zip`
relative to the root repository. Its 716 entries were independently hash-verified;
SHA-256 is `3058e568e20c27b6d600eae5044d197db405e036e4a0e34a0f7bcd6a71229eca`.
The sibling `.inventory.json` and `.retention.json` preserve the verification receipt.
It includes replay archives, failures, installed wheel, test logs, benchmark inputs,
reports and source snapshots at implementation head `9fb030f` plus the documented
working-tree documentation changes. This receipt-bearing document is excluded to
avoid a circular archive hash. No inputs were deleted by the delivery collector.

Both earlier mesh delivery archives were rechecked unchanged, with receipts in
`Saved/AssemblyPriorArchivePreservation.json`. The restored-example comparison is
retained in `Saved/AssemblyExampleQualification-20260913-02/qualification.json`.

## Implementation decisions

- Added the smallest rigid batch prerequisite so pairs share a native acquisition;
  the cost is an additional opt-in API and native rebuild.
- Preserved ignored development artifacts and existing worktrees for evidence;
  the cost is local disk usage. Future cleanup requires archive verification.
- Used EngineMeshes/Cube at exact scale `25/64`, after replay disproved the initial
  cube assumptions. Explicit bounds and topology tests establish 100 cm sides and
  twelve triangles; a future fixture change requires requalification.
- Preserved exact double serialization instead of rounding identities; new export
  hashes change while schema 1 and old recordings remain unchanged.
- Fixed the named demonstration's semantic acceptance profile so shortened inputs
  cannot certify it. Customized workflows use the configurable public APIs through
  an adapter; changing this demonstration's contract would require a separate mode.

## Remaining limits and consumer boundary

Intervals describe the supplied samples only. Between-sample behavior is not
evaluated; missing source bundles make the example insufficient independently of
the surviving samples' gap assessment. Morphs, cloth, mesh deformers, material
displacement and raster visibility remain explicit exclusions. Requiring an
excluded effect prevents an adequate result for that workflow.

Containment, penetration depth, continuous collision, support/sliding and arbitrary
live skeletal batching are outside this slice. Katana owns its dependency pin,
consumer rebuild, region selection, effective surface inventory and gameplay
qualification. The neutral result does not establish consumer adequacy. Its findings
will determine whether additional acquisition support is necessary before broader
deformation work.
