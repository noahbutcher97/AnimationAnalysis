# Mesh Surface Analysis Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to implement
> this plan task-by-task, with independent kernel and public-boundary review.

**Goal:** Measure explicit mesh-region separation and sampled intersection with
qualified evidence and bounded offline work.

**Architecture:** Reuse mesh records and coverage requirements. A private exact
triangle kernel and bounded AABB search serve the public measurement service;
interval reporting consumes immutable measurement metadata.

**Tech Stack:** Python >=3.11, standard library, unittest, Fraction; schema-1 replay.

**Spec:** `docs/superpowers/specs/2026-09-13-mesh-surface-analysis-design.md`

## Global constraints

- Python >=3.11; standard library only; mesh replay schema 1 remains readable.
- New additive measurement APIs ship as Python 0.4.0. Native acquisition is unchanged.
- Keep all changes in AnimationAnalysis. Katana owns adapters, assets, pins and live qualification.
- No push, consumer edit, image generation or archive cleanup is part of this slice.
- Exact predicates describe stored geometry, not acquisition accuracy or artistic quality.

## Task 1: Exact geometry and bounded search

Files: create `Python/src/animation_analysis/_triangle_geometry.py`,
`_mesh_search.py`, `Python/tests/test_triangle_geometry.py`, `test_mesh_search.py`.

Interfaces: `is_degenerate(triangle)`; `triangle_distance_squared(a,b)` returns
`(Fraction, first_point, second_point)`. Points contain three Fractions.
`nearest_triangles(first,second,*,max_pair_tests,max_node_visits)` consumes
`[(global_triangle_id, triangle), ...]` and returns immutable `SearchResult` fields
`distance_squared, first_point, second_point, first_triangle, second_triangle,
triangle_tests, node_visits`. `SearchLimitError` exposes the two counts.

- [ ] Add failing analytic geometry and search controls. Example independent expectation:
  ```python
  a = tuple(tuple(Fraction(v) for v in p) for p in ((0,0,0),(2,0,0),(0,2,0)))
  b = tuple(tuple(Fraction(v) for v in p) for p in ((0,0,3),(2,0,3),(0,2,3)))
  self.assertEqual(triangle_distance_squared(a,b)[0], 9)
  ```
- [ ] Run the two test modules; retain the expected missing-feature failure.
- [ ] Implement exact candidate calculations and deterministic binary AABB traversal.
  Count before each triangle test/node visit; raise on exhausted bounds.
- [ ] Verify crossing, coplanar, edge-edge, degeneracy, pruning and exhaustion controls.
- [ ] Review against the spec and commit the verified kernel slice.

## Task 2: Regions, provenance and measured results

Files: create `Python/src/animation_analysis/mesh_analysis.py`,
`Python/tests/test_mesh_analysis.py`, and test-only `mesh_analysis_fixtures.py`.
Modify exports in `Python/src/animation_analysis/__init__.py` after API tests pass.

Interfaces: all public records and `measure_mesh_pair` specified in the design;
`MeshPairResult.to_mapping()` emits format `mesh_pair_measurement`, schema 1,
method `exact-triangle-surfaces-v1`. Exact squared-distance numerator/denominator
are decimal strings; all presentation numbers must be finite.

- [ ] Add failing end-to-end fixtures with literal distances/intersections and
  explicit `MeshRequirement`; a translated parallel panel must measure distance 3.
- [ ] Implement bounded region normalization and content identity, eligibility,
  selected indexed reads, exact transforms, identity-only results and search wiring.
- [ ] Check `result.surface_intersection is False` for a tiny nonzero gap even
  when `result.within_tolerance is True`; containment remains `not_evaluated`.
- [ ] Add omitted-cloth/morph negative evidence, wrong topology/pose/time, selected
  degeneracy, transform collapse, malformed limits, and budget-exhaustion tests.
- [ ] Verify public serialization, source buffer independence and replay round trip;
  commit the reviewed public measurement slice.

## Task 3: Interval evidence

Files: create `Python/src/animation_analysis/mesh_intervals.py`,
`Python/tests/test_mesh_intervals.py`; add public exports.

Interface: `summarize_mesh_interval(results,interval,*,max_gap_seconds,max_samples)`
returns an immutable summary with JSON-compatible `to_mapping()`.

- [ ] Add failing controls for complete endpoints, a missing middle sample,
  insufficient observations, duplicate/reversed clocks and changing region/profile.
- [ ] Implement bounded validation and gap reporting, preserving partial observed
  minimum and counts without claiming continuous contact.
- [ ] Verify a sequence at 0, 0.5, 1 seconds passes a 0.5-second gap bound and fails
  a 0.25-second bound; neither result asserts between-sample behavior.
- [ ] Review and commit the interval slice.

## Task 4: Installed qualification and delivery

Files: update `Python/pyproject.toml`, `Python/README.md`, README and active handoff/
capability docs. Create `docs/MESH_ANALYSIS.md`, `docs/MESH_ANALYSIS_DELIVERY.md`.

- [ ] Set the additive Python release to 0.4.0; keep historical delivery versions intact.
- [ ] Run `python Python/verify_distribution.py --output Saved/MeshAnalysisDistribution`.
- [ ] Run tooling controls and compare native paths to baseline `0813894`.
- [ ] Exercise preserved native mesh replay through a fresh installed wheel;
  benchmark fixed region sizes and explicit exhausted bounds, recording context.
- [ ] Independently review numerical decisions, eligibility, identity, bounds and
  interval semantics; fix substantiated defects and rerun affected checks.
- [ ] Document exact results, limits and deferred consumer qualification; commit
  verified documentation and integrate locally without publication.

## Execution ledger

Baseline: root and isolated checkout clean at `0813894`; 52 core tests passed.

| Review | Interface / consistency finding |
|---|---|
| Task 1 internal | Exact triangle candidates and bounded search agree with exact predicates; no tolerance modifies intersection. |
| Task 2 internal | Eligibility precedes numeric results; original identities survive insufficient results. |
| Task 3 internal | Summary validates clocks and configuration; no interpolation or duration estimate. |
| Task 4 internal | Additive Python release; native source/replay compatibility remains distinct from consumer qualification. |
| Tasks 1/2 | Fraction points and SearchResult names fixed above; public layer owns transform/coverage/region checks. |
| Tasks 2/3 | Summary consumes immutable measurement metadata; raw mesh buffers are not retained. |
| Tasks 2/4, 3/4 | Exports and documentation follow the tested public signatures; package version changes once. |

Ruling: the user's repeated approval and final proceed authorize the scoped design
and execution; routine API details do not require another permission round.
Ruling: use the existing ignored `Saved` worktree convention and preserve old replay
evidence; the new worktree is `Saved/MeshAnalysisWorktree` on `feature/mesh-surface-analysis`.
