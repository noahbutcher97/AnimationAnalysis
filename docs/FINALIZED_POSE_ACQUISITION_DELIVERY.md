# Finalized pose acquisition delivery

Status: native, replay and archive qualification complete; final review pending,
2026-09-13.
Baseline is the completed neutral assembly delivery `a49850f`; its evidence remains
in [the assembly report](NEUTRAL_ASSEMBLY_DELIVERY.md).
The implementation is committed through
`b0de8a677fd486c51244df8a3a120372c380a89b` on
`feature/finalized-pose-acquisition`. No push, merge or consumer change is included.

## Consumer findings and reproduction

Katana qualified shared commit `2fb0dc980dbdba1348b02e3a93b4512606aa5d16`.
Its reports are `docs/audits/ANIMATION_ANALYSIS_CONSUMER_HANDOFF_2026-09-13.md`
and `docs/audits/MESH_REGION_PAIR_INTEGRATION_2026-09-13.md` in that consumer.
Both reports were read after finishing the neutral delivery, as requested.

| Finding at the consumer pin | Newer neutral delivery | Remaining shared requirement |
|---|---|---|
| AnimBlueprint bodies and their rigid attachment fail single-node pose qualification | Unchanged; deliberate original scope | Explicit finalized ordinary animation policy with stale/replacement/attachment controls |
| Ten eligible records produce five acquisition-mismatched pairs | Rigid-only batches establish honest shared time for unparented static components | Extend bounded synchronous acquisition to supported skeletal components and direct rigid attachments |
| Native replay loses timestamp/transform precision | Fixed by double round-trip serialization | Keep original historical clocks; precision alone cannot align separate captures |
| Masked/PDO facial-hair slots lack final visible-surface evidence | Explicit material/raster exclusions remain | Require visible coverage when the selected consumer question needs it; no blanket deformation extension |

The consumer archive `mesh-integration-evidence.zip` has SHA-256
`c27c83854fe1d5b49a6b6a83eb09ba015a8e7b85849c7e6e5408bc52b8ffd9fa`.
All 751 payload entries, their sizes/hashes and embedded inventory were independently
verified. Authored references, final analysis and live inventory were restored only
under AnimationAnalysis's ignored evidence directory.

Using the installed Python 0.4.0 public APIs, the retained ten observations again
qualified individually. All five original pairs returned exactly
`second:acquisition_mismatch`, without a numeric minimum. Their observer-local
poses, acquisition and completion timestamps were preserved. The reproduction is
`Saved/KatanaReproduction-20260913-01/reproduction.json` in the delivery worktree;
its script is `Saved/reproduce_katana_integration.py`. No consumer adapter or
record was edited or used to replace acquisition clocks.

The final retained live callback at montage time 0.403363 s records two ordinary
skeletal components using `ABP_SamuraiCharacter_C`, LOD 0 and 89 required bones.
Both CPU/GPU enrollment failures and direct rigid attachment failures carry the
single-node-only rejection. At that callback there was no running parallel
evaluation, leader, post-process instance/class, physics blending or reference
override. These facts reproduce the qualification decision against current source;
they are not a fresh live consumer run or proof of whole-interval state.

The victim's effective slots 6/8 are masked/PDO. Its selected region contains 2,282
triangles from those slots according to the consumer's coverage audit. Pre-material
geometry cannot certify the visible surface. Other morph/cloth/deformer observations
describe the recorded callback only. Full-region search and sustained live 30 Hz
performance have not been measured by this reproduction: pair checks reject before
search, so their duration is not geometry-analysis cost.

## Implemented compatibility boundary

The implementation adds an explicit finalized-animation policy and mixed
synchronous reference batches. Default single-node enrollment, narrower rigid-only
batches, Python 0.4.0 and replay schema 1 remain compatible. Native consumers must
rebuild and opt into new behavior. Existing independent acquisitions remain separate.
GPU single-component pose qualification is included; GPU group acquisition is not.

Independent review caught stronger checks incorrectly affecting the default
SingleNode policy. Commit `949bedd` restores its original guards and supported
behavior, with a failing-before/passing-after compatibility control and accepted
scoped re-review. The stronger initialized-instance, pending-update, linked-instance,
forced-reference-pose and update/bone witnesses belong to explicit FinalizedAnimation
enrollment. Default configuration hashes and producer identities remain unchanged.

Katana retains ownership of pin updates, adapter changes, assets, rebuild and fresh
live interval qualification. Do not switch its live pose mode or retime records.
Required unsupported material/raster effects remain insufficient. Containment,
penetration, swept collision, physical contact and artistic acceptance remain outside
this change. Candidate identity will be recorded after review.

The shared changes address both reproduced blockers at their source. The explicit
`FinalizedAnimation` policy admits qualified ordinary AnimBlueprint components and
direct rigid attachments. `CaptureBatch` establishes one native acquisition before
preparing the selected mixed CPU participants; it publishes all immutable outputs
or none. Each output retains its actual completion and observer-local revision.
Independent `Capture` calls still produce independent acquisitions. No consumer
retiming, pose driving, synchronous GPU wait or hidden CPU fallback is introduced.

## Package verification

Fresh `Python/verify_distribution.py --output Saved/FinalizedPoseDistribution-01`
passed 104 tests plus one optional-image skip without Pillow and all 105 tests with
image support. All 11 isolation/install/CLI commands passed and the verifier removed
its owned temporary environment. Wheel SHA-256:
`0eaa53decd4c00315299177ca58c13119c50f497948a5c22b8cd68f2888ea3c4`.
All 28 package modules match the checkout byte for byte. A separate installed-wheel
environment is retained at `Saved/FinalizedPoseExampleEnv` for native replay checks.

The policy intentionally excludes linked animation instances and preserves existing
GPU view/LOD/material restrictions. Katana's retained inventory does not establish
linked-instance absence or qualify its eventual GPU view. Fresh integration must
check these prerequisites; CPU reference batching does not depend on a GPU view.

## Native qualification

The independent full run `Saved/FinalizedPoseNativeFull-20260913-01` passed all
26 exact controls at native commit `949bedd`, using UE 5.6.1 CL 44394996/D3D11.
There were no missing, extra, duplicate, failed or skipped tests. The set includes
existing RGB/depth, lifecycle, budget, renderer-mismatch, rigid-batch and GPU-combined
controls plus the new real graph/slot/montage and legacy-default controls. All 44
staged source files and both verifier tools match the checkout byte for byte.

The isolated build took 48.594 s and the editor run 56.500 s, including startup and
teardown. No source warnings or compiler errors remain. UBT reports the existing
Visual Studio compiler-preference warning. This is separate from the corrected
fixture GC warnings. The verifier removed its temporary host.

All 254 replay payload entries and the embedded inventory were independently
verified. Archive SHA-256:
`c14727dda13f450e6e4fae3c1edf093500275c82334fa8491834f733ed43fcf5`.
The verifier deleted exactly the 60 inventoried PNGs only after archive verification;
all other retained files were checked against their recorded hashes. The finalized
fixture's 46 files were restored under `Saved/FinalizedPoseReplayRestored-04`.
`Saved/FinalizedPoseNativeFull-readback.json` records source, test and cleanup checks.

Five small CPU pair captures took 0.0546–0.0666 ms (median 0.0585 ms). One GPU sample
took 0.0399 ms preparation, 0.00470 ms capture, 0.00020 ms setup wait and 0.0970 ms
decode. Its acquisition-to-completion interval was 20.7901 ms; deliberate later
collection occurred after 53.3396 ms, after montage advance and source retirement.
The original GPU geometry/revision was retained. Peak admitted bytes were 11,550,560;
this is producer reservation accounting, not process RSS. These tiny-fixture samples
do not establish consumer-size costs, sustained cadence, a quiet system or a thermal
performance baseline. The [assembly benchmark](NEUTRAL_ASSEMBLY_DELIVERY.md) retains
the separate difficult-workload limits and exhaustion evidence.

Earlier fixture failures are retained. Installed DLL disassembly and UE source
located the initial AnimGraph null read at the missing PropertyAccessEditor compiler
service. An explicit prerequisite control reproduced the absence without crashing;
enabling that engine plugin only in the host resolved compilation. Separate fixes
retain transient assets across map-load garbage collection, replace an animation
instance using an actual class change, and establish the existing GPU diagnostic
view/Skin Cache prerequisites. None switches a consumer's live pose mode or relaxes
GPU admission. A forced-reference-pose rejection control exposed and fixed a missing
FinalizedAnimation qualification guard. Review also caught raw collector references
that produced UE 5.6 incremental-GC safety warnings. They now use `TObjectPtr`; the
corrected fixture passes with no C4996 warnings. A separately configured incremental
GC stress run was not performed.

Independent diagnostic replay of ten CPU bundles from the earlier, later-failed
`GREEN-12` run already measured all five expected distances and intersections with
installed public APIs. `Saved/FinalizedPosePartialReplay-01/report.json` labels this
as partial CPU evidence, without a completed native manifest or GPU claim. Its
40-entry source archive was independently verified before restoration.

## Consumer integration checks

The neutral replay example uses an interpreter with Python 0.4.0 installed:

```powershell
python -I Python/examples/finalized_pose_pairs.py --observations "<native FinalizedPose directory>" --output "<new report.json>"
```

It is a fixed-fixture qualifier for the explicit five-pair manifest, not a consumer
adapter. Other workflows supply their own records, regions and requirements to the
public analysis API. After review fixes at `b0de8a6`, the example passed all 11
focused tests and all 53 tooling tests. Installed original/restored replay runs each
measured five pairs, with a complete sampled interval and all six detached controls
returning insufficient. Coverage must exactly match the observed role/pose-ordering
features and five explicit exclusions; incomplete or inactive substitutions cannot
qualify. The fixed profile also enforces five distinct frames and differing first-pair
observer revisions. Directory and Windows symlink manifest rejection controls passed.

Final reports are under `Saved/FinalizedPoseInstalledQualification-Task2-Fix1-01`;
`Saved/FinalizedPoseInstalledQualification-Fix1-readback.json` is the controller's
independent readback. Earlier failing and initial passing runs remain retained.

Independent readback checked all 41 explicit input hashes, bound each reported
completion/request/pose/acquisition to its native record, and compared reports
after removing only elapsed analysis timings. Expected distances were 25, 15, 5,
0 and 0 cm, with intersections only in the last two pairs. The actual maximum
sample gap was 0.0186662 s; between-sample geometry remains unevaluated.

The final original-evidence Python pair analyses took 75.0388, 76.4377, 67.9673,
10.7160 and 10.9660 ms. These are offline search costs, separate from the native
capture timings above. Their inputs are a two-triangle body and a twelve-triangle
rigid part. Neither this small sample nor a sufficiently short capture gap proves
sustained 30 Hz analysis, especially for Katana's substantially larger selected
regions. End-to-end CLI wall times were 396.079 ms for original evidence and
439.292 ms for restored evidence; reports retain their individual elapsed timings.

After native qualification and candidate review, the consumer owner can test the
smallest change against its retained authored-reference and live finisher workflow:

1. Rebuild against the candidate and explicitly select `FinalizedAnimation` when
   enrolling each live body and rigid attachment. Enroll before a subsequent
   finalization; retain the existing live AnimBlueprint and montage behavior.
2. At the completed-world-tick boundary, pass the explicitly selected body and
   attached-part samplers to one bounded `CaptureBatch`. Export only its successful
   immutable outputs. Preserve each component's revision and actual completion.
3. Replay through the unchanged Python 0.4.0 API using the consumer's topology-bound
   regions, original acquisition and explicit requirements. Independent old captures
   must continue to reject; a matching engine frame is insufficient.
4. Verify eligibility throughout the selected interval, including absent linked
   instances, current finalizations, evaluated socket bones and stable attachment
   transforms. Stale, replaced or pending state must produce no partial pair.
5. Measure native capture, export and full selected-region analysis separately,
   including budget exhaustion and real sample gaps. Historical authored-reference
   capture timings do not establish live cadence or full-region search performance.

For the selected masked/PDO facial-hair triangles, either the question explicitly
accepts the pre-material surface or its required visible-surface result remains
insufficient. This finding supports a targeted coverage decision; it does not
establish a requirement for every deferred deformation feature. Single-component
GPU support also does not establish synchronized GPU/body-and-rigid pair acquisition.

## Decisions and remaining limits

- Retain the ignored worktrees, review records, failed runs and needed replay.
  This follows the requested evidence policy and costs local disk space; any later
  image cleanup still requires verified archival.
- Enable only the neutral host's required engine compiler plugin for its real
  AnimBlueprint fixture. Production module dependencies remain unchanged. This adds
  a host prerequisite and startup cost; other engine versions require qualification.
- Preserve the original default SingleNode behavior and put the stronger checks
  behind explicit FinalizedAnimation enrollment. This protects existing consumers
  but requires callers to opt in to the new witnesses.

The task review also identified a maintainability issue in the large native fixture:
asset construction, lifetime, controls and replay publication share one file. Final
review will assess whether helper extraction is needed for this delivery.

The verified shared changes establish a candidate for Katana to rebuild and test.
They do not establish successful consumer integration. Remaining checks include live
linked-instance state, socket/pose witnesses across the actual interval, selected
material coverage, complete region-analysis budgets and measured capture/export/
analysis cadence. Broader morph, cloth, deformer and rendered-surface support remains
a separate, evidence-driven extension; unsupported required coverage stays unknown.

## Delivery evidence

The local worktree is `Saved/FinalizedPoseWorktree` under AnimationAnalysis.
Paths in this report are relative to that worktree. Its complete evidence archive is
`Saved/FinalizedPoseDelivery-20260913-evidence.zip`, 20,856,528 bytes, SHA-256:

`daaa37bbcbcb5587e840ee5746441e590bbf9c59aa1829a85ce56afcfb92ab4c`.

All 1,451 payload entries and the embedded inventory were independently verified.
The archive includes the selected replay, failed-run diagnostics, package/native/
tooling reports, consumer report copies and reproduction, review records available
at collection, and a source snapshot at `3c7d51f`. All 103 non-Markdown source files
match the checkout; all 44 native staged files match the full passing run and all
28 package modules match the verified wheel. `Saved/FinalizedPoseDelivery-readback.json`
records these comparisons. The receipt-bearing delivery document is excluded from
the source snapshot to avoid a circular archive hash. Later documentation/review
receipts do not change the qualified implementation.

Collection deleted no input files. Generated native PNGs were already archived,
verified and cleaned by the native verifier as described above. Prior neutral and
consumer archives remain intact. The ignored installed interpreter and reusable host
build cache remain local and are not part of the delivery archive. The full native
command is preserved in `Saved/FinalizedPoseNativeFull-20260913-01.command.json`;
its expected set is the default 25 rendered controls plus explicit `Mesh.GPUCombined`.
