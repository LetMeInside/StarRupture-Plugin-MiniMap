#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "../Map/Buildings/BuildingComplexCellRasterizer.h"
#include "../Map/Buildings/BuildingComplexWorld.h"
namespace SDK { class UTexture2D; }

namespace MiniMapTexturePublication
{
    // Linear associated R,G,B and transmittance T, little-endian half words.
    inline constexpr std::array<uint8_t, 8> ClearPixel{0, 0, 0, 0, 0, 0, 0, 0x3c};
    struct Generation;
    class Image
    {
        int32_t Width = 0, Height = 0;
        std::vector<uint8_t> Pixels;
        friend uint64_t Prepare(Image&&, Generation);
    public:
        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&&) noexcept = default;
        Image& operator=(Image&&) noexcept = default;
        // Pure CPU operation, permitted on workers. Invalid input is rejected,
        // never clamped. Ownership transfers; callers retain no mutable aliases.
        static std::optional<Image> FromCanonical(int32_t width, int32_t height, std::vector<uint8_t>&& bytes);
    };
    struct Generation
    {
        uint64_t Epoch = 0, Request = 0, Revision = 0;
        bool operator==(const Generation&) const = default;
    };
    enum class State : uint8_t { Empty, Prepared, ResourceInitializing, Ready, Retiring, Failed };
    struct Status
    {
        State Phase = State::Empty;
        bool OwnershipUncertain = false, Failed = false;
        uint64_t ReservedCpuBytes = 0, ReservedGpuBytes = 0;
    };
    enum class RequestState : uint8_t { Unknown, Pending, Ready, Failed, Stale };
    struct RequestStatus
    {
        RequestState State = RequestState::Unknown;
        uint64_t RequestId = 0;
        uint64_t PublicationRevision = 0;
        bool CompleteCell = false;
    };
    struct Lease
    {
        uint64_t LeaseId = 0;
        uint64_t RequestId = 0;
        uint64_t PublicationRevision = 0;
        SDK::UTexture2D* Texture = nullptr;
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : LeaseId(other.LeaseId), RequestId(other.RequestId),
              PublicationRevision(other.PublicationRevision), Texture(other.Texture)
        { other.LeaseId = other.RequestId = other.PublicationRevision = 0; other.Texture = nullptr; }
        Lease& operator=(Lease&&) = delete;
    };
    struct GroupedRequestStatus
    {
        RequestState State = RequestState::Unknown;
        uint64_t RequestId = 0;
        uint64_t PublicationId = 0;
        uint64_t CertificateId = 0;
        bool CompleteTile = false;
    };
    bool Initialize() noexcept;
    // Native functions below are GT-only. Prepare/Ready require the existing
    // admitted BuildingComplex service scope. Zero means no admission.
    uint64_t Prepare(Image&&, Generation);
    SDK::UTexture2D* Ready(uint64_t owner, Generation current);
    Status Inspect(uint64_t owner);
    // The future consumer must detach all issued uses before Retire. A ready
    // identity is immutable and remains rooted until this acknowledgement.
    bool Retire(uint64_t owner, bool detached);
    // BCB request/acquire/release/invalidation functions are GT-only. Requests
    // retain the immutable CPU image; native upload runs only in ProcessRequests
    // inside the admitted GT service.
    uint64_t RequestCell(std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage> image) noexcept;
    RequestStatus InspectRequest(uint64_t requestId) noexcept;
    // Acquire and Release are GT-only. Each successful acquisition has its own
    // idempotently releasable lease. The texture pointer is valid only while
    // that lease is held and must be detached before Release.
    bool Acquire(uint64_t requestId, Lease& output) noexcept;
    bool Release(uint64_t leaseId) noexcept;
    // BCT1 grouped publications share the same native texture/root/fence owner
    // and deferred retirement path as canonical cell publications.
    uint64_t RequestGroupedTile(std::shared_ptr<const MiniMapBuildingComplexWorld::GroupedTileImage> image,
        uint64_t certificateId) noexcept;
    GroupedRequestStatus InspectGroupedRequest(uint64_t requestId) noexcept;
    GroupedRequestStatus InspectGroupedPublication(uint64_t publicationId) noexcept;
    GroupedRequestStatus FindGroupedTile(int64_t tileX, int64_t tileY, uint32_t interiorSide,
        MiniMapBuildingComplexWorld::TileIdentity& identity) noexcept;
    void WithdrawGroupedPublication(uint64_t publicationId) noexcept;
    bool AcquireGrouped(uint64_t publicationId, Lease& output) noexcept;
    bool ReleaseGrouped(uint64_t leaseId) noexcept;
    void WithdrawGroupedRequest(uint64_t requestId) noexcept;
    // Called by the existing BuildingComplex world service while admitted.
    void ProcessRequests() noexcept;
    void InvalidateCells(const std::vector<MiniMapBuildingComplexIndex::CellKey>& cells) noexcept;
    void InvalidateCapture(MiniMapBuildingComplexIndex::MassKey building) noexcept;
    void InvalidateAll() noexcept;
    // Called outside lifecycle locks by the existing plugin callback. Polls
    // active owners; never waits, flushes, or does worker GPU IO.
    void Service();
    void CancelWorld() noexcept; // Withdraw readiness; retain issued uses.
    void Shutdown() noexcept; // No native work without safe GT/GC admission.
}
#endif
