# Bounded Unreal readback

The editor-only `AnimationCapture` module provides an opt-in asynchronous path in
addition to the synchronous compatibility APIs. See the [delivery evidence](RENDERED_READBACK_DELIVERY.md)
for measured support and the [host workflow](HOST_WORKFLOW.md) for reproduction.

## Supported acquisition

Use `FViewportAsyncCapture` for a Game/PIE viewport. Its `Request` admits RGB plus
scene depth, custom depth and stencil labels for the next draw. Call `CaptureRGB`
from `UGameViewportClient::OnViewportRendered` for RGB alone. Construct the adapter
before that draw so its scene-view extension can capture the renderer witness.
`Poll` returns terminal results without a current-frame argument. Inspect `Status`
and `bHasView`; failed, cancelled and timed-out results are not usable images.

The first backend supports UE 5.6, D3D11 SM5, one full-resolution perspective view,
no AA, single-sample BGRA8/RGBA8/RGB10A2 and D32F/S8 depth. Surface requests require
`r.CustomDepth=3` and separately owned `FScopedSurfaceCaptureLabels`. Unsupported
formats, views and frame mismatches fail explicitly. Resize/replacement closes the
adapter; create another adapter with a new caller-supplied viewport generation.

The optional diagnostic-resolution argument forces full resolution on every
enrolled view family, including idle frames, until shutdown. It defaults to false
and does not disable AA. The caller owns camera, AA, labels and scene configuration.

`FAnimationCaptureReadbackProducer` is the lower-level reusable API. Admit a copied
request on the game thread, bind the actual view on the render thread, and enqueue
each requested channel in acquisition order. Deferred commands retain tickets and
render resources, never subject/world pointers. Public headers document thread rules.

## Identity and decoding

Admission copies session/request IDs, viewport identity/generation, caller clock
and time, dimensions, and supplied subject/pose revisions. Binding adds the actual
renderer frame, view key, rectangle, projection, depth transform and renderer clocks.
The supplied time remains separate from renderer time: admission for the next draw
does not prove that a pose was finalized on that draw. A zero/missing pose witness
remains insufficient evidence. Delayed collection never inspects live subjects.

Completion and collection have separate monotonic wall timestamps. Completion
latency includes scheduling, GPU work, readiness polling and decoding; it is not a
GPU timestamp measurement. Decoder identity is
`AnimationCapture.D3D11.PackedBuffer.RGB8_RGB10.D32FS8.v1`. RGB10 conversion matches
UE's screenshot integer requantization. RGB8 bypasses sRGB sampling conversion.
Depth and labels are packed into fixed GPU buffers before readback, avoiding any
assumption that the depth format's logical size describes physical staging pitch.

## Bounds and lifetime

Default producer limits are four pending requests, 64 MiB reserved payload capacity,
2,073,600 pixels per request and a five-second completion deadline. The byte bound
can reject a request below the pixel ceiling. Reservations include packed output,
staging and decoded arrays: 12 bytes/pixel for RGB, 41 for depth, 53 for both.
Identity text and pose counts are separately bounded. Driver/engine allocation
overhead and existing renderer textures are outside these payload counters.

Uncollected results keep their reservations. Cancellation/timeout produces one
terminal result while submitted resources stay charged until GPU retirement.
`Pump` schedules at most one render poll command and only locks ready readbacks.
After collection, callers own and must bound their arrays and downstream queues.

`Shutdown(true)` cancels pending logical requests; `Shutdown(false)` drains fully
submitted observations and cancels missing channels. When resources remain in flight,
teardown explicitly submits and waits for GPU idle, refreshes D3D11 fence events and
flushes its render command; `ShutdownWallSeconds` reports that device-wide cost.
This avoids UE 5.6 D3D11's per-fence wait stall when another owner's fence precedes
the requested one in the shared fence queue.
There is no per-frame GPU-idle wait or render flush in the asynchronous path.
Device hangs remain subject to UE's fence/device-failure policy; the host launcher
adds an external process deadline and preserves unsuccessful evidence.

## Session compatibility

`FAnimationCaptureSettings::bUseAsyncReadback` defaults to false. Set it true to use
the shared producer for session PNG acquisition. Set
`bUseAsyncDiagnosticResolution=true` separately when full-resolution diagnostic
rendering is intended. PNG encoding remains the existing bounded background writer.
The session bounds acquisition plus export to four outstanding frames and 64 MiB
of payload reservations, with file reservations charged against `MaxDataBytes`.

Legacy schema 2 fields retain acquisition meaning: `engine_frame`, sample linkage,
pose witnesses and `wall_elapsed_s` are frozen before readback. Async frames add
`readback_*` completion/collection/decoder fields and `renderer_*` view evidence.
`readback_wall_s` measures the acquisition API call; for async mode this is enqueue
time, also named `readback_enqueue_wall_s`, not completion latency. Manifests declare
mode, diagnostic policy, queue peaks, losses and shutdown cost. `readbacks.jsonl`
records one terminal readback event per admitted request when export succeeds,
including requests without a PNG. Data-budget or stream-write failures mark evidence
incomplete and can leave event gaps. Image export success remains a separate outcome
in `frames.jsonl`/manifest.

`FViewportSurfaceCapture` keeps its synchronous behavior. New surface clients opt
into `FViewportAsyncCapture`; same-frame `Collect` callers need an explicit migration
to delayed `Poll`. SURFACE1 and surface JSON schema 1 remain readable by the existing
Python decoder; new captures must retain the new decoder identity and recalibrate
any identity-sensitive consumer profiles. No Python analysis algorithm is changed.

Consumers must rebuild the native module and copy `Shaders/` with `Source/` and the
plugin descriptor. `AnimationCapture` loads at `PostConfigInit` to register shaders;
restart the editor after module/shader changes. Katana's owner selects the candidate
revision, updates its dependency, rebuilds, and verifies affected integration tests.
Pixels/depth do not establish physical contact, artistic quality or mesh penetration.
