# Suite migration and ownership

This repository owns the extracted foundation. KatanaCombat remains its first
consumer. Repository extraction does not close the remaining suite work. Paths in
the inventory below name the KatanaCombat integration workspace; contributors need
no local AI setup or private workflow definitions to interpret them.

Current layout: `Python/src/animation_analysis` owns portable contracts, geometry,
raster/pixel review, integrity, identities, metrics and temporal services. `Source/AnimationCapture`
owns the Unreal recorder, PNG writer, synchronous/asynchronous surface adapters and
[CPU bone/rigid references](NATIVE_MESH_REFERENCE.md). `Python/UnrealHost`
owns neutral native tests. The old project paths below are compatibility or migration
locations; shared implementations now have this repository as their source of truth.

## Product direction and reference consumer

The suite targets a broadly reusable, marketable product with robust surface
observation and analysis. Plan for the most complete practical deformation coverage
and let users explicitly select capabilities, fidelity and resource cost for their
needs. Bone-skinned geometry is a useful candidate capability and correctness baseline;
it does not define the product's final surface coverage.

KatanaCombat is the accessible first consumer and a valuable reference project.
Explore its actual assets, deformation features, workflows and analysis needs to
ground requirements and test production usefulness. Combine those findings with a
broader capability matrix and neutral controls. Katana's needs inform priorities;
its current configuration does not bound the suite's supported use cases. Gameplay
discovery, assets and project-specific assertions remain in consumer adapters.

Capability selection must distinguish deformation coverage, spatial precision,
sampling cadence, latency and resource budgets. Record requested and achieved
coverage, including missing evidence and any explicitly accepted fallback. Lower
cost must not silently weaken a requested result. Analysis acceptance criteria stay
separate from acquisition fidelity; higher fidelity alone is not an artistic-quality
or physical-contact verdict. Existing defaults remain compatible until an explicit
API change is delivered and documented.

## Remaining order

The first scoped readback delivery is documented in [delivery evidence](RENDERED_READBACK_DELIVERY.md)
and [API compatibility](ASYNC_READBACK.md). Katana adoption remains with its owner.

1. Moving skeletal surface sampling through the shared contracts. The
   [source investigation and neutral experiment](research/2026-09-11-skeletal-surface-sampling.md)
   establish a CPU bone-skinned reference and a narrow cached GPU feasibility result.
   The [capability and fidelity design](SURFACE_CAPABILITIES.md) now proposes the
   production slice and acceptance criteria, informed by the
   [Katana source assessment](research/2026-09-11-katana-surface-requirements.md).
   The [animated readiness follow-up](research/2026-09-11-animated-surface-readiness.md)
   adds real animation, rigid attachment, delayed geometry/raster controls and loaded
   asset findings. The [portable record/replay slice](MESH_RECORD_DELIVERY.md) is
   implemented in Python 0.3.0. Next qualify production CPU/rigid and bounded GPU sampling. Preserve explicit
   unsupported coverage and defer production morph/cloth/graph/material extensions
   until their positive controls pass. Katana's effective live component inventory
   remains a prerequisite to consumer adequacy claims. Native implementation is open.
2. Explicit region/support and temporal surfaces; offline intersection/containment
   with supported topology/deformation limits, then separate depth/volume/swept work.
3. Move remaining generic paired evaluation/preview calculations and offline stream,
   report, launcher and retention mechanisms out of consumer implementations.
4. Keep Katana finisher/counter integrations and a neutral host exercising the same APIs.

## Whole-suite inventory carried from the consumer

| Existing family | Current coupling or boundary | Required disposition |
|---|---|---|
| `CombatCaptureSession.h/.cpp`, `CombatCaptureCommands.cpp` | Project adapter delegates lifetime/engine observation to `FAnimationCaptureSession`; discovery, humanoid defaults, combat/warp fields and telemetry switch ownership remain in Katana | Preserve compatibility while further normalizing streams and extension contracts; migrate remaining paired consumers separately |
| `AnimationCaptureImageWriter`, `ViewportSurfaceCapture`, old compatibility headers | Independent plugin implementations; opt-in `ViewportAsyncCapture` and session RGB now share bounded readback | Consumer owner adopts async APIs explicitly; preserve synchronous compatibility and defer moving-surface capture |
| `Tools/CombatCapture/analyze_capture.py` | Strict input/PNG integrity, summaries/deltas and atomic publication delegate to the portable package; legacy stream decoding, motion metrics and combat report layout remain here | Move remaining reusable stream analysis/presentation behind explicit records and supplied context |
| `evaluate_capture.py`, `summarize_runs.py`, `capture_format.py` | Shared identity, status and clock/interval services are portable; file selection and simulation-field translation are in the format adapter; gameplay checks and reference criteria remain project-side | Continue separating reusable evaluation/report assembly from combat assertions, experiment validation and reference selection |
| `run_scenario.py`, `evaluate_pair.py` | Process handling and identity collection assume the checkout layout, editor target, project file, DLL names, automation namespace and engine install default | Separate reusable execution/evidence services from an explicit project launch/identity descriptor and Katana entry points |
| `PairedContactProfileEvaluation.cpp`, `PairedAlignmentEvaluation.cpp`, `PairedContactEvaluation.h`, `PairedWarpTuning.h` | Geometry/clock evaluation is reached through project subsystem/library APIs; authored sampling resolves Katana paired data, weapon data and sync notifies | Extract common measurements/contracts; keep Unreal sampling separate from Katana asset/notify and warp-experiment adapters |
| `PairedAnimationAnalysisLibrary`, `PairedAnimationAnalysisSubsystem`, `PairedAnimationEditorTypes`, `PairedAnimationPreview`, `PairedAnimationPreviewConfig`, evaluation commandlet | Existing library/subsystem/UI split contains shared calculations, engine state and project authoring assumptions; some calculations still live in the widget | Inventory each calculation/type during migration; shared math and results enter the suite, engine state stays in adapters, project authoring/selection stays in Katana. UI and commandlet consume the same evaluation API |
| `visual_analysis/geometry.py`, package exports | Pure segment math shares a module with UE projection conventions and legacy actor/pose linkage; a generic error still refers to a weapon | Move format/engine interpretation behind compatibility adapters; keep math and subject-neutral results portable; update consumers and exports together |
| `visual_analysis/reviews.py`, `surface_evidence.py`, `images.py` | Useful independent review/decoding services, with current montage/simulation clock and engine-frame field conventions | Normalize through versioned adapters; retain current formats as supported legacy readers and keep decoding optional |
| `visual_analysis/surfaces.py`, `pixel_alignment.py` | Reusable explicit-input measurements; detector identity/profile/file concerns share the pixel module | Preserve algorithms; separate measurement from profile loading and implementation identity when packaging |
| `review_contact.py`, `review_visual.py`, `review_surfaces.py`, `calibrate_segment_alignment.py`, `detect_segment_alignment.py` | Reusable command behavior imports sibling scripts/package paths; contact review interprets project capture records | Expose callable package services; leave thin CLIs and compatibility translation; use shared evidence/publication rather than duplicating it |
| `scenarios/*.json`, `pairs/*.json`, `visual_analysis/profiles/cyan-segment-alignment.json` | Katana assets, role/bone names, provisional criteria and a project-calibrated profile | Keep as optional Katana integration data, outside generic package defaults; migrate profile paths and all consumers together without resaving assets |
| Python `test_*.py`, native capture/contact/warp tests and scenario fixtures | Pure controls, engine controls and gameplay integration currently share local module/test placement | Separate portable, Unreal-only and Katana integration suites; preserve meaningful regression coverage and repository test naming conventions |
| Generated review/cleanup scripts and retention records under `Saved/` | Previous runs retain verified evidence but some retention orchestration is session-specific | Promote reusable manifest-driven retention into the suite; preserve historical artifacts as evidence, not required executable infrastructure |
| Guides, examples, build/test commands and repository entry points | Current usage depends on project paths and local Unreal setup | Document both standalone usage and Katana integration; provide runnable neutral examples and isolated dependency checks |

Every remaining family needs an owner and an explicit disposition. Do not treat the
new repository as permission to abandon existing mixed implementations or duplicate
them with another generic-looking implementation. Migrate consumers and tests together.
