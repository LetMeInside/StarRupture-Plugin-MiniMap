#pragma once
#include <stdint.h>

#if defined(_WIN32)
#ifndef MINIMAP_BUILDING_ABI_CALL
#define MINIMAP_BUILDING_ABI_CALL __cdecl
#endif
#ifndef MINIMAP_BUILDING_ABI_EXPORT
#define MINIMAP_BUILDING_ABI_EXPORT __declspec(dllexport)
#endif
#define BC_CALL MINIMAP_BUILDING_ABI_CALL
#else
#define BC_CALL
#define MINIMAP_BUILDING_ABI_CALL
#define MINIMAP_BUILDING_ABI_EXPORT
#endif

#ifdef __cplusplus
namespace SDK { class UWorld; class UTexture2D; }
typedef SDK::UWorld BC_World;
typedef SDK::UTexture2D BC_Texture;
#else
typedef struct BC_World BC_World;
typedef struct BC_Texture BC_Texture;
#endif

#pragma pack(push, 8)
typedef struct BC_Key { int64_t X, Y; } BC_Key;
typedef struct BC_Rect { double MinX, MinY, MaxX, MaxY; } BC_Rect;
typedef struct BC_Ref { uint32_t StructSize, Reserved; uint64_t Session, Id; } BC_Ref;
typedef struct BC_Open {
    uint32_t StructSize, Reserved; BC_World* World; uint64_t ClientWorldEpoch;
    double OriginX, OriginY, CellSizeCm; uint32_t CellPixels, GroupWidth; uint64_t GridSchema;
} BC_Open;
typedef struct BC_Grid {
    uint32_t StructSize, Version, ContractId, AdmissionFlags;
    uint64_t Session, WorldGeneration, RestoreEpoch, GridId;
    double OriginX, OriginY, CellSizeCm, TileSizeCm;
    uint32_t CellPixels, GroupWidth, MaxInteriorSide, PixelConvention;
} BC_Grid;
typedef struct BC_Demand {
    uint32_t StructSize, InteriorSide; uint64_t Session, ClientDemandId;
    int64_t TileX, TileY; uint32_t Visible, Reserved; uint64_t GridId, DetailEpoch;
} BC_Demand;
typedef struct BC_Status {
    uint32_t StructSize, State, Flags, Reason;
    uint64_t RequestId, PublicationId, WorldGeneration, RestoreEpoch, GridId, CertificateId, DetailEpoch;
    int64_t TileX, TileY; BC_Rect WorldBounds;
    uint32_t InteriorSide, TextureWidth, TextureHeight, Gutter;
    double UVScaleX, UVScaleY, UVBiasX, UVBiasY;
    uint64_t ReadinessRevision, RequiredCells, ReadyCells, PendingCells, FailedCells;
} BC_Status;
typedef struct BC_Lease { uint32_t StructSize, Reserved; uint64_t Session, LeaseId, PublicationId; BC_Texture* Texture; } BC_Lease;
typedef struct BC_Policy {
    uint32_t StructSize, PolicyId, ClassCount, Gutter; uint32_t Sides[8];
    double DowngradeFraction; uint32_t MaxInteriorSide, Flags;
} BC_Policy;

enum { BC_VERSION=1u, BC_CONTRACT_ID=0x42435431u, BC_SCHEMA=1u, BC_PIXEL_CONVENTION=1u };
enum { BC_RESULT_OK=0u, BC_INVALID_ARGUMENT=1u, BC_UNAVAILABLE=2u, BC_REJECTED_THREAD_OR_ADMISSION=3u,
       BC_BUSY=4u, BC_VERSION_MISMATCH=5u, BC_STALE_SESSION=6u, BC_CLOSED=7u,
       BC_INTERNAL_FAILURE=8u, BC_RESOURCE_FAILURE=9u };
enum { BC_PENDING=1u, BC_PARTIAL=2u, BC_READY=3u, BC_EMPTY=4u, BC_FAILED=5u, BC_STALE=6u };
enum { BC_CURRENT=1u, BC_NATIVE_READY=2u, BC_COMPLETE=4u, BC_HAS_FAILURE=8u,
       BC_DETAIL_PENDING=16u, BC_RETRY_ELIGIBLE=32u, BC_ADMISSION_PENDING=64u };
enum { BC_REASON_NONE=0u, BC_SOURCE_PENDING=1u, BC_CAPTURE_PENDING=2u, BC_SPLINE_OCCLUSION_PENDING=3u,
       BC_UPLOAD_PENDING=4u, BC_GLOBAL_COMPLETENESS_UNAVAILABLE=5u, BC_POLICY_MISMATCH=6u,
       BC_RESOURCE_FAILURE_REASON=7u, BC_STALE_OR_REVOKED_IDENTITY=8u };

typedef uint32_t (BC_CALL *BC_OpenFn)(const BC_Open*, BC_Grid*);
typedef uint32_t (BC_CALL *BC_InspectSessionFn)(const BC_Ref*, BC_Grid*);
typedef uint32_t (BC_CALL *BC_SubmitFn)(const BC_Demand*, uint64_t*);
typedef uint32_t (BC_CALL *BC_InspectRequestFn)(const BC_Ref*, BC_Status*);
typedef uint32_t (BC_CALL *BC_InspectPublicationFn)(const BC_Ref*, BC_Status*);
typedef uint32_t (BC_CALL *BC_AcquireFn)(const BC_Ref*, BC_Lease*);
typedef uint32_t (BC_CALL *BC_ReleaseFn)(const BC_Ref*);
typedef uint32_t (BC_CALL *BC_WithdrawFn)(const BC_Ref*);
typedef uint32_t (BC_CALL *BC_WithdrawAllFn)(const BC_Ref*);
typedef uint32_t (BC_CALL *BC_CloseFn)(const BC_Ref*);
typedef uint32_t (BC_CALL *BC_GetPolicyFn)(const BC_Ref*, BC_Policy*);

typedef struct BC_Api {
    uint32_t StructSize, Version, ContractId, Reserved;
    BC_OpenFn Open;
    BC_InspectSessionFn InspectSession;
    BC_SubmitFn Submit;
    BC_InspectRequestFn InspectRequest;
    BC_InspectPublicationFn InspectPublication;
    BC_AcquireFn Acquire;
    BC_ReleaseFn Release;
    BC_WithdrawFn Withdraw;
    BC_WithdrawAllFn WithdrawAll;
    BC_CloseFn Close;
    BC_GetPolicyFn GetPolicy;
} BC_Api;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif
MINIMAP_BUILDING_ABI_EXPORT uint32_t MINIMAP_BUILDING_ABI_CALL MiniMap_GetBuildingComplexApi(uint32_t requestedVersion, BC_Api* outApi);
#ifdef __cplusplus
}
#endif
