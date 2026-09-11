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

## Delivered implementation and verification

Verified implementation candidate: **`89d3f27c451dee746f0ae39514b4c5ae079ac26d`**.
The preceding host/baseline slice is **`644cb7d`**. Both are local commits on
`feature/bounded-rendered-readback`; nothing was published. The existing AGENTS.md
was preserved. Documentation following the candidate does not alter native code.

The reusable producer packs fixed GPU buffers, polls readiness without waiting,
preserves caller and actual renderer witnesses separately, and bounds requests,
staging, decoded results and session PNG output. Session RGB and the new surface
adapter share that producer. Synchronous APIs and the session default remain supported.
[API/compatibility details](ASYNC_READBACK.md) describe supported formats, opt-ins,
payload accounting and the explicit teardown wait.

Final native command, from the repository root:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/Delivery-20260911/final-native-02 --rendered
python Python/verify_distribution.py --output Saved/Delivery-20260911/final-python
python -m unittest discover -s Python/tooling_tests -v
```

Use fresh output names when reproducing. Final native verification built an isolated
Engine/plugin-only copy outside the checkout, passed **all 16 exact controls**,
verified its replay archive, and removed the temporary host. Its 29 staged source
hashes match the committed implementation; `implementation-verification.json`
records that comparison. No headless skip was counted as a rendered pass.

| Controls | Evidence established |
|---|---|
| Five `Portability` controls | Independent session/extension lifecycle, admission, immutable requests, byte limits, missing-draw timeout, stale-ticket clearing and rejection counts |
| Two `Surfaces` controls | Original label ownership and five synchronous geometry cases remain valid |
| Two `Rendered` controls | sRGB BGRA8 bytes preserved; all 1,024 RGB10 codes match UE D3D11 screenshot conversion |
| `Async.RenderedGeometry`, `DelayedIdentity` | Same depth/gap/IoU criteria; eight-draw delayed polling with caller mutation and destroyed subject preserves acquisition identity |
| `Async.RenderedLifecycle`, `WorldCleanup` | Queue saturation, cancellation, actual resize/replacement, repeated ownership, label restoration and PIE destruction |
| `Async.SessionRGB`, `SessionCleanup` | Concurrent recorders, moving/destroyed/replaced fixed enrollment, restart, PNG export, separate completion fields, exactly-once logical termination and extension cleanup |
| `Host.InteractiveCommands` | Actual registered Inspect/Stop routes, five controls over real draws, invalid indices, restart and scene/view restoration on stop/PIE teardown |

All **22 repository-tooling tests** pass. Installed-package isolation again passes
24 core tests plus one optional-image skip without Pillow, and all **25** with the
image extra; four CLI entry points and 11 commands were exercised. The portable
Python algorithms and calibration-sensitive files are unchanged. Final wheel SHA-256:
`758aaf2debd9108c56ba4b743b81af3643d86fa89a337aa28a37b35ee06f4150`.

Offline replay loaded the final five async controls, five synchronous controls and
one delayed observation. Their unchanged Python reader/measurement services produced
HTML/JSON reviews. All **24 session PNGs** fully decoded; a representative async PNG
was visually inspected. The async controls retained IoU **1.0** for the first four
layouts, front-plane depth within 1 cm, gaps **66/1/1**, and unknown occluded evidence.
Console inspection was exercised through automation; no manual editor UI inspection
or consumer gameplay run is claimed.

## Comparable performance

Two separate editor runs each executed three repetitions of disabled, synchronous
and asynchronous modes: **2,160 measured attempts**, including **720 async observations**.
Each repetition had 60 warmup draws and 120 measured attempts per mode, rotating
mode order. All modes used one admission attempt per viewport draw and the same
host view-policy extension, including idle draws. Both reports are comparison-eligible:
zero view-policy divergences, idle sampled draws, rejected requests, cancellations,
failures or timeouts. Raw outcomes include warmup losses rather than hiding them.

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/DevelopmentHost --output Saved/Delivery-20260911/readback-comparison-01 --run-tests --rendered --test AnimationAnalysis.Capture.Performance.ReadbackComparison
```

Repeat with a new output directory. `host-commands.json` records exact invocations.
The view extension enforced 640x480/full resolution although the inherited
`r.ScreenPercentage` CVar was 50. Renderer witnesses verified the effective policy.
VSync was 0, `t.MaxFPS` 0, one-frame thread lag 1, dynamic resolution 0; PIE scheduling
still held the disabled/async workload near 60 Hz.

The following cells are **ranges of per-repetition percentiles across all six
repetitions**, not pooled percentiles. Times are milliseconds.

| Mode | Frame p50 | Frame p95 | Frame p99 | Acquisition p50 | Completion p50 |
|---|---:|---:|---:|---:|---:|
| Disabled | 16.658–16.672 | 16.800–16.873 | 16.871–17.379 | <0.001 | n/a |
| Synchronous | 20.385–21.495 | 21.702–27.502 | 23.142–33.139 | 15.848–16.439 | 20.464–21.508 |
| Asynchronous | 16.654–16.668 | 16.799–16.935 | 16.929–17.599 | 0.007–0.009 | 38.340–38.762 |

Async acquisition p95/p99 ranges were **0.009–0.012 / 0.010–0.133 ms**; completion
p95/p99 were **38.866–39.779 / 39.134–40.720 ms**. Synchronous acquisition p95/p99
were **16.796–22.137 / 17.761–23.628 ms**; completion p95/p99 were
**21.625–27.139 / 22.567–34.295 ms**. Async median CPU decoding was approximately
**1.56–1.70 ms** on the render thread. Full per-run values remain in `summary.json`.

The paired median frame interval fell **18.3–22.5%** in this fixture while observations
became available later. This supports reduced acquisition stalls on this workload,
not faster end-to-end observation delivery or an uncapped throughput claim.
Acquisition includes game-thread request and post-draw submission/collection work;
frame intervals include engine scheduling and rendering. Completion includes
admission-to-draw delay, GPU work, polling and decoding. GPU timestamp duration,
synchronous decode alone and synchronous allocation peaks were not instrumented.
PNG encoding/file export were excluded from these timed windows and tested separately.

Sampled async occupancy reached three requests; reserved-capacity high water was
**65,126,400 bytes (62.11 MiB)**, corresponding to four combined 640x480 requests,
within the 64 MiB per-producer limit. All requests/bytes returned to zero after drain.
Final session controls peaked at **11,255,808 bytes (10.73 MiB)** across readback/PNG
reservations. Their readback shutdown took **6.16–6.96 ms**; world cleanup cancelled
one admitted request per session and explicitly marked incomplete captures as errors.
The already-drained async performance owners took **0.59–1.21 ms** to shut down.

## Failures resolved and replay retention

Retained unsuccessful runs document unsupported RGB10 viewport format, an implicit
PIE scaling policy, and a reproducible two-session D3D11 teardown stall. The stall
was localized to an unready per-fence wait. Teardown now uses an explicit measured
GPU-wide drain and refreshes cached fence events; normal polling still never waits.
The launcher also retains bounded file-read retries after confirmed timeout cleanup.
The earlier locked timeout artifacts were recovered and verified separately.

Final admission counts producer-held requests until GPU retirement, including
collected cancellation/timeout results, when enforcing the combined session bound.
Queue/timeout controls verify retained producer reservations; the final session
tests verify ordinary combined bounds. A session-specific GPU timeout plus pending
PNG was reviewed statically, not induced in a live test.

Evidence is under `Saved/Delivery-20260911/`; source fixtures, reports and workflows
are tracked, while captures/builds remain ignored. Important SHA-256 identities:

| Artifact | SHA-256 |
|---|---|
| `final-native-02/replay-evidence.zip` (92 entries) | `60dcae07c28f5984c60ed9e2df53f454a974f2860d6b88c6b7403edb3907cd92` |
| Comparison 01 raw `performance.json` | `7d4f652e9f8fc0976bfeddd562d3e58158e656523144a0f9ca67cdab7afbe79a` |
| Comparison 02 raw `performance.json` | `8e47fa24631781e74db46e7f8bc356407de5ab6c320ad5a287f4440eab54a166` |
| `cleanup/final-review-replay.zip` (7 review entries) | `b53295571e3f33b226ffacd0ed82b7d028e0ede2289ce62ed130037be9e4bfdf` |
| `cleanup/retained-host-replay.zip` (148 entries) | `285fe10a9f057fb44622c82792aebb01bb6e2e90aefc4774d0c3404c78092ed8` |
| `cleanup/cleanup-report.json` | `4952c55adfc7bbe99de214b78be62f9c52a88e9eb99e059f46a9b18b9c39c915` |

Final cleanup reverified 12 original archives, created and verified seven explicit
replay inventories/archives, then removed **103 inventoried PNGs** from this delivery's
loose outputs and retained host observations. It reverified archives after deletion;
no PNGs remain in those scoped trees. The baseline contact-sheet PNG was separately
archived/verified and removed earlier. Raw surface bundles, metadata, reviews and
archives remain available. `finalize_replay.py` and `cleanup/cleanup-report.json`
retain exact cleanup paths and hashes. Restore selected archive entries to a fresh
replay directory when images are needed; do not overwrite historical evidence.

## Consumer handoff and remaining limits

Katana's owner receives candidate **`89d3f27`**, updates its pin/setup, copies shader
resources, restarts/rebuilds its editor, and runs dependency plus affected native,
rendered surface, recorder, finisher/interruption and offline replay checks. Default
synchronous callers need no async migration. Async adopters must choose the view
policy explicitly, handle delayed terminal results and read the additive completion
metadata. Decoder-sensitive profiles require fresh identity/calibration checks.

This delivery establishes standalone integration readiness. Katana assets, adapters,
dependency pins and runtime compatibility were not changed or reverified here.
D3D12/Vulkan, AA/upscaling, multiple views, HDR formats outside the listed support,
moving skeletal surface sampling, continuous intersection/penetration, and artistic
criteria remain unverified or outside scope. Teardown may stall the whole GPU and
device failure follows UE's policy; payload bounds exclude driver overhead and
caller-owned arrays after collection. The remaining suite migration is unchanged.
