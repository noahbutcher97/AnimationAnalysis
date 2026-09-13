# Deformation deferral review

Reviewed 2026-09-13 against `784acd2`. This is a source and requirements review,
with five fresh Python coverage controls. No new native deformation experiment or
live consumer qualification was performed. Earlier asset findings remain dated
evidence; Katana assets, adapters and dependency pins were not changed.

## Conclusion

Keep core mesh analysis next, but make the deformation deferral conditional on
the geometry each supported workflow requires. The first analysis slice can use
qualified bone/rigid observations. That does not establish that every intended
consumer can supply eligible observations or that the broader product is complete.

Morphs, cloth and material effects can change the measured surface enough to
change an intersection result. For a workflow requiring that surface, support is
a correctness prerequisite. Calling it optional higher fidelity is appropriate
only for a workflow that explicitly accepts the narrower geometry. Do not change
the intended surface, acceptance threshold or claim merely to obtain eligibility.

## Findings affecting the decision

| Area | Evidence | Consequence |
|---|---|---|
| Morphs | Installed UE 5.6 `GPUSkinCache.cpp:789-828` has morph/cloth dispatch variants. Our GPU producer rejects active or external morphs at `AnimationCaptureMeshGPU.cpp:98-102`. | A morph extension may reuse the cached position path. This is an architectural inference, not proof of correctness or low effort. Do not group its unknown cost with arbitrary material reconstruction. |
| Cloth | Our GPU producer rejects an entire observation when any selected-LOD section has clothing data (`:105-107`). Engine code also handles cloth mapping and simulation LOD. | Even a desired region outside the garment can be blocked by the current component-wide policy. Offline region selection cannot recover an observation the producer rejected. Garment/body analysis requires cloth geometry; an explicit body reference answers a different question. |
| Pose evaluation | `QualifiedPose` in `AnimationCaptureMeshReference.cpp:25-35` requires ordinary single-node animation and rejects graph/post-process, leader and physics cases. The GPU producer reuses this qualification. | Live animation support may be an earlier core integration blocker than morphs or cloth. Verify the chosen workflow instead of inferring adequacy from the neutral fixture. |
| Material behavior | WPO changes vertices; PDO changes pixel depth; masking changes visible coverage. Engine base-pass shader source confirms the stage distinction. | Treat geometric measurements and rendered visibility/depth separately. Pre-material triangles can support a declared geometric result without qualifying the visible surface. |
| Portable contract | `FeatureCoverage` applies to every included section. `MeshRequirement` requires explicit accepted exclusions and rejects missing/unknown required coverage. | Existing records support honest limited results. Mixed-section coverage, simulation-state provenance and topology-bound regions still need design review before claiming extension compatibility. |

Epic's [UE 5.6 rendering-path guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/skeletal-mesh-rendering-paths-in-unreal-engine?application_version=5.6)
describes Skin Cache buffers and availability limits. Its
[material-input guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/material-inputs-in-unreal-engine?application_version=5.6)
separately describes WPO, PDO and masking. These references support the architectural
distinctions; neither validates our producer with those effects enabled.

The [2026-09-11 loaded-asset inspection](2026-09-11-animated-surface-readiness.md#katana-asset-inventory)
found zero stored morph/clothing assignments on the three selected skeletal meshes,
with masked/PDO materials and a used null material slot. It supports beginning with
bone geometry, but did not run gameplay components or prove absent live overrides.
It supplies no evidence about how prevalent these features are across other users.

## Conditions for retaining the deferral

1. Define each initial workflow's participants, regions, intended surface and
   measurement. Include Katana plus a neutral noncombat case through the same APIs.
2. Establish that those observations can actually be acquired at the required pose
   and cadence. Consumer-owned live inventory/integration remains outstanding;
   neutral/offline analysis work can proceed while it is collected.
3. Connect coverage eligibility to the analysis entry point and report. Test an
   omitted required effect that would change the expected measurement; it must
   produce insufficient evidence, never a confident pass on substitute geometry.
4. Review region/topology, mixed-section coverage and acquisition/simulation
   identities before freezing new analysis contracts. Synthetic known-deformation
   inputs can test portable measurements without claiming native deformation support.

Default production ordering remains core analysis, then broader deformation.
Revisit an individual deferral if the selected core workflow cannot run without it,
an omitted contribution invalidates its required result, or a concrete contract
assumption would force avoidable rework. A small neutral morph experiment is justified
when it resolves one of those questions; broad extension implementation is not
required merely to start the analysis layer. Full positive native controls remain
mandatory before advertising each extension.

The next review point is the first usable analysis slice with consumer qualification
evidence, rather than an undefined end of all suite work. No live consumer sufficiency,
extension effort or broad product coverage is established by this audit.

## Verification

Five existing `MeshRecordTests` passed freshly: identity mismatch, explicit excluded
cloth, missing/unknown/unsupported required coverage, exclusion permissions, and
witnessed inactive/unfamiliar features. These validate portable eligibility behavior,
not native detection or a future analysis service's use of that behavior.

Logs and SHA-256 identities for the reviewed producer, record/test and engine shader/
Skin Cache sources are retained under
`Saved/DeformationDeferralReview-20260913T143440909618Z/`. No images were generated or
cleaned, and prior replay archives remain unchanged.
