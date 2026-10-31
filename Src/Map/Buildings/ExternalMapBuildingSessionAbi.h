#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define MINIMAP_BUILDING_ABI_CALL __cdecl
#define MINIMAP_BUILDING_ABI_EXPORT __declspec(dllexport)
#else
#define MINIMAP_BUILDING_ABI_CALL
#define MINIMAP_BUILDING_ABI_EXPORT
#endif

#define MINIMAP_BUILDING_SESSION_ABI_VERSION 1u

typedef uint32_t MiniMapBuildingAbiResult;
enum
{
    MINIMAP_BUILDING_ABI_OK = 0u,
    MINIMAP_BUILDING_ABI_INVALID_ARGUMENT = 1u,
    MINIMAP_BUILDING_ABI_UNAVAILABLE = 2u,
    MINIMAP_BUILDING_ABI_REJECTED = 3u,
    MINIMAP_BUILDING_ABI_NOT_ACTIVE = 4u,
    MINIMAP_BUILDING_ABI_VERSION_MISMATCH = 5u
};

enum
{
    MINIMAP_BUILDING_ABI_CAP_SESSION = 1u << 0,
    MINIMAP_BUILDING_ABI_CAP_DIAGNOSTICS = 1u << 1
};

typedef struct MiniMapBuildingWorldRectangleV1
{
    double MinX;
    double MinY;
    double MaxX;
    double MaxY;
} MiniMapBuildingWorldRectangleV1;

enum
{
    MINIMAP_BUILDING_STATUS_ACTIVE = 1u << 0,
    MINIMAP_BUILDING_STATUS_ADMITTED = 1u << 1,
    MINIMAP_BUILDING_STATUS_COLLECTOR_READY = 1u << 2,
    MINIMAP_BUILDING_STATUS_MEMBERSHIP_COMPLETE = 1u << 3,
    MINIMAP_BUILDING_STATUS_WORLD_INVALIDATED = 1u << 4
};

typedef struct MiniMapBuildingSessionStatusV1
{
    uint32_t StructSize;
    uint32_t Flags;
    uint64_t SessionGeneration;
    uint64_t CollectorPublicationGeneration;
    uint64_t AuthoritativeBuildingCount;
    uint64_t CompletedBuildingCount;
    uint64_t PublishedInRegionCount;
    uint64_t PendingAcquisitionCount;
} MiniMapBuildingSessionStatusV1;

typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingOpenSessionFn)(
    const MiniMapBuildingWorldRectangleV1* rectangle);
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingUpdateRectangleFn)(
    const MiniMapBuildingWorldRectangleV1* rectangle);
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingServiceFn)(float deltaSeconds);
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingReadStatusFn)(
    MiniMapBuildingSessionStatusV1* status);
typedef MiniMapBuildingAbiResult (MINIMAP_BUILDING_ABI_CALL *MiniMapBuildingCloseSessionFn)(void);

typedef struct MiniMapBuildingSessionApiV1
{
    uint32_t StructSize;
    uint32_t Version;
    uint32_t Capabilities;
    uint32_t Reserved;
    MiniMapBuildingOpenSessionFn OpenSession;
    MiniMapBuildingUpdateRectangleFn UpdateWorldRectangle;
    MiniMapBuildingServiceFn Service;
    MiniMapBuildingReadStatusFn ReadStatus;
    MiniMapBuildingCloseSessionFn CloseSession;
} MiniMapBuildingSessionApiV1;

#ifdef __cplusplus
extern "C" {
#endif
MINIMAP_BUILDING_ABI_EXPORT MiniMapBuildingAbiResult MINIMAP_BUILDING_ABI_CALL
MiniMap_GetBuildingSessionApi(uint32_t requestedVersion, MiniMapBuildingSessionApiV1* api);
#ifdef __cplusplus
}
#endif
