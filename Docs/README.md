# MiniMap Technical Documentation

These technical references preserve findings about StarRupture, Unreal Engine, AlienX ModLoader, and MiniMap's implementation boundaries. They are primarily for MiniMap maintainers, mod developers, and developers investigating relevant StarRupture native interfaces and resource lifetimes. They are research references, not installation guides, user manuals, or specifications for new features.

## Engine

1. [Mass Entity Identity and Building Discovery](Engine/Mass-Entity-Identity-and-Building-Discovery.md) — Mass handles, configured building identity, fragment ownership, authoritative queries, grid traversal, and persistent save IDs.
2. [Rendering and Asset Capture](Engine/Rendering-and-Asset-Capture.md) — Asset readiness, StaticMesh CPU data, scene capture, GPU readback, and resource lifetime.
3. [Spline Payloads, Geometry, and Occlusion](Engine/Spline-Payloads-Geometry-and-Occlusion.md) — Native spline payload ABI and ownership, geometry conversion, category limitations, clipping, and render ordering.
4. [Story-Item Identity and Collection State](Engine/Story-Item-Identity-and-Collection-State.md) — Story-object identity, placed actors, GUID relationships, collection status, read state, playback state, and save paths.
5. [Terrain Acquisition and Cache Identity](Engine/Terrain-Acquisition-and-Cache-Identity.md) — Terrain source discovery, texture/BulkData access, BC1 decoding, cache identity and validation, and world-lifetime boundaries.

## Lifecycle

- [Loading and World Transitions](Lifecycle/Loading-and-World-Transitions.md) — Loading presentation, GameInstance/world lifetimes, native manager behavior, garbage collection, teleportation, plugin initialization, and shutdown limitations.

## ModLoader

- [Texture Publication](ModLoader/Texture-Publication.md) — AlienX interface-70 texture APIs, ownership, atlas publication, upload timing, and retirement semantics.

## Evidence and version boundaries

The references combine historical native/PDB/SDK research, historical runtime observations, current MiniMap source inspection, explicitly identified inferences, and unresolved questions. Findings are version-bound: historical executable RVAs, native layouts, and implementation observations are not universally portable contracts. Current-source descriptions do not substitute for runtime compatibility testing.