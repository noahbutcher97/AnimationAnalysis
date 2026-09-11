# Portable mesh record delivery

Recorded 2026-09-11. Python **0.3.0** adds immutable mesh observations, explicit
coverage eligibility and bounded replay. This is the first production contract
slice following the [animated readiness investigation](research/2026-09-11-animated-surface-readiness.md),
not a native skeletal-capture delivery. See [API/schema details](MESH_OBSERVATIONS.md).

## Delivered behavior

- `MeshTopology` owns global uint32 triangle indices and exact section/material
  mapping. Its identity includes asset/configuration generation and LOD.
- `MeshObservation` owns float64 XYZ bytes, transform/coordinate declarations,
  component generation, producer/configuration, acquisition clock and pose identity.
- `MeshRequest` and `MeshCompletion` preserve failure identity and distinguish
  completion time. Early failures explicitly lack an acquisition witness.
- `FeatureCoverage`, `MeshRequirement` and `assess_mesh_coverage` distinguish
  observed/inactive/excluded/unknown/unsupported evidence. Only explicit exclusions
  can use an allowed exclusion; unknown data cannot silently become eligible.
- Schema-1 replay bounds actual reads and declared counts, verifies complete hashes,
  rejects malformed identities/fields/buffers/paths, and publishes new bundles through
  an exclusive completion marker. Its returned manifest includes every replay file.

The initial record commit is `9ef3674`; replay is `a68da01`. Subsequent verification
hardening adds the explicit-only exclusion rule, pre-decode combined metadata bounds,
pre-open nonregular-file rejection and complete retention manifests. Existing motion,
RGB/depth and visual-evidence schemas/defaults are unchanged; no new dependency is
required. The Python package version changes from 0.2.0 to 0.3.0. Unreal plugin wiring,
consumer code/assets and dependency pins are unchanged.

## Verification

The final isolated-wheel run used:

```powershell
python Python/verify_distribution.py --output Saved/MeshRecordsDelivery-20260911/distribution-final
```

- **50 tests executed:** 49 pass with one optional-image skip in the core installation;
  all 50 pass with the image extra. This includes **25 new mesh controls**.
- Existing four CLI entry points and import/package isolation pass. No Unreal,
  consumer content or image dependency enters core-only installation.
- The complete documentation example executes and replays its original pose,
  simulation acquisition clock and distinct wall completion clock.
- Replay tests cover mutation after acquisition, topology/configuration/pose mismatch,
  coverage eligibility, truncated/corrupt data, duplicate JSON keys, explicit bounds,
  interrupted/competing publication, complete-manifest copying and linked-path rejection.
- Review-driven failures were reproduced before fixes. The nonregular-file control
  models the FIFO file-type boundary on Windows; it is not a live POSIX FIFO run.

Final wheel SHA-256:
`82614152db101f7e12607d99b2623aa3068f26b49165e7ab27848850a3dbdda8`.
The verifier removed its temporary build/install directories. The earlier successful
wheel run predates final review fixes and is retained separately; only `distribution-final`
qualifies this delivery. Native/consumer runtime tests were not rerun for this Python
slice. All 29 baseline native/host source hashes remain unchanged.

## Limits and next delivery

Binary float64 storage does not imply float64 measurement accuracy. Limits bound
encoded bytes and element counts, not total Python process memory. The filesystem
must support exclusive hard-link publication and the caller must keep the root stable;
hashes do not authenticate producer claims. Immutable records do not implement
concurrent request ownership or prove exactly-once termination across records.

Native capability detection, supported animation finalization, CPU/rigid production
sampling, GPU lifetime and shared mesh/image/export admission are next. The separate
research probe passed 24 animated cases and 16 raster comparisons, with approximately
0.51 ms CPU reference sampling for 10.2k vertices; this remains a microbenchmark,
not production throughput or a GPU speedup claim. No new combined performance claim
is made by the portable contract slice.

Katana's owner integrates a selected shared revision and runs affected consumer checks.
The asset audit supports deferred production morph coverage, while masked/PDO sections
and runtime overrides still require explicit qualification. Broad cloth, graph,
material-effect and geometric contact analysis remain later work.

## Retained evidence

`Saved/MeshRecordsDelivery-20260911-evidence.zip` contains **82 hash-verified entries**:
final and superseded verification logs/wheels, reproduced review failures, exact
package/test source snapshots and the runnable example with all replay files.
SHA-256: `259c807a361bea7158016c17a710c3d9f0b8b7ab8a28441a868e30d341de0b32`.
The evidence directory at `Saved/MeshRecordsDelivery-20260911/` includes its inventory,
verification summary and retention receipt independently of the development worktree.
No images were generated by this slice. The separate animated-probe archive and its
verified loose-image cleanup remain as documented in the readiness report.
