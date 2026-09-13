# Neutral assembly qualification design

## Scope

Deliver one runnable noncombat capture-to-analysis example and bounded offline
workload measurements. Reuse mesh replay schema 1 and Python 0.4.0 analysis APIs.
All changes stay in AnimationAnalysis; Katana owns consumer qualification and pins.
No deformation extension, containment, engine patch, push or merge to main is included.
Python >=3.11 with no new package dependency. UE 5.6.1/D3D11 native qualification.

The user approved the neutral end-to-end workflow, difficult workloads and runnable
example. Implementation exposed a concrete prerequisite: separate reference captures
stamp different monotonic times, while paired analysis requires a shared acquisition.
Qualify synchronous rigid batch acquisition rather than relabel exported records.

## Acquisition

Add `FAnimationCaptureMeshReference::CaptureRigidBatch(Samplers, RequestId,
MaxComponents, OutSnapshots, Error)`. Samplers are an explicit `TConstArrayView` of
reference-observer pointers; outputs are immutable snapshots in supplied order.
`MaxComponents` is an explicit positive bound no larger than 64. Enforce it before
copying or reserving output. Each enrolled mesh must be a different registered
ordinary unparented static component, in the same world, outside world tick, without
physics simulation. Skeletal and attached cases remain unavailable for this operation.

Capture one monotonic acquisition stamp and engine frame at the start of the
validated synchronous game-thread operation. All components' game-thread state is
stable during that operation: it must not tick, yield, invoke caller callbacks or
drive component state. Pass the common stamp into private preparation before it
constructs observations; never edit already published snapshots or replay records.
Keep each actual completion time and observer revision. Preserve all existing
single-capture/GPU preparation behavior. Same-world/frame validity must still hold
before publication. Reject duplicate observers/components and invalid/retired inputs.
On any failure release this call's partial results and return no output. Existing
per-observer and optional shared budgets continue to own every result reservation.

Native replay serialization must round-trip finite double values, including clocks
and transforms. Existing six-decimal `LexToString(double)` output was found to lose
the shared acquisition identity. Correct the writer rather than rounding manifests
or changing loaded records. Schema 1 remains compatible; new record byte hashes
change. Qualify exact round trips and the existing canonical topology control.

## Native assembly

Use two independent actors with ordinary unparented static cube components in the
neutral host. Use `/Engine/EngineMeshes/Cube.Cube`: its 24 vertices have coordinates
of exactly +/-128 cm and its 36 indices form 12 triangles. The explicit uniform
scale `25/64` (`0.390625`) is exactly representable and produces 100 cm sides.
Verify those source and transformed bounds. Use no physics or collision and integer
translations. Fixed center `(0,0,10000)`; moving center has X
positions `[140,110,100,80,140]` with matching Y/Z. Whole-mesh regions contain all
12 triangles. These are fixture-owned actors; no consumer assets or scene edits.
Sample IDs `step-00` through `step-04`. Expected distances `[40,10,0,0,40]` cm and
surface intersections `[false,false,true,true,false]` are supplied independently
in the tracked example criteria. No sample asserts containment or physical contact.

Sample across latent automation updates, at least 0.1 seconds apart. The expected
maximum acquisition gap is 1.0 seconds, fixed before runs; retain slower failed runs.
The test `AnimationAnalysis.Capture.Mesh.RigidAssembly` exports a unique
`Saved/Observations/RigidAssembly-<guid>` directory. Add it to default rendered tests.
Preserve replay after fixture retirement. Verify admission failure leaves zero
partial results/reservations, output retention charges capacity, shared timestamps,
separate completion, unsupported/duplicate/over-limit inputs, and unchanged single
capture behavior. Keep batch boundary controls in this fixture/test translation unit.

`assembly.json` format `neutral_assembly_capture`, schema_version 1, has exactly
`format`, `schema_version`, `roles`, `samples`, `controls`. Each role (`fixed`,
`moving`) supplies `component_id`, `component_generation`, `configuration_id`,
`subject_id`, `stream_id`, `region_id`, `topology_id`, `triangle_ids`. Each sample
supplies `sample_id`, `fixed_bundle`, `moving_bundle`, `acquired_seconds`, `frame_id`,
`fixed_revision`, `moving_revision`. Bundle names are `step-00-fixed` etc; each native
request ID is its shared sample ID. Acquired clock is `unreal-monotonic` throughout.
`controls` has exactly `checks_passed` (the accumulated native assertion outcome)
and `peak_reserved_bytes` (nonnegative integer). Detailed boundary assertions stay
in the native test log; no unknown renderer behavior is claimed. Publish the manifest after all five pairs are written.
The file is at most 64 KiB. Retain native controls/logs if publication fails.

## Installed consumer example

Track `Python/examples/neutral_assembly.py` and `neutral_assembly_criteria.json`.
The script imports the installed `animation_analysis` package normally, with no
checkout-path injection. CLI requires `--observations`, `--criteria`, `--output`.
Its callable `evaluate_run(observations, criteria)` returns a detached JSON mapping;
the CLI exclusively creates its output and returns nonzero for failed qualification.
Bound both input JSON files to 64 KiB and reject duplicate/nonfinite/unknown fields.
Use schema-1 replay validation for every explicitly named bundle; no directory scan
or participant discovery supplies records. Actual completion envelopes stay in the
report separately from paired acquisition identity.

The criteria explicitly bind units/convention, rigid/pose coverage, accepted
exclusions, producer, record limits, analysis limits, maximum samples/gap, tolerance,
and the five independent expected outcomes. Role configuration/pose/region identity
comes from the run's explicit declarations, not silently from whatever record loads.
This named fixture qualification accepts only the fixed semantic criteria profile
listed in the plan and shipped criteria file. It must reject shortened sample lists
or altered units, feature/producer policy, limits, gap, tolerance or outcomes instead
of calling a weakened profile verified. JSON formatting and set ordering need not be
identical. General consumer-specific criteria remain supported by the public APIs;
this example is a reproducible qualification of its documented five-sample fixture.
The report contains individual results, completion provenance, interval summary,
input hashes, expected-versus-observed outcomes, read errors and labelled controls.
Missing bundles/samples leave the run insufficient even if surviving samples meet
the interval gap threshold. Wrong source identities and required missing effects
must not produce measured values. Unknown required evidence stays unknown.

Deliberate controls use detached in-memory variants, labelled as controls and never
published as native captures: wrong topology/pose/clock, required excluded cloth,
exhausted search, missing endpoint and excessive acquisition gap. Preserve original
input records unchanged. The primary run uses only original native observations.

## Offline workloads

Track `Python/examples/benchmark_mesh_analysis.py`, with no geometry optimization
in this slice. Fixed counts 32, 128 and 512 triangles per side, one warmup and three
repetitions with rotated execution order, not rotated geometry. The base order is
case-major in the case order below, with counts ascending within each case. Run
one warmup of that nine-case sequence; repetition `r` (0, 1, 2) rotates the sequence
left by `3*r` entries. Retain warmup results separately from measured aggregates.
Limits: 2048 triangle tests, 8192 node visits, 512 triangles
per region, 256 coordinate bits. Time includes preparation, hashing, transforms,
validation, tree construction and search; excludes fixture construction/export.

Cases use deterministic repeated triangles with independent exact expectations:

- Overlapping boxes, disjoint surfaces: A `(0,0,0),(2,0,0),(0,2,0)`;
  B `(2,2,0),(3,2,0),(2,3,0)`. Exact squared distance 2.
- Long thin triangles with touching boxes: A `(0,0,0),(1024,0,0),(0,1/1024,0)`;
  B `(1024,1/1024,0),(2048,1/1024,0),(1024,2/1024,0)`.
  Exact squared distance `1048576/1099511627777`.
- Near parallel surfaces: copies of `(0,0,0),(2,0,0),(0,2,0)` translated by
  `1/1048576` in Z. Exact squared distance `1/1099511627776` and no intersection.

Independent expectations: the first case's closest points are `(1,1,0)` and
`(2,2,0)`. For the thin case, A's separating edge is `x/1024 + 1024*y = 1`;
B's nearest vertex gives residual 1, so squared perpendicular distance is
`1/(2^-20 + 2^20)`. Its projection lies inside A's edge and B's other points are
farther into the same half-space. The parallel case has coincident XY projections,
so the plane gap is attained. Repeating a triangle changes work, not these distances.

Report all raw timings/status/counters and expected values; exhausted cases publish
no minimum. Report medians/ranges per case and size separately for completed versus
budget-exhausted work. Record interpreter/platform, exact parameters and input/source
hashes. These are deliberately narrow stress controls, not representative game-mesh
or worst-case wall-time guarantees. Archive results and reproducible inputs/scripts.

## Acceptance and delivery

Pass meaningful example/benchmark tooling controls, core tests, distribution
isolation, a fresh independent native host build and relevant native controls.
Run the example against fresh native replay with a clean installed wheel, including
after verified archive restoration. Record numerical agreement, actual gaps, budgets,
timings, exact commits, commands and limitations. Review task boundaries and the final
branch independently. Archive/hash all replay evidence before any generated-image
cleanup. No images are required for this rigid geometry example. Preserve existing
archives and WIP. Commit verified slices and leave the branch ready for review.
