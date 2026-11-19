# MiniMap Technical Documentation

These references cover StarRupture engine interfaces, MiniMap rendering and resource lifetimes, lifecycle behavior, terrain acquisition, and AlienX integration. They are maintainer references, not installation guides or specifications for new features.

## Engine

1. [Mass Entity Identity and Building Discovery](Engine/Mass-Entity-Identity-and-Building-Discovery.md) — Mass handles, configured building identity, fragment ownership, authoritative queries, grid traversal, and persistent save IDs.
2. [Rendering and Asset Capture](Engine/Rendering-and-Asset-Capture.md) — Asset readiness, StaticMesh CPU data, scene capture, GPU readback, and resource lifetime.
3. [Spline Payloads, Geometry, and Occlusion](Engine/Spline-Payloads-Geometry-and-Occlusion.md) — Native spline payload ABI and ownership, geometry conversion, category limitations, clipping, and render ordering.
4. [Story-Item Identity and Collection State](Engine/Story-Item-Identity-and-Collection-State.md) — Story-object identity, placed actors, GUID relationships, collection status, read state, playback state, and save paths.
5. [Terrain Acquisition and Cache Identity](Engine/Terrain-Acquisition-and-Cache-Identity.md) — Terrain source discovery, texture/BulkData access, BC1 decoding, cache identity and validation, and world-lifetime boundaries.

## Lifecycle

- [Loading and World Transitions](Lifecycle/Loading-and-World-Transitions.md) — Loading presentation, GameInstance/world lifetimes, native manager behavior, garbage collection, teleportation, plugin initialization, and shutdown limitations.

## ModLoader

- [Compatibility and Version Resolution](ModLoader/Compatibility-and-Version-Resolution.md) — Loader requirements, SDK and runtime build boundaries, version-map resolution, and Foundables GameState validation.
- [Texture Publication](ModLoader/Texture-Publication.md) — AlienX interface-70 texture APIs, ownership, atlas publication, upload timing, and retirement semantics.

## Architecture

- [Map Service Architecture](Architecture/Map-Service-Architecture.md) — Current building acquisition, immutable Section publications, scheduling, GPU presentation, numerical images, and ownership in the MiniMap building layer.

The Map service interfaces described here are internal C++ source contracts used by MiniMap. They are not an exported plugin ABI.

## Evidence and version boundaries

The Engine and Lifecycle references preserve historical native findings alongside current implementation notes. The architecture and compatibility references describe the active service boundaries; historical collector, atlas and rendering details are labelled separately.

The references combine historical native/PDB/SDK research, historical runtime observations, current-source descriptions, explicitly identified inferences, and unresolved questions. Findings are version-bound: historical executable RVAs, native layouts, and implementation observations are not universally portable contracts. Current-source descriptions do not substitute for runtime compatibility testing.
