# Finalized animation pose and coherent reference acquisition

## Scope and evidence

Continue from verified neutral delivery `a49850f`. Keep every implementation change
inside AnimationAnalysis. Katana owns consumer edits, assets, pins, rebuilds and
gameplay qualification. No push or merge is included.

Retained consumer archive `c27c83854fe1d5b49a6b6a83eb09ba015a8e7b85849c7e6e5408bc52b8ffd9fa`
was independently verified (751 payload entries). Installed Python 0.4.0 reproduced
10 individually eligible observations and five `second:acquisition_mismatch`
results without modifying records. New rigid batching excludes skeletal/attached
participants; exact JSON serialization does not make separate captures coherent.
The retained live inventory records ordinary AnimBlueprint components and direct
rigid attachments failing the existing single-node qualification. That is retained
consumer evidence, not a fresh live consumer rerun.

## Public contract

Add explicit enrollment policy `EAnimationMeshPosePolicy { SingleNode,
FinalizedAnimation }` and `FAnimationMeshEnrollment::PosePolicy`, default SingleNode.
Existing default calls retain their supported scope and producer identities.
FinalizedAnimation admits ordinary registered skeletal components in SingleNode or
AnimationBlueprint mode with a valid corresponding instance. No leader, post-process
class/instance, physics simulation/blending, reference override, custom animation
mode or linked instances qualify. These exclusions also apply to an attached rigid
component's direct skeletal parent. Invalid policy values reject enrollment.

Enroll before the next pose finalization. Capture remains observation only: never
tick, refresh, drive montage/pose, switch modes, force LOD, wait for animation work,
or alter render settings. Require a witnessed current frame/world-time finalization,
stable enrolled pose source/asset/animation instance/mode, no ongoing parallel or
post evaluation, no outstanding instance update, current evaluated-bone/LOD coverage
and a matching animation update counter. Store the engine bone revision and check
it alongside the observer-local serial; neither same frame nor a revision alone
establishes finalization. Repeated observation may retain a witnessed revision;
pending updates, source replacement or stale finalization remain unavailable.

For direct rigid attachments additionally retain parent/socket enrollment, reject
absolute transforms, and validate composed socket/relative transform and evaluated
socket-bone membership. Qualified CPU geometry remains bone/rigid reference geometry.
All existing deformation and raster exclusions remain explicit.

Add `CaptureBatch` with the exact signature of `CaptureRigidBatch`. It accepts a
bounded ordered nonempty set of distinct enrolled supported components in one world
outside world tick, with maximum 1..64. It may include skeletal components and
direct rigid attachments. Establish one native acquisition stamp before preparation;
do not export, tick, yield or call consumer callbacks between preparations. Preserve
component generations, observer-local revisions and actual separate completions.
Failures return no snapshots and release partial reservations. Validate stable
participants/frame/world and witnesses before publication. Preserve the narrower
`CaptureRigidBatch` contract and existing single-capture behavior.

The existing GPU single-component path accepts the explicit finalized-animation
policy through shared preparation, with its existing exact view/LOD/transform and
renderer-matrix controls. Coverage text must describe the actual policy. It never
CPU skins or falls back. This slice does not add GPU group acquisition: independent
GPU/rigid calls still cannot be paired by proximity of clocks or completion times.

No Python contract, replay schema or package version change is needed. Replay remains
schema 1 and Python 0.4.0; old records retain their original precision and clocks.
Use distinct configuration identity for the new policy without changing default
single-node configuration hashes solely because the policy field was added.

## Native qualification

Use only transient neutral assets and Engine/plugin host dependencies. Construct
a real transient compiled AnimBlueprint graph with a slot and real montage/sequence,
not a mock animation instance or a switch to SingleNode. Use two ordinary skeletal
components and a rigid component directly attached to one of them. Assert animation
mode, instance, slot/montage execution and independently expected animated vertices
and attachment transforms. Fixture setup may drive animation; acquisition may not.
Include ordinary gameplay-style parent transforms on skeletal components.

Export five coherent CPU body/attached-part pairs through schema 1 and a bounded
explicit manifest. Use purpose-based role/request names. Provide independent literal
geometric expectations in that fixture's manifest, along with original frame,
acquisition, per-component revision/configuration/topology and actual completions.
Keep geometry small enough that successful offline pair checks are practical.
Sample across distinct observed frames; report actual clock gaps without retiming.

Required controls: default policy rejects the graph; opt-in enrollment works before
finalization but capture does not; finalized geometry matches analytic motion;
pending same-frame update and previous-frame witness reject; one-side instance or
asset replacement rejects until reenrollment/finalization; absent LOD/required bones,
unsupported modes/features and unsynchronized attachment reject; batch duplicate,
cross-world/count/budget failures return no output or leaked reservations. Late
collection of a GPU graph result preserves its earlier acquisition/revision/geometry
after the fixture advances or retires. Existing GPU renderer mismatch guards and
all prior capture controls must remain valid.

Installed Python replay must qualify two distinct native components as a successful
pair, preserve independent completions and observer revisions, and keep detached
wrong-pose, one-side replacement, required excluded coverage, exhausted-work and
gap/missing-endpoint controls insufficient. Completion timing does not choose pairing.
Historical Katana records must remain insufficient with unchanged hashes.

## Verification and delivery

Follow TDD for native boundaries, retain failed runs, build the isolated UE5.6.1 host,
run exact focused tests then the affected full D3D11 set, and verify distribution
isolation. Reuse same-source core evidence where justified by byte comparison.
Record native capture/replay and analysis timings as local observations, not a
30 Hz or real-time guarantee. Preserve archives and verify needed replay before
cleaning generated images. Commit verified slices, perform scoped/final reviews,
and return a local candidate with compatibility and consumer verification notes.

Masked/PDO facial hair remains outside pre-material surface claims. These findings
do not establish a need for all deferred morph, cloth, material, containment or
continuous-collision work. The live consumer interval and full-size region search
cost still require the consumer owner's fresh qualification.
