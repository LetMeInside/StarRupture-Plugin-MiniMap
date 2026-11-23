// Defines MiniMap's polling-only Windows x64 ABI for one external CPU-image viewport.
// This header is standalone C/C++; service, collector and image ownership stay in MiniMap.
#pragma once

#include <stdint.h>

#define BUILDING_MAP_ABI_VERSION 1u
#define BUILDING_MAP_ABI_CONTRACT_ID 0x424D5331u
#define BUILDING_MAP_ABI_CALL __cdecl

/** Fixed-width operation results; absence of pixels is distinct from operation failure. */
typedef uint32_t BuildingMapABIResult;
#define BUILDING_MAP_ABI_OK 0u
#define BUILDING_MAP_ABI_UNAVAILABLE 1u
#define BUILDING_MAP_ABI_NO_SESSION 2u
#define BUILDING_MAP_ABI_INVALID_HANDLE 3u
#define BUILDING_MAP_ABI_INVALID_ARGUMENT 4u
#define BUILDING_MAP_ABI_BUSY 5u
#define BUILDING_MAP_ABI_FAILED 6u
#define BUILDING_MAP_ABI_BUFFER_TOO_SMALL 7u
#define BUILDING_MAP_ABI_NO_IMAGE 8u
#define BUILDING_MAP_ABI_WRONG_THREAD 9u

/** Provider-local IBuildingCollector identity; zero is invalid and values are never reused. */
typedef uint64_t BuildingMapCollectorHandle;
/** Provider-local ordinary BuildingMapViewport identity; zero is invalid. */
typedef uint64_t BuildingMapViewportHandle;

/** Synchronous availability and handle inspection; initialize StructSize before each call.
 * Valid flags refer only to the supplied nonzero handles. Grid dimensions are populated
 * only for a valid viewport. Service/session availability is independent of image readiness.
 */
typedef struct BuildingMapABIState
{
    uint32_t StructSize;
    uint32_t ServiceAvailable;
    uint32_t SessionAvailable;
    uint32_t CollectorValid;
    uint32_t ViewportValid;
    uint32_t Reserved;
    uint64_t VisibleGridWidth;
    uint64_t VisibleGridHeight;
} BuildingMapABIState;

/** One current visible cell and its optional completed straight-alpha sRGB RGBA8 image.
 * X/Y and SectionSize use the service's unchanged map units. Columns increase rightward,
 * rows downward; bounds are half-open. Pixels are tightly packed, top-to-bottom RGBA.
 * Initialize StructSize before every call. NO_IMAGE still supplies coordinate/bounds;
 * Width/Height/RowStride/ByteCount are then zero. No provider-owned pointer is returned.
 */
typedef struct BuildingMapABISectionImage
{
    uint32_t StructSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t Reserved;
    int64_t GridX;
    int64_t GridY;
    double XMapUnits;
    double YMapUnits;
    double SectionSizeMapUnits;
    uint64_t RowStride;
    uint64_t ByteCount;
} BuildingMapABISectionImage;

/** Exact v1 function table, copied into consumer storage by the exported getter.
 * Calls except discovery require serialized game-thread execution, without concurrent
 * service/lifecycle mutations. Consumer buffers must be writable and large enough for
 * their declared sizes. No function calls consumer code, retains a caller pointer,
 * advances work implicitly, or permits C++ exceptions across the boundary.
 *
 * Poll QueryState, then every visible local cell with CopySectionImage on every successful
 * update. During a valid CPU viewport lifetime, once a global Section coordinate has a
 * completed CPU image, its pixels, dimensions, row stride, byte count, world bounds and
 * coordinate identity remain unchanged. The completed image cannot be replaced or removed
 * during that lifetime, remains retained when it leaves the visible range, and is exposed
 * unchanged if the coordinate returns. Previously absent images may complete later.
 * NO_IMAGE means only that no completed image is currently available; it does not distinguish
 * pending, transparent or otherwise unavailable content. This is not a guarantee that an
 * image reflects later game-world changes. Closing or invalidating the viewport, ending the
 * gameplay session, or shutting down the service ends the guarantee. Reopening starts a new
 * identity scope even if provider storage survives. Images are synchronously copied into
 * caller-owned buffers; no provider-owned pointer is exposed.
 *
 * Consumers must continue observing availability and metadata for every visible cell.
 * After an image has been copied and retained within the same valid viewport lifetime,
 * matching global coordinates and complete image metadata guarantee unchanged pixels, so a
 * consumer may omit later pixel copies for that retained image. New or unretained images
 * still require copying. This guarantee covers completed CPU image storage only; it does not
 * constrain GPU scene preparation, texture publication or atlas resources. Pointer equality
 * and hashes are not publication identities. There is no pending/empty/failure classification
 * or latest-source guarantee. No atomic multi-call snapshot is promised. Intermediate
 * transitions between polls are not an event history and cannot be reconstructed.
 *
 * The provider uses restart-only reload policy. Consumers must stop calls before module
 * unload/process teardown; a module handle alone does not prove service/session readiness.
 */
typedef struct BuildingMapServiceABITable
{
    uint32_t StructSize;
    uint32_t Version;
    uint32_t ContractId;
    uint32_t Reserved;

    /** Inspects availability and optional handles (zero means none); does not advance work. */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *QueryState)(
        BuildingMapCollectorHandle collector, BuildingMapViewportHandle viewport,
        BuildingMapABIState* state);
    /** Explicitly constructs/initializes one IBuildingCollector implementation, including its
     * existing native metadata initialization. This is a resource-initialization operation,
     * not a regular polling/update operation. Output is zero on failure.
     */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *CreateCollector)(BuildingMapCollectorHandle* collector);
    /** Disposes an unused collector; BUSY while associated with an open viewport. */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *DisposeCollector)(BuildingMapCollectorHandle collector);
    /** Forwards OpenViewport(collector, x, y, width, height), unchanged; output is zero on failure. */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *OpenViewport)(
        BuildingMapCollectorHandle collector, double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits, BuildingMapViewportHandle* viewport);
    /** Forwards the same upper-left-origin, half-open x/y/width/height rectangle as OpenViewport,
     * unchanged. Service scheduling continues for unchanged bounds. Collector acquisition
     * advances when needed, even for unchanged bounds; fully cached CPU-image ranges skip it.
     */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *UpdateViewport)(
        BuildingMapViewportHandle viewport, double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits);
    /** Reads one local cell. Null buffer with zero capacity queries current metadata only.
     * A present image returns BUFFER_TOO_SMALL until capacity >= ByteCount; no partial copy.
     * OK copies current RGBA8 into caller storage, which remains caller-owned after return.
     * NO_IMAGE leaves the buffer untouched. Re-query/retry if dimensions change between calls.
     */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *CopySectionImage)(
        BuildingMapViewportHandle viewport, uint64_t localX, uint64_t localY,
        BuildingMapABISectionImage* image, uint8_t* buffer, uint64_t capacity);
    /** Closes the viewport before shutting down/discarding its associated collector.
     * Old/zero handles return INVALID_HANDLE without affecting a newer owner. Repeated
     * cleanup is safe. Session end invalidates both handles; they are never resurrected.
     */
    BuildingMapABIResult (BUILDING_MAP_ABI_CALL *CloseViewport)(BuildingMapViewportHandle viewport);
} BuildingMapServiceABITable;

#ifdef __cplusplus
extern "C" {
#endif
/** Copies the exact v1 table on matching version/size; does not require an active session.
 * Returns INVALID_ARGUMENT for a null output, wrong version or nonexact table size.
 * The DLL export name is MiniMap_GetBuildingMapServiceABI; no import library is required.
 */
#if defined(MINIMAP_EXPORTS) && defined(MODLOADER_CLIENT_BUILD)
__declspec(dllexport)
#endif
BuildingMapABIResult BUILDING_MAP_ABI_CALL MiniMap_GetBuildingMapServiceABI(
    uint32_t version, uint32_t tableSize, BuildingMapServiceABITable* table);
#ifdef __cplusplus
}
#endif

#if defined(__cplusplus) && defined(MINIMAP_EXPORTS) && defined(MODLOADER_CLIENT_BUILD)
namespace Services::Map::Buildings { class BuildingMapService; }

namespace Services::Map::Buildings
{
    /** Owns the single external collector/viewport association inside MiniMap.
     * Plugin lifecycle wiring binds/unbinds the borrowed service and ends sessions before
     * native teardown. This adapter owns no scheduler, image cache or consumer callbacks.
     */
    class BuildingMapServiceABI final
    {
    public:
        /** Binds the plugin-owned service after successful initialization; does not allocate resources. */
        static void BindService(::Services::Map::Buildings::BuildingMapService& service) noexcept;
        /** Invalidates handles and closes viewport before collector shutdown; false retains failed cleanup for retry. */
        static bool EndGameSession() noexcept;
        /** Stops admission before provider-local cleanup; never calls consumer code. */
        static bool Shutdown() noexcept;
        /** Fills the public function table; validates caller version, capacity and output pointer. */
        static BuildingMapABIResult GetABI(uint32_t version, uint32_t tableSize, BuildingMapServiceABITable* table) noexcept;

    private:
        /** Provider-local ownership, never exposed through the binary boundary. */
        struct State;
        /** Returns the inert process-lifetime adapter storage. */
        static State& GetState() noexcept;
        /** Checks service availability and the existing typed game-thread predicate. */
        static BuildingMapABIResult CheckThread() noexcept;
        /** Checks thread plus existing gameplay/session admission, without advancing work. */
        static BuildingMapABIResult CheckSession() noexcept;
        /** Shuts down the concrete integration collector through its existing lifecycle. */
        static void ReleaseResources();
        /** Converts the currently handled exception to a fixed-width ABI result. */
        static BuildingMapABIResult CurrentExceptionResult() noexcept;
        /** Populates current state for the supplied handles; output size must exactly match v1. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL QueryState(BuildingMapCollectorHandle collector, BuildingMapViewportHandle viewport, BuildingMapABIState* state) noexcept;
        /** Creates initialized provider-local IBuildingCollector ownership; one external slot only. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL CreateCollector(BuildingMapCollectorHandle* collector) noexcept;
        /** Releases an unused matching collector; does not close an associated viewport. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL DisposeCollector(BuildingMapCollectorHandle collector) noexcept;
        /** Opens a normal CPU-image viewport with unchanged service parameters. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL OpenViewport(BuildingMapCollectorHandle collector, double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits, BuildingMapViewportHandle* viewport) noexcept;
        /** Forwards the rectangle unchanged; scheduling continues, with collector advancement only when acquisition is needed, including unchanged bounds. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL UpdateViewport(BuildingMapViewportHandle viewport, double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits) noexcept;
        /** Borrows a completed image only during this synchronous caller-buffer copy. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL CopySectionImage(BuildingMapViewportHandle viewport, uint64_t localX, uint64_t localY, BuildingMapABISectionImage* image, uint8_t* buffer, uint64_t capacity) noexcept;
        /** Reads exact current cell metadata and copies optional pixels without retaining a view. */
        static BuildingMapABIResult CopyCurrentSection(uint64_t localX, uint64_t localY, BuildingMapABISectionImage& image, uint8_t* buffer, uint64_t capacity);
        /** Closes the matching viewport and its associated collector; stale handles have no effect. */
        static BuildingMapABIResult BUILDING_MAP_ABI_CALL CloseViewport(BuildingMapViewportHandle viewport) noexcept;
    };
}
#endif
