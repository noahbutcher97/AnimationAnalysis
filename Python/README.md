# Animation analysis

An installable Python package for geometry, raster measurements and evidence-linked visual review. Core operations use the standard library and have no Unreal runtime, game project or AI-tool dependency. Producer-specific formats are handled by explicit adapters.

## Install and run

Python 3.11 or later:

```sh
python -m pip install .
python -m pip install '.[images]'
animation-review --evidence observations.html --findings findings.json --output review.html
animation-surface-review --help
animation-segment-calibrate --help
animation-segment-detect --help
```

The `images` extra is needed only for image decoding and RGB/label publication. Installation does not include project profiles, assets or scenario runners. The four commands are also callable services under `animation_analysis.jobs` and can run from any working directory with explicit input/output paths.

## Contracts and APIs

```python
from animation_analysis import ClockStamp, PoseKey, Projection, project_point, require_same_pose
from animation_analysis.surfaces import measure_surface_relation

acquired = ClockStamp("inspection_simulation", 1.25)
later = ClockStamp("inspection_simulation", 1.30)
elapsed = later.delta_seconds(acquired)
pose = PoseKey("moving_part", "inspection_camera", frame_id=42, revision=7)
require_same_pose(pose, PoseKey("moving_part", "inspection_camera", 42, 7))
```

Clocks cannot be subtracted across different domains. Pose keys include subject, stream, acquisition frame and revision; completion time does not replace acquisition identity. Projection uses row-major matrix storage with explicit row/column multiplication and NDC vertical direction. World points and matrices must use the same caller-declared coordinate system. Geometry does not select a skeleton, role or world unit.

Canonical visual evidence uses `schema_version: 2` and frames with unique `file`, `time: {"domain": "inspection_simulation", "seconds": 1.25}`, embedded PNG `image` and `image_sha256`. It is carried in one HTML `<script id="visual-evidence" type="application/json">` block. `animation_analysis.load_evidence` validates canonical records. Reviewed findings retain schema 1, explicit reviewer provenance and `consistent`, `concern`, `indeterminate` or `not_reviewed` assessments.

`animation_analysis.adapters.legacy_evidence` reads older montage/simulation-clock evidence and normalizes it in memory, preserving the original file hash and bytes. The command services accept this compatibility format and canonical evidence. `adapters.unreal_capture` handles existing row-vector projection/pose records; `adapters.unreal_surface` handles bounded `SURFACE1` raster bundles. These adapters do not infer missing clock relationships, hidden geometry or unsupported deformation.

`surfaces.measure_surface_relation` and `pixel_alignment.analyze_segment` accept explicit raster data and criteria. Images are decoded through the optional `images` module. Pixel/depth measurements and authored review records are separate from physical-contact or artistic acceptance. Passing a control establishes only the conditions it exercised.

Shared offline services are available in version 0.2.0:

- `integrity`: strict JSON/JSONL and finite numeric input, contained paths, RGB/RGBA PNG checks including CRCs and bounded decompressed scanlines. These checks need no image extra. Embedded visual evidence retains its separate signature/hash validation contract.
- `artifacts`: SHA256, canonical JSON identity, explicitly selected file manifests and atomic UTF-8/JSON replacement. Manifests use relative paths and reject escape/duplicate entries. `implementation_manifest()` identifies installed package Python sources; callers also identify external backends, profiles and producer code. Atomicity is per file; multi-file jobs retain their pending-state protocol.
- `metrics`: finite numeric summaries and nested deltas, plus explicit status precedence. The caller selects compatible metrics, units and thresholds. Boolean flags are not numeric observations. A pass applies only to executed cases; consumers must retain the complete case list.
- `temporal`: clock-declared `TimeInterval`, unique named event windows and complete observation bracketing. Callers translate their own records into `ClockStamp`; missing coverage, unrelated clocks and unordered observations reject. Duplicate acquisition times remain available for the caller's identity/cadence checks.

Version **0.3.0** adds `mesh_records` and `mesh_replay`: immutable component/topology
and acquisition identities, explicit feature eligibility, bounded binary replay and
exclusive bundle publication. Their public types/functions are also exported from
`animation_analysis`. Supply caller limits, units, coordinates and producer coverage;
no component discovery or native skeletal sampling is implied. The separate mesh
schema is version 1; existing visual/motion/RGB/depth formats remain compatible.
See [mesh contracts, binary layout and runnable example](../docs/MESH_OBSERVATIONS.md).

```python
from animation_analysis.temporal import TimeInterval, bracket_observations
from animation_analysis.artifacts import file_manifest, identity

interval = TimeInterval(ClockStamp("sensor", 1), ClockStamp("sensor", 2))
rows = [{"time": 0.9}, {"time": 1.5}, {"time": 2.1}]
selected = bracket_observations(rows, interval, lambda row: ClockStamp("sensor", row["time"]))
# The caller selects identity-bearing input files, excluding derived outputs.
# observation_identity = identity(file_manifest(input_directory, ["observations.jsonl"]))
```

## Verification and extraction scope

After installation:

```sh
python -m unittest discover -s tests -v
```

The package contains the migrated Python geometry/raster/pixel/review services, shared offline integrity/identity/metric/interval/publication services and four command services. Native capture lives in the separate `AnimationCapture` Unreal plugin; `verify_unreal_host.py` exercises a copied plugin in an independent host. Legacy stream analysis, project-specific evaluation/launching, paired preview integration and generalized retention remain migration work. This Python package does not implement continuous skeletal sampling or full mesh penetration.

The source tree is independently buildable using the standard `pyproject.toml` packaging contract. Core import/testing must also succeed without the image extra. Optional image commands require their declared extra. Do not silently reuse a calibration after a measurement/decoder implementation or profile identity changes.
