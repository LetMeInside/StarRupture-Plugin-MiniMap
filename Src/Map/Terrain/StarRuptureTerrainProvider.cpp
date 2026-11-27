// Implements the unregistered native default terrain provider and scoped BC1 source retrieval.
// Reuses native resolution/readiness/metadata/copy APIs; no cache, image conversion or GPU work occurs here.
#include "StarRuptureTerrainProvider.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainSource.h"
#include "TerrainChunkAcquisition.h"
#include "TerrainChunks.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <memory>
#include <stdexcept>
#include <utility>

namespace MiniMapTerrainSource
{
    /** Holds copied source/session evidence only. The shared capability can outlive its provider;
     * native pointers and BulkData allocations exist solely in individual synchronous operations.
     */
    class StarRuptureTerrainProvider::SourceRetrieval final : public Services::Map::Terrain::ITerrainSourceRetrieval
    {
    public:
        /** Copies the re-resolvable sample, identity and registration, retaining no native pointer. */
        SourceRetrieval(Configuration configuration, Position sample,
            Services::Map::Terrain::TerrainImage::Identity identity,
            Services::Map::Terrain::TerrainImage::Registration registration);
        /** Re-resolves source and validates native readiness/mip metadata before any future cache decision. */
        Inspection Inspect(const SourceLocator& source) override;
        /** Revalidates inspected identity/layout, borrows a paired native BC1 copy through ingestion only. */
        Status Retrieve(const Inspection& expectedSource, Ingestion& destination) override;

    private:
        /** Re-resolves the native source without waiting, rejecting changed identity or registration. */
        Status ResolveReference(SourceTile& tile) const;
        /** Waits once, then re-resolves before querying metadata; no stale native borrow crosses the wait. */
        Inspection InspectReady(const SourceLocator& source,
            MiniMapNative::TextureAccess::MipData& mip, SDK::UTexture2D*& texture) const;
        /** Compares all ingestion layout evidence rather than relying on source identity alone. */
        static bool SameInspection(const Inspection& left, const Inspection& right) noexcept;
        /** Confirms the copied native allocation still belongs to the admitted source before ingestion. */
        bool IsCopyCurrent(SDK::UTexture2D* texture, const Inspection& inspection) const;

        const Configuration configuration_;
        const Position sample_;
        const Services::Map::Terrain::TerrainImage::Identity identity_;
        const Services::Map::Terrain::TerrainImage::Registration registration_;
    };

    /** Validates values only; no current-world observation occurs in construction. */
    StarRuptureTerrainProvider::StarRuptureTerrainProvider(Configuration configuration)
        : configuration_(std::move(configuration))
    {
        if (configuration_.WorldId.empty() || !configuration_.WorldGeneration ||
            configuration_.ContentRevision.empty())
            throw std::invalid_argument("Native terrain provider requires world, session and content identity.");
    }

    /** Advertises native terrain as the default without pretending its sparse tiles form XYZ selection boxes. */
    StarRuptureTerrainProvider::Coverage StarRuptureTerrainProvider::DescribeCoverage() const
    {
        return {"StarRupture.NativeTerrain", configuration_.WorldId, CoverageRole::Default, {}};
    }

    /** Checks thread before touching the current world; never dereferences a stored UWorld pointer. */
    bool StarRuptureTerrainProvider::IsCurrentSession(const Configuration& configuration)
    {
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread || !native->player.isGameThread() ||
            !MiniMapMap::GameplayReady() || MiniMapMap::GetWorldGeneration() != configuration.WorldGeneration)
            return false;
        const auto* world = MiniMapMap::GetWorld();
        return world && world->GetName() == configuration.WorldId;
    }

    /** Reuses native sparse-grid, axis exchange and radiation-brush fallback without consulting FE selection. */
    bool StarRuptureTerrainProvider::ResolveSource(const Configuration& configuration,
        Position sample, SourceTile& tile)
    {
        tile = {};
        if (!sample.IsFinite() || !IsCurrentSession(configuration)) return false;
        SDK::FVector position{};
        position.X = sample.X;
        position.Y = sample.Y;
        position.Z = sample.Z;
        if (!TryResolveSourceTile(position, tile) || !IsCurrentSession(configuration))
        {
            tile = {};
            return false;
        }
        return tile.Texture != nullptr && !tile.RegisteredImage;
    }

    /** Authored tile placement matches the active eight-by-eight, 125-metre chunk anchor convention. */
    Services::Map::Terrain::TerrainImage::Registration StarRuptureTerrainProvider::GetRegistration(
        const SourceTile& tile)
    {
        constexpr double worldTileSize = 100.0 * MiniMapTerrainChunks::kChunkWorldSizeMeters *
            MiniMapTerrainChunks::kChunksPerAxis;
        if (tile.NativeWorldSize.X != worldTileSize || tile.NativeWorldSize.Y != worldTileSize) return {};
        return {{tile.NativeWorldOrigin.X, tile.NativeWorldOrigin.Y},
            {tile.NativeWorldSize.X, 0.0}, {0.0, tile.NativeWorldSize.Y}};
    }

    /** Keeps variants distinct in memory without changing the legacy cache key/schema. */
    Services::Map::Terrain::TerrainImage::Identity StarRuptureTerrainProvider::GetIdentity(
        const Configuration& configuration, const SourceTile& tile)
    {
        const auto assetName = tile.Texture->GetFullName();
        const auto sourceId = configuration.WorldId + "/Terrain/" + std::to_string(tile.GridX) +
            "/" + std::to_string(tile.GridY);
        const auto revision = std::to_string(configuration.ContentRevision.size()) + ":" +
            configuration.ContentRevision + "/R" + std::to_string(tile.RadiationLevel) +
            "/V" + std::to_string(static_cast<int>(tile.SelectedVariant)) + "/" + assetName;
        return {sourceId, revision};
    }

    /** Returns generic source metadata and a strongly owned capability, never a durable native borrow. */
    StarRuptureTerrainProvider::SourceDescription StarRuptureTerrainProvider::DescribeSource(
        const SourceQuery& query)
    {
        if (query.WorldId != configuration_.WorldId || !query.RegionId.empty() ||
            !query.PhysicalPosition.IsFinite()) return {};
        SourceTile tile;
        if (!ResolveSource(configuration_, query.SamplePosition, tile)) return {};
        const auto registration = GetRegistration(tile);
        if (!registration.IsValid()) return {};
        auto identity = GetIdentity(configuration_, tile);
        auto capability = std::make_shared<SourceRetrieval>(configuration_, query.SamplePosition,
            identity, registration);
        RetrievalSource retrieval{{identity.SourceId}, std::move(capability), registration};
        return {std::move(identity), std::move(retrieval)};
    }

    /** Stores immutable scalar/string evidence only; ownership is the returned shared capability. */
    StarRuptureTerrainProvider::SourceRetrieval::SourceRetrieval(Configuration configuration,
        Position sample, Services::Map::Terrain::TerrainImage::Identity identity,
        Services::Map::Terrain::TerrainImage::Registration registration)
        : configuration_(std::move(configuration)), sample_(sample), identity_(std::move(identity)),
          registration_(registration)
    {
    }

    /** Source descriptions remain tied to their original content and placement even if the world stays live. */
    StarRuptureTerrainProvider::SourceRetrieval::Status
        StarRuptureTerrainProvider::SourceRetrieval::ResolveReference(SourceTile& tile) const
    {
        if (!ResolveSource(configuration_, sample_, tile)) return Status::Unavailable;
        if (GetIdentity(configuration_, tile) != identity_) return Status::SourceChanged;
        const auto registration = GetRegistration(tile);
        if (registration.Origin.X != registration_.Origin.X || registration.Origin.Y != registration_.Origin.Y ||
            registration.U.X != registration_.U.X || registration.U.Y != registration_.U.Y ||
            registration.V.X != registration_.V.X || registration.V.Y != registration_.V.Y)
            return Status::SourceChanged;
        return Status::Available;
    }

    /** A wait may replace the world or brush. Re-resolve both before using native metadata. */
    StarRuptureTerrainProvider::SourceRetrieval::Inspection
        StarRuptureTerrainProvider::SourceRetrieval::InspectReady(const SourceLocator& source,
            MiniMapNative::TextureAccess::MipData& mip, SDK::UTexture2D*& texture) const
    {
        Inspection result;
        result.Locator = source;
        result.Source = identity_;
        mip = {};
        texture = nullptr;
        if (source.Value != identity_.SourceId)
        {
            result.State = Status::Failed;
            return result;
        }
        SourceTile tile;
        result.State = ResolveReference(tile);
        if (result.State != Status::Available) return result;
        auto* waitedTexture = tile.Texture;
        tile = {};
        result.State = Status::Unavailable;
        if (!TerrainChunkAcquisition::WaitForTexture(waitedTexture)) return result;
        result.State = ResolveReference(tile);
        if (result.State != Status::Available) return result;
        if (tile.Texture != waitedTexture)
        {
            result.State = Status::SourceChanged;
            return result;
        }
        MiniMapNative::TextureAccess::PlatformData platform;
        if (!TerrainChunkAcquisition::ReadReadyMetadata(tile, tile.Texture, platform, mip) ||
            !IsCurrentSession(configuration_))
        {
            result.State = Status::Unavailable;
            mip = {};
            return result;
        }
        result.Encoding = SourceEncoding::Bc1;
        result.WidthPixels = mip.SizeX;
        result.HeightPixels = mip.SizeY;
        result.RowStrideBytes = static_cast<std::size_t>(mip.SizeX / 4u) * 8u;
        result.SRgb = platform.SRGB;
        texture = tile.Texture;
        return result;
    }

    /** Drops all native borrow values before returning copied readiness/layout evidence. */
    StarRuptureTerrainProvider::SourceRetrieval::Inspection
        StarRuptureTerrainProvider::SourceRetrieval::Inspect(const SourceLocator& source)
    {
        MiniMapNative::TextureAccess::MipData mip;
        SDK::UTexture2D* texture = nullptr;
        return InspectReady(source, mip, texture);
    }

    /** Includes exact logical dimensions, BC1 stride and sRGB in retrieval revalidation. */
    bool StarRuptureTerrainProvider::SourceRetrieval::SameInspection(
        const Inspection& left, const Inspection& right) noexcept
    {
        return left.State == right.State && left.Locator.Value == right.Locator.Value &&
            left.Source == right.Source && left.Encoding == right.Encoding &&
            left.WidthPixels == right.WidthPixels && left.HeightPixels == right.HeightPixels &&
            left.RowStrideBytes == right.RowStrideBytes && left.SRgb == right.SRgb;
    }

    /** Rechecks source and current mip interpretation after potentially reentrant native copying. */
    bool StarRuptureTerrainProvider::SourceRetrieval::IsCopyCurrent(
        SDK::UTexture2D* texture, const Inspection& inspection) const
    {
        SourceTile tile;
        if (ResolveReference(tile) != Status::Available || tile.Texture != texture) return false;
        MiniMapNative::TextureAccess::PlatformData platform;
        MiniMapNative::TextureAccess::MipData mip;
        return TerrainChunkAcquisition::ReadReadyMetadata(tile, tile.Texture, platform, mip) &&
            IsCurrentSession(configuration_) && mip.SizeX == inspection.WidthPixels &&
            mip.SizeY == inspection.HeightPixels && platform.SRGB == inspection.SRgb;
    }

    /** Supplies the native full-tile block rows, not the cache's concatenated Y-outer/X-inner chunks.
     * Native copy/free are paired even if the synchronous ingestion sink throws. The sink must copy
     * accepted bytes before returning; this provider retains no BC1 allocation between operations.
     */
    StarRuptureTerrainProvider::SourceRetrieval::Status
        StarRuptureTerrainProvider::SourceRetrieval::Retrieve(
            const Inspection& expectedSource, Ingestion& destination)
    {
        if (expectedSource.State != Status::Available) return Status::Unavailable;
        MiniMapNative::TextureAccess::MipData mip;
        SDK::UTexture2D* texture = nullptr;
        const auto current = InspectReady(expectedSource.Locator, mip, texture);
        if (current.State != Status::Available) return current.State;
        if (!SameInspection(expectedSource, current)) return Status::SourceChanged;
        void* allocation = nullptr;
        if (!MiniMapNative::TextureAccess::CopyBulkData(mip, &allocation, false) || !allocation)
            return Status::Unavailable;
        const std::unique_ptr<void, decltype(&MiniMapNative::TextureAccess::FreeBulkDataCopy)> copy(
            allocation, &MiniMapNative::TextureAccess::FreeBulkDataCopy);
        if (!IsCopyCurrent(texture, current)) return Status::SourceChanged;
        const auto bytes = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(copy.get()),
            static_cast<std::size_t>(mip.BulkSize));
        return destination.AcceptBc1(current, bytes) ? Status::Available : Status::Failed;
    }
}
#endif
