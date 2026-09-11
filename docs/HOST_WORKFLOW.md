# Neutral host workflow

The tracked `Python/UnrealHost` project supplies the same neutral fixtures to
automation and interactive inspection. `Python/verify_unreal_host.py` stages it
with this plugin, using one source allowlist for both temporary verification and
retained development copies. It requires an explicit UE 5.6 engine location.
No consuming project, gameplay module, project asset or local tool installation
is discovered or copied.

## Isolated verification

Run from the repository root and give each invocation a new evidence directory:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/HostChecks/nullrhi-01
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/HostChecks/rendered-01 --rendered
```

The first command uses NullRHI. The second uses a real D3D11 renderer at the
declared 640x480 window size; the fixture applies and records its diagnostic view
policy. Run rendered checks when other editor/GPU workloads are idle.

| Mode | Exact expected automation results |
| --- | --- |
| NullRHI | Five `AnimationAnalysis.Capture.Portability` controls: `IndependentSessions`, `ExtensionIntegrity`, `ReadbackAdmission`, `ReadbackByteLimit`, `ReadbackTimeout` |
| Rendered | All five above, two `Surfaces` controls, two `Rendered` RGB decoder controls, six `Async` geometry/lifecycle/session controls, and `Host.InteractiveCommands` (sixteen total) |

Every expected test must complete exactly once with `Success`. Missing, duplicate,
unexpected or failed results fail verification. A rendered run also needs an
observed D3D11 renderer identity. A control reporting
`ANIMATION_ANALYSIS_RENDERED_SKIP` produces a distinct `skipped` result and a
nonzero launcher exit; an actual failed test still takes precedence. A NullRHI
pass establishes lifecycle behavior only.

`--test` replaces the default exact list. Repeat it to select multiple complete
names; it is not a prefix filter. Performance controls are deliberately explicit:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/HostChecks/performance-01 --rendered --test AnimationAnalysis.Capture.Surfaces.Performance
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --output Saved/HostChecks/comparison-01 --rendered --test AnimationAnalysis.Capture.Performance.ReadbackComparison
```

Use the same option for additional implemented controls. Selecting a test does
not imply that other capabilities were verified. `--build-timeout` defaults to
900 seconds; `--test-timeout` defaults to 300 seconds. A timeout terminates the
process tree and retains the command, deadline, elapsed time, exit/cleanup result
and logs. If tree cleanup or replay retention cannot be confirmed, the temporary
host is preserved for recovery and verification fails.

The comparison rotates disabled/synchronous/asynchronous order over three repetitions,
each with 60 warmup draws and 120 measured attempts. All modes use the same full-resolution
policy and one attempt per draw; encoding/export is outside the timed window.
Summarize the retained `observations/Performance-*/performance.json` with
`python Python/summarize_readback_performance.py <performance.json> --output <summary.json>`.
Losses or view/cadence differences invalidate a timing comparison; inspect raw results.

## Retained development and interactive inspection

Prepare a generated development copy below this checkout's ignored `Saved`:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/DevelopmentHost --output Saved/HostChecks/prepare-01
```

This stages and builds the host. Add `--stage-only` to copy sources without
building. Neither command reports a runtime pass. Subsequent preparations refresh
owned source files while preserving build outputs and observations. An unmanaged
directory or a locally modified staged source is rejected; make source edits in
the tracked plugin/host, then prepare again. Do not refresh a host while its editor
or another build is using it.

Use the retained build for repeatable tests:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/DevelopmentHost --output Saved/HostChecks/retained-rendered-01 --run-tests --rendered
```

The launcher fingerprints pre-existing observations before the run and retains
only newly created run directories. It fails if the run modifies previous
observations or writes new files into a previous run directory. Prior evidence
is not attributed to the new test run.

Open the interactive editor with the same staged source and fixture definitions:

```powershell
python Python/verify_unreal_host.py --engine "C:/Program Files/Epic Games/UE_5.6" --prepare Saved/DevelopmentHost --output Saved/HostChecks/interactive-01 --launch
```

The editor opens `/Engine/Maps/Entry`. Start PIE, then enter an inspection command
in the console:

```text
AnimationAnalysis.Host.Inspect 0
```

Indices select separated (0), touching (1), intersecting (2), label-occluded (3)
and scene-occluded (4) controls. Switch indices to inspect another control.
`AnimationAnalysis.Host.Stop` restores and removes the fixture. End PIE and close
the editor when finished. The launched editor remains under the user's control;
it has no automation deadline and its launch is recorded as
`interactive_unverified`. Starting an editor is not a rendered test result.

These geometry controls establish known visibility/depth/label observations.
Pixel adjacency and depth proximity do not establish physical contact or artistic
quality. Unsupported geometry and insufficient observations remain unknown.

## Evidence and replay retention

Each test output retains the source SHA-256 map, tool-source hashes, engine/host
identity, commands, logs, exact results and `host-verification.json`. Staging copies
only the two root project/plugin descriptors, host/plugin native sources and
optional plugin `Shaders/**/*.usf` / `Shaders/**/*.ush` resources. It creates an
empty host Content directory. Generated folders, binaries and consuming content
are excluded. Staged source hashes are checked again after successful work.

Native fixtures write beneath the host's `Saved/Observations`. The launcher
inventories the selected run files, copies them into the fresh output's
`observations`, and creates:

- `replay-inventory.json`: explicit relative paths, sizes, SHA-256 hashes and the
  absolute loose replay root.
- `replay-evidence.zip`: all inventoried entries plus its embedded inventory.
- `replay-retention.json`: archive SHA-256, count of verified replay entries and
  the exact list of any deleted PNGs.

Every ZIP entry is read and SHA-256 checked, including binary depth/label data;
entry names and the embedded inventory must also match exactly. A temporary host
is removed only after needed replay files have been retained and verified.
`--delete-pngs` additionally removes only the fresh output's inventoried loose PNGs
after archive verification. It does not delete retained development-host images,
unlisted images or non-PNG files. Without that switch, the output keeps loose PNGs.

Reverify an archive after loose images or the temporary host have been removed:

```powershell
python Python/host_tools.py verify-archive --inventory Saved/HostChecks/rendered-01/replay-inventory.json --archive Saved/HostChecks/rendered-01/replay-evidence.zip
```

For an explicit completed run outside an automated invocation, first inventory
every file needed for replay, using one `--file` per relative path:

```powershell
python Python/host_tools.py inventory --run-root Saved/DevelopmentHost/Saved/Observations/SelectedRun --file session.json --file samples.jsonl --file frames/frame0001.png --output Saved/HostChecks/manual-inventory.json
python Python/host_tools.py archive --run-root Saved/DevelopmentHost/Saved/Observations/SelectedRun --inventory Saved/HostChecks/manual-inventory.json --archive Saved/HostChecks/manual-replay.zip --delete-pngs
```

Replace those example filenames with the run's actual complete replay inventory.
The archive must be outside the run root and must not already exist. Cleanup
rejects absolute/escaping/noncanonical entry paths, duplicate entries, mismatched
run roots, changed files, symbolic links and Windows reparse points. Source hashes
are checked before archival and again before PNG deletion. Archive verification
does not assert that an operator-supplied inventory includes every semantically
required replay file; that selection remains explicit.

Tooling tests are separate from installed-package tests:

```powershell
python -m unittest discover -s Python/tooling_tests -v
python Python/verify_distribution.py --output Saved/PythonVerification-01
```

The host tools are repository tooling and are not shipped in the portable wheel.
Native host verification establishes readiness for consumer integration; the
consumer owner still updates its dependency, rebuilds and runs its affected
integration checks under the [development handoff](DEVELOPMENT_HANDOFF.md).
