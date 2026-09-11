# Native mesh reference delivery

Scope: implement the first native slice following `MESH_OBSERVATIONS.md` and
`SURFACE_CAPABILITIES.md`. CPU bone and rigid geometry are explicit references,
not final rendered surfaces. Preserve existing image producers and Python schema 1.

## Task 1: Reference acquisition and admission

Add `AnimationCaptureMeshReference.h/.cpp`: explicit fixed component enrollment,
caller identities and analysis LOD, finalized single-node bone poses and ordinary
static-mesh components. Never drive animation, force LOD, enable CPU access, finish
compilation or change rendering. Reject missing resident data, leader poses,
unqualified animation/physics ordering, replacement and stale finalization.
Retain immutable positions, complete global indices/sections and acquisition transform
without UObject access after capture. Fingerprint effective weights/configuration.
Use shared count/byte admission retained until the last snapshot reference dies.
Bounds cover producer payload/work buffers; engine storage and allocator/RSS overhead
are outside this accounting. This CPU-only budget is not combined GPU/image admission.
Record explicit exclusions and conservative material/deformation inventory reasons.

## Task 2: Native replay

Implement schema-1 serialization in a separate private file, with canonical topology
hashing matching Python, little-endian binary buffers and acquisition/completion
identity. Publish only complete bundles under a stable trusted root; reject reuse,
escaping paths and unsupported filesystem semantics. Verify native output through
the existing Python reader, including Unicode identity and multiple sections.

## Task 3: Neutral host qualification

Promote the prior procedural two-bone animated fixture into the existing host and
add multiple sections and rigid attachment. Exercise acquisition, independent scalar
geometry, distinct LOD topology, stale/finalized revisions, replacement, retirement,
unsupported enrollment and shared-budget saturation/release. Fixture setup may drive
animation; the observer may not. Add an interactive inspection command using the
same producer. Preserve result bundles in `Saved/Observations` for verifier retention.
Measure repeated warmed CPU acquisition on a fixed workload; report raw samples and
limits without a GPU throughput or final-surface claim.

## Task 4: Verification and handoff

Run distribution isolation and the native host build, new controls and existing
rendered regression suite. Replay native bundles with installed Python 0.3.0.
Review implementation and fix findings. Archive/hash evidence before deleting any
inventoried images. Commit verified local slices and update capability/delivery docs.
Katana's owner retains dependency-pin edits and consumer integration. No remote push.

## Decisions and progress

- Ruling: qualify single-node skeletal ordering first; other animation graphs,
  post-process, leader poses and physics ordering remain unavailable until tested.
- Ruling: reference exclusions are explicit output semantics; material connectivity
  and stored feature absence do not certify an inactive rendered contribution.
- Ruling: native publication targets the verified Windows host; broader platforms
  may reject publication until exclusive completion semantics are implemented.
- Interface review: tasks 1/2 share immutable snapshot metadata; tasks 1/3 share the
  public sampler; task 4 consumes all evidence. No consumer edits or GPU quota claims.
- Tasks 1-3 complete: explicit CPU/rigid acquisition, schema-1 replay and neutral
  automation/interactive controls. Final host run passes 18 controls; Python isolation
  passes 52 tests (one optional-image skip in core installation).
- Review rulings: require evaluated weighted/socket bones; reject reference-pose
  overrides; fingerprint inverse binds/weight layout; serialize exports on the game
  thread so concurrent writes cannot multiply unaccounted work buffers.
- Task 4 complete: native implementation committed as `4a2975a`; final-source runs
  pass all 18 controls, distribution isolation passes, eight archived native bundles
  replay, and verified retention precedes removal of 44 loose PNG copies. See
  `docs/NATIVE_MESH_REFERENCE_DELIVERY.md` for hashes, measurements and consumer boundary.
