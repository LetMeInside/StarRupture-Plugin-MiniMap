# Map Service Architecture

## Scope and data flow

MiniMap's building Map service is implemented in `Src/Services/Map/Buildings`. It manages a sparse grid of Sections, acquisition demand, sprite resources, incremental preparation, and building presentation. Terrain, fog, foundables and POIs retain their separate game-specific implementations under `Src/Map`; this service does not own the whole minimap.

The HUD building layer is [BuildingLayer](../../Src/UI/BuildingLayer.h). Its data flow is:

```text
BuildingLayer -> BuildingCollectorV2 -> immutable Section publications
  -> BuildingMapService -> Section scheduling and prepared scenes
  -> immutable GPU viewport snapshot -> ICanvas -> AlienX adapter
```

[BuildingCollectorV2](../../Src/Map/Buildings/BuildingCollectorV2.h), [BuildingDefinitionAcquisition](../../Src/Map/Buildings/BuildingDefinitionAcquisition.h), and [BuildingSplineSource](../../Src/Map/Buildings/BuildingSplineSource.h) acquire StarRupture facts. The collector implements [IBuildingCollector](../../Src/Services/Map/Buildings/IBuildingCollector.h). Neutral candidate records contain owned numerical values, strings and immutable CPU owners; no Mass fragment borrow crosses that publication boundary. Geometry preparation and numerical drawing consume those records without reading game entities.

These internal C++ contracts separate MiniMap's acquisition, preparation and drawing responsibilities. Capture uses Unreal facilities; lifecycle/bootstrap and some map paths depend on the AlienX integration.

## Ownership and lifecycle

| Owner | Responsibility and lifetime |
|---|---|
| [plugin.cpp](../../Src/plugin.cpp) | Owns the plugin-lifetime `BuildingMapService`, starts/stops gameplay sessions, forwards tick timing, and dispatches the persistent capture renderer's lifecycle work. |
| [BuildingMapService](../../Src/Services/Map/Buildings/BuildingMapService.h) | Owns Sections, sprite source manager, scheduling coordinator, shared budget and viewport registrations. Viewport registrations are non-owning. |
| `BuildingLayer` | Owns the current collector and GPU viewport; closes the viewport before shutting down collector resources. |
| [BuildingMapViewport](../../Src/Services/Map/Buildings/BuildingMapViewport.h) | Caller-owned demand rectangle with a non-owning collector association. One collector may be associated with only one open viewport and must outlive that association. |
| [BuildingMapSection](../../Src/Services/Map/Buildings/BuildingMapSection.h) | Owns preparing/completed scenes, retained geometry, completed numerical image state and GPU presentation state for its grid cell. |
| [SpriteSourceManager](../../Src/Services/Map/Buildings/Cache/SpriteSourceManager.h) | Owns immutable CPU sprite payloads, disk-cache access, atlas storage, published bindings and texture receipts/leases. |

`StartGameSession` establishes active map dimensions and scheduling. Collector native resources have explicit initialization and shutdown; `Update` does not own session creation or teardown. World replacement closes admission and stops the building session before a new session starts. Service destruction invalidates outstanding viewport associations.

Stopping gameplay requests capture cancellation and clears gameplay state; it does not synchronously prove native retirement. The persistent [BuildingSpriteRenderer](../../Src/Services/Map/Buildings/Capture/BuildingSpriteRenderer.h) owns its private capture world and request resources. Plugin shutdown retains service resources when callback removal, capture teardown or texture receipt retirement is incomplete. Native dependencies remain available for dependent cleanup. See [Loading and World Transitions](../Lifecycle/Loading-and-World-Transitions.md) and [Rendering and Asset Capture](../Engine/Rendering-and-Asset-Capture.md).

## Acquisition and immutable publication

`SetAcquisitionRegion` receives the exact rectangle covered by demanded Sections. Discovery of contributors whose origins lie outside that rectangle belongs to the collector. The service adds no game-specific acquisition padding.

The collector performs a Building-tag Mass chunk query, copies facts during admitted game-thread access, and commits a regional result only while its source context remains current. Initial/uncovered demand requires acquisition. Accepted Mass mutations trigger acquisition after a 100 ms trailing-edge quiet period; failed attempts use a retry delay (one second by default). There is no ordinary stationary periodic refresh. Successful commits acknowledge only the mutation revision sampled for that attempt, leaving later arrivals pending.

`AcquisitionHash()` reads committed regional content without advancing acquisition. It prompts the service to compare demanded Section content. `AcquireSectionPublications()` performs no native acquisition and returns one non-null immutable publication per exact requested Section, in input order. Empty Sections are valid results. Unavailable committed data throws `SectionPublicationUnavailable`; invalid requests and construction failures remain distinct from empty content.

[BuildingCandidatePublication](../../Src/Services/Map/Buildings/BuildingCandidatePublication.h) owns aligned candidate/source vectors. Publication-local ordinal N identifies source record N; it is not a game entity ID. Borrowed spans and pointers require the publication owner to remain alive. Every 64-bit hash, including zero, is valid. Equal Section content can retain its selected owner and preparation progress; a different publication address alone does not require reconstruction.

## Coordinates and transforms

Service rectangles use an upper-left origin, positive X rightward and positive Y downward. Bounds are half-open: `[x, x + width)` and `[y, y + height)`. Local cell `(0, 0)` is the top-left cell. Signed global grid coordinates support negative positions; cells are aligned to the configured Section size.

The service is unit-agnostic. MiniMap supplies native centimetres directly: the current grid uses **12,500 units per Section** and **256 pixels per numerical Section image**. Sprite capture density is configured separately from Section image resolution. These 2D conventions do not redefine Unreal's 3D axes.

[MapTransform](../../Src/Map/MapTransform.h) projects world XY relative to the tracking origin, rotates, scales by `1 / (100 * metersPerPixel)` and translates to the screen centre. Z remains available to preparation and height-aware occlusion but is not a screen axis. `BuildingLayer` copies the current projection and screen clip into `BuildingMapGpuViewport::CanvasTransform` for each draw.

## Scheduling and Section state

[SectionSchedulingCoordinator](../../Src/Services/Map/Buildings/Scheduling/SectionSchedulingCoordinator.h) owns the Section scheduler and sprite acquisition scheduler. The shared [FrameBudgetController](../../Src/Services/Map/Buildings/Coordination/FrameBudgetController.h) establishes one tick allowance capped at 2 ms. Section preparation and GPU assembly share an absolute deadline and retain cursors across yields. Repeated advancements do not replenish that tick's allowance. The deadline is cooperative: indivisible operations can exceed it, and native acquisition, publication construction and atlas upload have separate synchronous costs.

[SectionScheduler](../../Src/Services/Map/Buildings/Scheduling/SectionScheduler.h) establishes sprite painter order once per stable source generation: valid placement in ascending world Z/Y/X, then publication ordinal; unresolved placements retain source order after valid placements. Ready occurrences append into storage with direct ordinal lookup. Late sprite readiness fills its existing painter position; it does not cause a new sort. Required ordinary occluder evidence precedes dependent spline preparation.

The Section retains preparing and completed numerical scenes. First appearance can publish eligible partial geometry. Completed scenes take precedence over preparing replacements. Routine dirty GPU publication opportunities are spaced by one second; admitted assembly continues across deadline yields. Assembly gathers the ready subset in painter order into contiguous pass arrays and seals them through immutable ownership transfer. Growth after an assembly's input boundary is handled by a later batch. Unchanged Sections share existing geometry owners.

A completed nonempty replacement keeps its GPU predecessor until a matching replacement owner is sealed. A completed empty result withdraws presentation; an invalidated incomplete result loses eligibility. Already borrowed immutable snapshots remain valid for the borrowing callback. Section rendering state outside every viewport's one-cell residency margin can be released independently of the sparse cell's existence.

## Rendering boundaries

[BuildingMapGpuViewport](../../Src/Services/Map/Buildings/BuildingMapGpuViewport.h) is the HUD route. Its game-thread publication pairs immutable Section geometry with the exact atlas bindings, texture identity, dimensions, drawing revision and leases. Atlas-only changes rebind the existing geometry. A range change refreshes membership in Y-then-X grid order without preparing geometry during Draw.

Draw acquires one immutable snapshot and submits **ordinary sprites, spline surfaces, then overlay-last sprites** across the viewport. The Base Core is the overlay-last building. Draw applies the live transform and intersecting screen clip; it does not access game entities, the service, Sections, scheduler or mutable atlas state.

[ICanvas::TexturedTriangles](../../Src/Services/Viewport.h) is a synchronous neutral drawing contract. The AlienX adapters translate portable vertices and 16-bit indices to the loader's bulk API. Batches preserve pass order and split at texture changes or index capacity. Solid spline surfaces use a shared opaque-white texture with vertex tint. There is no legacy per-triangle fallback. See [Compatibility and Version Resolution](../ModLoader/Compatibility-and-Version-Resolution.md).

The existing `OpenViewport` route requests numerical Section images. [SectionRendering](../../Src/Services/Map/Buildings/Rendering/SectionRendering.h) draws the same prepared numerical scene through [IImageDestination](../../Src/Services/Map/Buildings/IImageDestination.h), whose implementation is [Rgba8ImageDestination](../../Src/Services/Map/Buildings/Rendering/Rgba8ImageDestination.h). Completed pixels are Section-owned and commit only against current source/demand evidence. This route is independent of atlas bindings and GPU snapshot adoption; the HUD's GPU viewport does not request these images. GPU presentation and numerical image completion are separate states; their pixel equivalence has not been established by this source review.

## Texture and native resource lifetimes

The sprite manager packs processed CPU payloads into a persistent RGBA atlas with two-pixel extruded gutters, starting at 512×512 and growing to 1024×1024. GPU publication uploads a complete replacement and adopts its matching metadata only after validating the upload. Failure preserves the prior valid publication. Atlas publication batching is separate from Section geometry publication.

[ITextures](../../Src/Services/Textures.h) separates the non-owning `Texture` drawing identity from an `OwnedTexture` receipt. Receipt destruction does not call the backend. Explicit release distinguishes returned ownership, unavailable retained ownership and uncertain ownership. Snapshot leases defer owner release while CPU draw callbacks can still use it; backend retirement protects already recorded GPU work. Neither mechanism retires Unreal capture resources.

Capture/readback uses separate UObject, RHI, readback-job and fence ownership. Cancellation, copied pixels, scene retirement and private-world cleanup are separate transitions. UI texture leases and CPU publication generations cannot replace native retirement fences. See [Texture Publication](../ModLoader/Texture-Publication.md).

## CPU viewport image lifetime

The Building Map ABI copies completed CPU Section images into caller-owned buffers. For a valid CPU viewport lifetime, a completed image's pixels, dimensions, row stride, byte count, world bounds and global Section coordinate remain unchanged. The service retains completed images when their Sections leave the visible range, so returning to a completed coordinate exposes the same image. Sections that have no completed image may become available on a later poll; `NO_IMAGE` does not classify whether content is pending, transparent or otherwise unavailable.

The service releases completed CPU images when the last CPU image viewport closes. Closing another CPU viewport or changing the visible range does not remove them while a CPU image viewport remains active. Gameplay-session termination, viewport invalidation and service shutdown end the image guarantee. Reopening a viewport starts a new consumer identity scope, regardless of whether provider storage happens to survive.

This immutability applies only to completed CPU Section image storage. It does not constrain later game-world freshness, GPU scene preparation, texture publication or atlas resources; those retain their independent update and lifetime rules.

## Threading and failure contracts

Service, viewport mutation, collector, sprite-manager and scheduling operations are serialized on the game thread. These objects do not provide a general concurrent mutation API. Render-thread Draw uses atomic immutable snapshot publication; `BuildingLayer` also takes a short mutex to acquire its shared viewport owner. This permits local snapshot borrowing, not concurrent mutation of the service.

Image-change handlers are synchronous coordinate-only callbacks. They must not destroy the service, change registrations or recursively advance scheduling. Drawable enumeration likewise borrows geometry only for its callback; it is not the render-thread snapshot API.

Unavailable acquisition is not an empty Section, a pending sprite is not a completed image, and failed upload is not a new atlas revision. Cleanup preserves uncertain ownership instead of retrying ambiguous backend operations blindly. Source-level lifecycle guards do not establish arbitrary physical DLL hot-unload safety.
