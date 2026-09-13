# Neutral assembly example

This noncombat example measures the surfaces of two rigid parts through native
capture, schema-1 replay and the installed Python analysis APIs. It uses two 100 cm
cubes with independently enrolled components and explicit whole-surface regions.
The fixture owns its actors and motion; it does not change consumer assets.

## Capture a known assembly

From the repository root, use a fresh evidence directory:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/AssemblyHost --output Saved/AssemblyNative-01 --run-tests --rendered --test AnimationAnalysis.Capture.Mesh.RigidAssembly
```

The verifier stages an independent host, builds it and checks the exact native
automation result. It retains the new `observations/RigidAssembly-<run-id>` directory,
source identities, logs and a verified replay archive beneath `Saved/AssemblyNative-01`.
Use the explicit run directory recorded in that invocation's replay inventory.

The fixture uses `/Engine/EngineMeshes/Cube.Cube`, a 256 cm, twelve-triangle cube,
at the exactly representable uniform scale `25/64` to produce 100 cm sides.
The fixed cube stays at `(0,0,10000)` cm. The moving cube visits X coordinates
140, 110, 100, 80 and 140 cm. The five independently specified expected surface
distances are 40, 10, 0, 0 and 40 cm; touching and crossing both have surface
intersection. No containment, penetration depth or physical-contact result is implied.

`CaptureRigidBatch` captures both parts in one synchronous game-thread operation
with a shared native acquisition stamp. It keeps separate completion times and
observer revisions. The operation qualifies ordinary unparented static components
in the same world, outside world tick, without physics simulation. It has an
explicit component-count bound and preserves existing snapshot/shared budgets.
Failure returns no partial output. Existing single-capture and GPU paths remain
separate; sequential calls do not implicitly acquire a shared time.
Fresh replay exports preserve double round-trip precision for clocks and transforms.
Older records remain readable at their recorded precision; do not round or restamp
them to force a match with an independently supplied acquisition identity.

## Analyze explicit replay inputs

Install the portable package in an isolated environment:

```powershell
python -m venv Saved/AssemblyPython
Saved/AssemblyPython/Scripts/python.exe -m pip install ./Python
```

Replace `<run-id>` with the retained directory from the capture above, then run:

```powershell
Saved/AssemblyPython/Scripts/python.exe -I Python/examples/neutral_assembly.py --observations Saved/AssemblyNative-01/observations/RigidAssembly-<run-id> --criteria Python/examples/neutral_assembly_criteria.json --output Saved/assembly-report.json
```

The script uses the installed `animation_analysis` package, with no source-path
injection. It reads only the manifest's explicitly named replay bundles. The
criteria file supplies units, producer/coverage requirements, work limits, tolerance,
sample-gap limit and independent expected outcomes. Both JSON inputs are bounded;
the output must not already exist. A failed qualification returns a nonzero exit.
This named example requires the shipped five-sample criteria profile semantically;
shortening the sequence or changing its acceptance values cannot produce a verified
demo. Formatting and set ordering may differ. For another workflow, supply its
criteria through the public analysis APIs in a consumer adapter.

The report retains each measurement, original acquisition and completion provenance,
interval gaps, expected/observed comparisons, errors and input hashes. Missing bundles
make the run insufficient even if surviving samples meet the maximum-gap criterion.
The native roles bind component, configuration, topology, region and pose identities;
loading a different record cannot silently change those requirements.

Labelled negative controls operate on detached in-memory variants and retain their
own outcomes. They exercise wrong identities, required excluded coverage, exhausted
work and incomplete intervals. They never modify the native replay inputs or replace
failed primary observations with successful controls.

## Adapt the analysis to another workflow

Use the public `MeshRequirement`, `MeshRegion` and `measure_mesh_pair` APIs in a
consumer adapter to supply different coverage, regions, tolerances and work limits.
Keep its acceptance criteria explicit and independent of the observations being measured.

This is explicitly a rigid geometric reference. Native coverage retains its exclusions
for morphs, cloth, mesh deformers, material displacement and raster visibility.
Accepting these exclusions allows this narrower geometry; it does not prove that
omitted effects are absent. Required unsupported effects must remain insufficient.

The shipped region mappings contain all twelve triangles of each cube and are tied
to their topology. Real consumers provide their own surface intent and mappings.
Changing topology or LOD requires a new mapping. Never reinterpret a material section
as anatomy or loosen an acceptance threshold merely to obtain a passing result.

Samples are separated by at least 0.1 seconds and the example permits no gap over
1.0 seconds. These are qualification settings for this fixture, not application
recommendations. A complete interval describes the supplied samples only; behavior
between them remains not evaluated. The exact rational analysis is an offline path.

## Measure difficult offline layouts

```powershell
Saved/AssemblyPython/Scripts/python.exe -I Python/examples/benchmark_mesh_analysis.py --output Saved/AssemblyBenchmark-01
```

The benchmark uses fixed sizes, shapes and budgets, one warmup and three repetitions
with rotated execution order.
Its disjoint overlapping-box and thin-triangle cases deliberately frustrate pruning;
a near-parallel case checks a small positive gap. Reports separate completed
measurements from budget-exhausted attempts, which have no numeric verdict.
Timing includes hashing/preparation and exact search, excluding input creation and
export. These controlled workloads do not certify worst-case wall time or real-time use.

Keep the replay inventory and archive with the reports. Verify archived entry hashes
before removing any generated images needed for replay. This assembly fixture requires
no image export. Consumer-owned live pose/deformation qualification remains separate.
