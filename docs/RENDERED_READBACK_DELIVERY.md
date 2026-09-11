# Rendered readback delivery evidence

Recorded 2026-09-11. This report tracks the first scoped delivery from
[the development handoff](DEVELOPMENT_HANDOFF.md). Katana integration and dependency
updates remain owned by its developer. No consumer files or assets were changed.

## Synchronous baseline, before acquisition changes

The neutral host now shares its camera/cube fixture between automation and
`AnimationAnalysis.Host.Inspect`. See [host commands](HOST_WORKFLOW.md).
The synchronous capture implementation remained at the extraction baseline while
these measurements were recorded.

Environment: Windows, UE **5.6.1**, build **44394996**, D3D11, NVIDIA GeForce RTX 5090
Laptop GPU (driver **32.0.16.1074**), one perspective view, unlit engine cubes against
black, no AA, diagnostic full resolution **640x480**. The run retained the engine log
with the chosen adapter and source hashes. No other Unreal editor was running at
launch. Initial shader compilation finished before fixture warmup.

Evidence root: `Saved/Delivery-20260911/sync-baseline/`. Exact renderer invocation is
in `host-commands.json`; source SHA-256 inventory is `host-source.json`. Five exact
native test results succeeded: `Portability.IndependentSessions`,
`Portability.ExtensionIntegrity`, `Surfaces.LabelOwnership`,
`Surfaces.RenderedGeometry`, `Surfaces.Performance` under `AnimationAnalysis.Capture`.

The five geometry controls retain the 350 cm front plane (1 cm tolerance),
pixel-centre gaps **66, 1, 1**, absent fully occluded label, and zero visible pixels
for the scene-occluded second label. RGB/combined-label silhouette IoU was **1.0**
for the first four controls against the unchanged **0.99** floor. The ordinary
occluder is correctly outside that combined-label outline assertion. All five RGB
controls were decoded and visually inspected; offline surface replay completed.

Three repetitions used 60 warmup updates and 120 measured samples per mode. Cadence
was one request per automation update; timed samples excluded encoding and file
export. Frame intervals include engine scheduling and the previous draw's capture.
The viewport was effectively capped near 60 Hz; these are workload measurements,
not maximum-throughput results.

| Mode/repetition | Frame p50/p95/p99 ms | Acquisition p50/p95/p99 ms |
|---|---|---|
| Disabled 0 | 16.65 / 17.45 / 19.00 | <0.001 / <0.001 / <0.001 |
| Disabled 1 | 16.65 / 17.64 / 18.53 | <0.001 / <0.001 / <0.001 |
| Disabled 2 | 16.64 / 17.23 / 18.01 | <0.001 / <0.001 / <0.001 |
| Synchronous 0 | 23.45 / 26.97 / 33.70 | 17.79 / 19.79 / 22.72 |
| Synchronous 1 | 23.51 / 27.62 / 29.45 | 17.79 / 20.82 / 23.11 |
| Synchronous 2 | 21.21 / 24.35 / 25.73 | 16.66 / 18.76 / 20.63 |

Raw samples: `observations/Performance-071644444ED366E27F6463ADEF4FF8E7/performance.json`,
SHA-256 `65e778ff7ce3971be76a5f170b29bae56aa5f269e45f87f9ac5ee55656d12e09`.
`Python/summarize_readback_performance.py` uses nearest-rank percentiles and rejects
incomplete repetitions, duplicated samples and nonfinite timing data.

`replay-evidence.zip` contains 28 verified replay entries. Archive SHA-256:
`19596efbd58c9d26e1f46ec433ec9025cf419539cc53053f72f22e223e1b22d4`.
`replay-inventory.json` and `replay-retention.json` record entry hashes. No generated
PNGs were present in that original observation inventory. Subsequent review output
has its own retention inventory.

Initial installed-package isolation passed: 24 core tests plus one optional-image
skip without Pillow; all 25 tests passed with the image extra. Two native lifecycle
tests passed both before and after adding the host fixture. Evidence is under
`initial-python/`, `initial-native/` and `fixture-build/` alongside the baseline.

## Delivery status

The rendered host and synchronous baseline are verified. Asynchronous acquisition,
its lifecycle controls and comparable performance results are being implemented;
this baseline alone establishes no asynchronous or Katana runtime claim.
