# Animation Analysis

Shared animation observation and analysis foundation: a standalone Python package
under `Python/`, an Unreal editor plugin at this repository root, and a minimal
Unreal host under `Python/UnrealHost`. Game-specific discovery, profiles, scenarios
and gameplay assertions belong in consuming projects.

The foundation is independently buildable. The broader suite is still migrating;
see [remaining ownership and work](docs/MIGRATION.md). Async GPU readback, moving
skeletal surface sampling and mesh penetration are not implemented yet.

## Verification

```powershell
python Python/verify_distribution.py --output Saved/PythonVerification
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/NativeVerification
```

These commands retain evidence and remove their validated temporary environments.
The Python package declares its optional image dependency. The Unreal verifier
requires UE 5.6 and copies only plugin and neutral host sources.

## Unreal capture API

An editor-only UE 5.6 plugin for explicit-subject world/pose observations, bounded
PNG export and viewport surface readback. `AnimationCapture` depends on Engine,
UnrealEd and rendering/image/JSON modules. It has no project gameplay dependency,
asset content, subject discovery or skeleton defaults.

Copy this plugin into another project's `Plugins` directory, enable
`AnimationAnalysis`, and add `AnimationCapture` to the consuming module's dependencies.
The public API is under `AnimationCapture/`. This adapter is separate from the
engine-independent Python package in `Python`.

```cpp
#include "AnimationCapture/AnimationCaptureSession.h"

FAnimationCaptureSettings Settings;
Settings.Scenario = TEXT("MovingFixture");
Settings.OutputRoot = OutputDirectory; // supplied by the caller
Settings.FrameHz = 0;

FAnimationCaptureSubject Subject;
Subject.Id = TEXT("MovingPart");
Subject.Actor = Actor;
// Mesh, points and owned point-source overrides are explicit and optional.
TArray<FAnimationCaptureSubject> Subjects = {Subject};
FString Error;
FAnimationCaptureSession Session;
Session.Start(World, Settings, Subjects, Error);
// Keep the session alive while the Game/PIE world ticks.
Session.Stop(TEXT("inspection_complete"), Error);
```

All calls and extension callbacks run on the game thread. Sessions enroll weak
actor/component references once; destroyed references remain missing and never
silently switch to replacements. The recorder observes engine transforms, selected
skeletal points and montage instances. It does not evaluate missing poses or change
animation, camera, time-dilation or gameplay settings. Each recording creates a
unique directory beneath the supplied output root.

`IAnimationCaptureExtension` lets a consumer acquire its own producer resources,
collect incremental telemetry, contribute subject/session fields and export text
artifacts within the data budget. A successful `Begin` has exactly one `End` on
stop, limit exhaustion, teardown or startup manifest failure. Failed acquisition
must leave no resources owned. Extensions cannot replace existing recorder fields
or overwrite its files. Callbacks must not reenter session operations or mutate
returned JSON after handing it to the recorder. Multi-file export is not atomic;
the final manifest carries completion or error status.

The current wire format preserves legacy schema 2 names (`role`, `actors`,
`simulation_time_s`) for compatibility. `role` carries the supplied subject ID.
Clock, engine-frame and pose-finalization evidence retain their existing meaning.
Extensions supply additional producer data; the native recorder does not emit
combat CSVs or assume a specific offline evaluator. Further normalized stream and
analysis contracts remain part of the suite migration.

`FAnimationCaptureImageWriter` bounds background PNG encoding by pending frames
and bytes. Its pixel acquisition is still synchronous. `FViewportSurfaceCapture`
and `FScopedSurfaceCaptureLabels` supply the existing diagnostic surface path;
label ownership/restoration and requested viewport/frame checks remain intact.
The tested surface backend is D3D11 with the declared full-resolution/no-AA
diagnostic view policy. Surface depth readback still waits for the GPU. This
extraction adds no performance improvement, continuous skeletal surface sampling,
contact verdict or mesh penetration capability.

Verify a copied plugin in a minimal host outside this checkout:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/Logs/NativeHostVerification
```

The verifier stages plugin/host source, builds and runs native controls, preserves
small observations/logs and removes its temporary host. The host runs
`AnimationAnalysis.Capture.Portability.*`, with no gameplay modules or project assets.
Katana's compatibility session, discovery, default points, telemetry switches,
combat/warp observations and console commands remain in `KatanaCombatEditor`.
