#pragma once
#include <stdint.h>
#if defined(_WIN32)
#define MINIMAP_BUILDING_ABI_CALL __cdecl
#define MINIMAP_BUILDING_ABI_EXPORT __declspec(dllexport)
#else
#define MINIMAP_BUILDING_ABI_CALL
#define MINIMAP_BUILDING_ABI_EXPORT
#endif
#define MINIMAP_BUILDING_SESSION_ABI_VERSION 1u /* Keep at v1 until public release. */
#ifdef __cplusplus
namespace SDK { class UWorld; class UCanvas; class UTextureRenderTarget2D; class UMaterialInstanceDynamic; }
typedef SDK::UWorld MiniMapBuildingWorld;
typedef SDK::UCanvas MiniMapBuildingCanvas;
typedef SDK::UTextureRenderTarget2D MiniMapBuildingDestination;
typedef SDK::UMaterialInstanceDynamic MiniMapBuildingMaterial;
#else
typedef struct MiniMapBuildingWorld MiniMapBuildingWorld;
typedef struct MiniMapBuildingCanvas MiniMapBuildingCanvas;
typedef struct MiniMapBuildingDestination MiniMapBuildingDestination;
typedef struct MiniMapBuildingMaterial MiniMapBuildingMaterial;
#endif
typedef uint32_t MiniMapBuildingAbiResult;
enum {
    MINIMAP_BUILDING_ABI_OK=0u, MINIMAP_BUILDING_ABI_INVALID_ARGUMENT=1u,
    MINIMAP_BUILDING_ABI_UNAVAILABLE=2u, MINIMAP_BUILDING_ABI_REJECTED=3u,
    MINIMAP_BUILDING_ABI_VERSION_MISMATCH=5u
};
typedef struct MiniMapBuildingWorldRectangle { double MinX,MinY,MaxX,MaxY; } MiniMapBuildingWorldRectangle;
/* Single external owner, synchronous admitted game-thread calls only. No callbacks.
   Render opens the persistent provider session if necessary. All native pointers
   are borrowed for this call only; caller owns target, active Begin/End canvas
   pass, and two distinct compose MIDs. SpriteMaterial may be rebound to the
   provider atlas; SolidMaterial keeps the compose parent's white texture.
   Caller retains these UObjects through queued rendering, using normal Unreal
   GC/resource retirement (EndDraw is not GPU completion). Provider retains its
   atlas independently. Rectangle axes: world +X -> right, +Y -> down.
   Failure never acknowledges closure. Only successful Close releases ownership. */
typedef struct MiniMapBuildingRenderRequest {
    uint32_t StructSize;
    float DeltaSeconds;
    MiniMapBuildingWorldRectangle Rectangle;
    MiniMapBuildingWorld* World;
    MiniMapBuildingDestination* Destination;
    MiniMapBuildingCanvas* Canvas;
    MiniMapBuildingMaterial* SpriteMaterial;
    MiniMapBuildingMaterial* SolidMaterial;
} MiniMapBuildingRenderRequest;
enum {
    MINIMAP_BUILDING_STATUS_ACTIVE=1u, MINIMAP_BUILDING_STATUS_MEMBERSHIP_COMPLETE=2u,
    MINIMAP_BUILDING_STATUS_DRAWN=4u, MINIMAP_BUILDING_STATUS_UNCHANGED=8u,
    MINIMAP_BUILDING_STATUS_ATLAS_PENDING=16u
};
typedef struct MiniMapBuildingRenderStatus {
    uint32_t StructSize,Flags;
    uint64_t PendingAcquisitionCount,PublishedInRegionCount;
} MiniMapBuildingRenderStatus;
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingRenderFn)(
    const MiniMapBuildingRenderRequest*,MiniMapBuildingRenderStatus*);
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingCloseSessionFn)(void);
typedef struct MiniMapBuildingSessionApi {
    uint32_t StructSize,Version;
    MiniMapBuildingRenderFn Render;
    MiniMapBuildingCloseSessionFn Close;
} MiniMapBuildingSessionApi;
#ifdef __cplusplus
extern "C" {
#endif
MINIMAP_BUILDING_ABI_EXPORT MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL
MiniMap_GetBuildingSessionApi(uint32_t requestedVersion,MiniMapBuildingSessionApi* api);
#ifdef __cplusplus
}
#endif
