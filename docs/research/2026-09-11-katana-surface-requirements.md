# Katana reference-consumer surface requirements

Recorded 2026-09-11. This is a read-only requirements investigation of the accessible
first consumer, informing the broader [surface capability design](../SURFACE_CAPABILITIES.md).
It does not implement capture, approve choreography, or change Katana assets or pins.

## Evidence and freshness

Katana HEAD was `50685e86dc2a284408f9f8fa6fbcedd37efed470`, branch
`investigate/finisher-source-pair`, with substantial uncommitted integration and
paired-facing work. The review uses **working-tree contents**, not HEAD alone.
An explicit 23-file snapshot, SHA-256 manifest and collection script are retained
under `Saved/SurfaceCapabilityReview-20260911/` in AnimationAnalysis. Those files
were unchanged during snapshot collection. References below are relative to
KatanaCombat and describe this snapshot; they may change as its developer proceeds.

The current lock in `Tools/AnimationAnalysis/dependency.json` selects shared revision
`3fd91eb70be340db63778a697b12465ab895cd8a`. The working-tree integration report,
`docs/audits/ANIMATION_ANALYSIS_INTEGRATION_2026-09-11.md`, records a consumer build,
Python/native checks and rendered async compatibility. These are **consumer-owner
reported results**, not tests rerun by this investigation. Older guides still
describe synchronous-only capture or pre-extraction ownership; current code, lock
and dated integration evidence take precedence over those passages.

The earlier source-pair review records inspected images and asset properties. This
pass read its report and the metadata portion of its `source-assets.json`; it did
not re-review images, load assets in Unreal or inspect a live AnimGraph. Its visual
findings therefore remain attributed historical evidence.

## Concrete workflows and shared requirements

| Katana workflow and observed evidence | What AnimationAnalysis should provide | Consumer responsibility |
|---|---|---|
| `pairs/finisher-contact.json` uses a blade segment and an 18 cm `spine_03` sphere. The source-pair review reports a rear restraint/downward strike, an anatomical mismatch and partly obscured contact. | Independent body and rigid-attachment mesh observations, explicit regions, synchronized imagery, and later surface-distance/intersection measurements. Retain proxy measurements as named alternatives. | Determine intended anatomy, timing, acceptable penetration/separation and artistic interpretation. A different bone/radius must not be selected merely to obtain a pass. |
| `pairs/counter-contact.json` reuses the same proxy around an authored `CounterImpact`; both example profiles request 60 Hz. | Reuse the same records and measurement services for another interaction and interval, with coverage and gap checks. | Resolve montage/notify semantics and approve criteria. The counter is a second integration case, not independent portability proof. |
| `PairedContactProfileEvaluation.cpp`, `AcquireAuthored` and `SampleContactPreviewPose`, create preview components, sample a restricted single-node montage, force preview LOD 0 and apply extracted root motion. Runtime comparison reads captured final points. | Distinct authored and live observation provenance; mesh/LOD/pose compatibility checks. Geometry comparison must identify the effective pose and transform in each path. | Keep paired asset discovery and montage scheduling. Preview sampling is allowed to drive its own scene; the live recorder must not drive gameplay pose evaluation. |
| `CombatCaptureScenarioTests.cpp` explicitly enrolls actor meshes and the matching attached `UStaticMeshComponent` for weapon sockets; destruction remains missing evidence. | Multiple explicit component observations per subject, stable component generations, attachment identity and topology. A skeletal body alone omits the weapon surface. | Select components and meaningful roles. Never discover anatomy, attachment or replacement components through generic naming defaults. |
| `scenarios/finisher-recovery.json` version 5 requests 60 Hz motion, 30 Hz frames, 960x540 images and four selectable camera views. Each view is a separate run. | Original observation clocks, actual cadence, immutable pose links and visible sampling gaps. Surface acquisition cadence must be independent of image export cadence. | Supply scenario actions, camera choices and comparison context. Separate runs cannot be represented as simultaneous stereo evidence. |
| Working-tree facing and sync controls vary placement, heading, event timing and nudge settings while retaining asset/override provenance. | Compare explicitly compatible experiments; distinguish observed outcomes from causal interpretation. Surface proximity must not stand in for damage timing or applied warp work. | Own gameplay changes, exact event instrumentation, source assets and dependency integration. |
| The integration report records one synchronous PNG queue overflow and two passing repetitions with unchanged bounds. | Combined mesh/RGB/depth/export admission, observable loss, exact terminal results and retained unsuccessful runs. Reduced export or cadence can be explicit user choices. | Select workload budgets. Passing repetitions do not prove the intermittent overflow fixed; no cause is established here. |

Profile paths in this table are under `Tools/CombatCapture/`. Native paths are under
`Source/KatanaCombatEditor/Private/Analysis/`, except the scenario fixture under
`Source/KatanaCombatTest/Private/`.

## Asset and deformation knowledge

The profiles explicitly nominate CyberpunkRunner `SKM_CyberpunkRunnerr_B`,
FuturisticMercenary `SKM_FuturisticMercenary_FullBodyC`, and the separate
`SKM_Manny_Simple` victim profile for DefenseMatrix. Weapon data is
`/Game/ProjectFiles/Data/PDA/Weapons/DA_Weapon_Katana`; native source confirms
static-mesh attachment sampling despite the weapon asset's `SKM_` name.

The historical inspector reports the attacker/victim sharing `SK_Mannequin` and
using root-motion source sequences. Its mesh properties cover skeleton identity,
not a complete deformation/material inventory. The Content path inventory includes
modular mercenary parts and facial-hair materials/textures; filenames establish
available content only, not instantiated components or active rendering behavior.

`Config/DefaultEngine.ini` enables Skin Cache shader compilation and selects D3D11.
That setting cannot prove that each observed section uses Skin Cache. Current
effective runtime settings, memory pressure, LOD overrides and render-path selection
must be observed. The targeted text search found no additional deformation wiring
in `Source`, `Config` or `Tools/CombatCapture`; Blueprint and binary asset behavior
is outside that search.

**Bone-only adequacy for Katana remains unproven.** Active morphs, cloth,
post-process/Control Rig evaluation, leader-pose components, skin-weight overrides,
mesh deformers, material displacement, masked/translucent sections, actual render
LODs and topology validity remain unknown. No absence claim follows from the text
search or the earlier neutral bone-only experiment.

## Required consumer qualification

Before declaring the new surface path useful for Katana, its owner should exercise
the shared capability inventory and capture APIs on both real-map finisher cases
and the counter. Retain, per component and sampled interval:

1. Asset/content identity, attachment/leader identities, active components,
   render LOD, section/material mapping, vertex/index counts and resident resources.
2. Effective deformation path and evidence for active morphs, cloth, graph/post-process
   output and skin-weight overrides; retain changing configuration generations.
3. Material/pass behavior relevant to geometry, depth and visibility, including WPO,
   pixel depth offset, masking and transparency. Unknown detection stays unknown.
4. Acquisition pose/finalization identity, world transform, renderer observation,
   actual sample gaps, missing sections and per-channel budget/loss results.
5. Explicit body/weapon region intent from reviewed evidence. A material section or
   skin-weight partition is not automatically an anatomical region.

The shared developer owns the reusable inventory/records and neutral qualification
controls; Katana's developer owns applying them to its project and all resulting
consumer edits. If an active feature is outside the validated backend, either
qualify that feature before the consumer claim or expose an explicitly limited
observation. Do not call that result full rendered-surface contact evidence.

## Disposition

Katana provides a strong first validation workflow: body/weapon observations through
approach, restraint, strike and release, checked alongside authored playback and
rendered evidence. It also exposes attachment, clock, region and budget requirements
that a bone-position-only API would miss. Broader cloth, facial, deformer and
material capabilities remain product requirements even if Katana does not use them.

This pass changes documentation only. No new runtime or performance results were
produced; no editor was launched and no consumer files were written. No images were
generated or cleaned. Existing replay archives remain untouched. The snapshot and
verification evidence are retained as described in the surface capability design.
