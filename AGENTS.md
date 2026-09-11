# Repository instructions

Keep this suite independent of consuming games and local tooling. The Python core
accepts explicit records, clocks, identities and criteria. Unreal-specific sampling
lives in the AnimationCapture module; game classes, assets, skeleton defaults,
profiles and assertions stay in consumer adapters. Read README.md, docs/MIGRATION.md
and docs/DEVELOPMENT_HANDOFF.md for the current delivery and integration boundary.

Use purpose-based names. Never introduce private workflow gate names. Preserve
acquisition versus completion identity and report insufficient evidence explicitly.
Pixel adjacency, depth proximity and gameplay telemetry do not establish physical
contact or artistic quality. Unsupported geometry remains unknown.

Use Python/verify_distribution.py for package isolation and Python/verify_unreal_host.py
for native isolation. Build the consuming project and run relevant integration tests
when public behavior or module wiring changes. Keep generated files out of Git.
Archive and verify needed replay evidence before cleaning generated images. Do not
alter consumer assets or publish a repository without explicit authorization.
