# Stage C.1 visual sources and first-pass limits

The installed EXE/PDB pair matched GUID `41cbaea9-7ac3-99da-b947-a1db9c1021ed`, age 1. Read-only PDB TPI inspection confirmed `UCrBuildingData::Icon` (+0x1F0), `UStaticMesh::RenderData` (+0xE8), `bAllowCPUAccess` (+0x1D5), `ExtendedBounds` (+0x240), and the scene-capture fields below. No private functions or new fingerprints were added.

SDK paths below are relative to `C:/Modding/StarRupture/StarRupture-Plugin-SDK/StarRupture SDK/Client/SDK/`.

| Source | Evidence | Availability and decision |
|---|---|---|
| Building UI image | `Chimera_classes.hpp:9375`, `UCrBuildingData::Icon`, `FSlateBrush` | Definition-owned authored UI brush. Its resource residency, top-down viewpoint and world alignment are not established by its declaration. Not used as factory imagery. No claim that all icons are generic or unavailable. |
| Custom output image | `Chimera_classes.hpp`, `UCrBuildingData::UIBuildingInfoCustomOutputImage` | A building-info output image; not evidence of a whole-building overhead image. Not used. |
| Placement preview | `AuActorPlacement_classes.hpp:51`, `ActorClass` (soft class), and `:57`, `HelperActorCustomMesh` | Soft references need not be loaded. Helper geometry belongs to placement, not proof of the constructed visual. No loads or preview actors introduced. |
| High/low representation | `MassRepresentation_classes.hpp:52-54`, `UMassVisualizationTrait::StaticMeshInstanceDesc`, `HighResTemplateActor`, `LowResTemplateActor`; `Chimera_classes.hpp:23628`, `UCrMassRepVisCosmeticsTrait` | Config-owned references may be resident. Construction scripts/components/variants can contribute geometry; an actor representation can disappear. No render-side actor/component inspection. |
| ISM mesh | `MassRepresentation_structs.hpp:192-203`, `FMassStaticMeshInstanceVisualizationMeshDesc::Mesh` and its local transform | Definition-level mesh references exist. A mesh bound is not a baked image or the entire multi-mesh building. CPU triangle residency is unproven. Not decoded/projected in C.1. |
| Static-mesh render data | `Engine_classes.hpp:40349`, `UStaticMesh`; PDB `RenderData`, SDK `bAllowCPUAccess` at `:40378` | Private render data exists, but a pointer/CPU-access flag does not prove cooked CPU vertices/indices are available for each resident asset. No GPU readback/private ABI traversal. |
| Spline mesh bounds | `Engine_classes.hpp:40385`, `UStaticMesh::ExtendedBounds`; `Chimera_classes.hpp:13563`, rail `SplineMesh`; `:16846`, walkway `SplineMesh` | Mesh-local bounds. B.2 already copies cross-section/extension metadata from resident template defaults into plugin-owned definition geometry, retrying as assets settle. C.1 consumes those copies. |
| Targeting proxy | `BuildingCollector.cpp`, `ResolveGeometry`, `Read`, `Geometry`; `FCrBuildingAggroTargetDataTrait/Fragment::BuildingBoundingBox` | Already copied, definition or entity-local gameplay proxy. Selected for approximate ordinary silhouettes. Not advertised as the complete visual mesh footprint. No new native reads. |
| Orthographic capture | `Engine_classes.hpp:18866`, `USceneCaptureComponent2D::ProjectionType`; `:18871`, `OrthoWidth`; `:18880`, `TextureTarget`; `:18921`, `CaptureScene` | UE capture capability is proven. A working isolated preview scene, component registration, lighting/visibility, render-target ownership and safe plugin texture bridge are not. No scene capture or render target is created. |

## Existing texture mechanisms

`Foundables.cpp::ResolveFoundableIcon` and `PointsOfInterest.cpp::ResolveIcon` resolve category brushes once and cache plugin-owned icon handles. Their bulk-data loaders use `Native/TextureAccess` and `LoadFromRGBA`. Their plugin-lifetime icon policy remains unchanged.

`Terrain.cpp` retains/reuses resident chunk textures and retires them after three render generations. C.1 does not alter this policy.

`plugin_interface.h::IPluginImGuiTextures` exposes file/memory/RGBA/UTexture2D acquisition, not an isolated mesh-preview renderer or a direct UTextureRenderTarget2D import. ModLoader `StarRupture-ModLoader-Core/UI/imgui_backend.cpp::TextureLoadFromRGBA` uploads supplied pixels; `TextureLoadFromUTexture2D` acquires/copies an engine texture resource. Neither generates a building view. C.1 creates no texture handles, so no second texture-lifetime framework is needed.

## Selected implementation

`BuildingVisuals` consumes only immutable collector snapshots on post-engine tick. Ordinary definition keys are `(world generation, UObject index, BuildingID, FName comparison index, FName number)`. Cached local bounds encode center, dimensions and aspect. A local +X-forward convention is used; signed scale, quaternion and translation are applied to all eight box corners, then their XY convex hull is copied into the visual snapshot. Instance-local targeting proxies override definition defaults. Missing sources use an outlined 4m placeholder symbol, never the 50m discovery box.

Ordinary definition work is bounded to 16 updates / 1ms of soft resolution work per new collector snapshot. Missing sources retry after 1s, resolved sources after 5s; newer copied source versions can refresh earlier. This only inspects copied data. The overall instance-preparation time is measured separately and is not capped by that definition budget.

Splines reuse B.2's shared immutable world-space Bezier geometry and definition width metadata. They receive no second entity transform. Rendering uses screen-space adaptive subdivision (0.75px control-to-finite-chord tolerance, depth 8 / 257 points maximum per segment). Constant interpolation does not draw a bridge across a jump. Source cross-section determines width when available; unknown width uses the existing 10m half-width fallback with faint fill and a centerline. Cap inflation remains in acquisition bounds; detailed cap mesh appearance is not reproduced.

The renderer culls copied bounds and segment control hulls, then uses one viewport clip rectangle. Splines draw before ordinary shapes, in stable collector identity order. The entire building layer draws after terrain and before FOW; existing marker order is unchanged.

## Validation and remaining questions

`Tests/BuildingVisualGeometryTests.cpp` exercises signed scale, yaw/tilt, offset bounds, winding, aspect, map rotation, ordinary/spline origin-outside intersections, offscreen culling, curve approximation/backtracking and subdivision limits. It is a standalone C++20 executable using the same Client SDK include paths; it is not compiled into the plugin.

Actual authored icon pixels/residency, complete visual silhouettes, CPU mesh triangle availability, and a capture-to-plugin bridge remain unverified. Unsupported curves get an explicit unresolved counter and an origin ring where visible; they cannot claim coverage for unknown geometry outside the viewport. Proxy/placeholder geometry, broad fallback spline strips and overlapping vertical levels need in-game inspection. The identity oracle remains unchanged and does not validate pixels. No taxonomy or ownership filter was introduced.
