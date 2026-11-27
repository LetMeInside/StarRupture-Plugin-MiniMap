// Implements demand-driven BC1 source inspection, compatible cache coordination and immutable delivery.
// Each advancement executes one phase; native/provider calls remain synchronous and application-admitted.
#include "TerrainImageAcquisition.h"
#include "../Processing/TerrainBc1Codec.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "../Cache/NativeTerrainCacheRecord.h"
#endif
#include <utility>

namespace Services::Map::Terrain::Acquisition
{
    /** Call-local sink. Owns canonical BC1 before returning to the provider; never retains borrowed spans. */
    class TerrainImageAcquisition::SourceIngestion final : public ITerrainSourceRetrieval::Ingestion
    {
    public:
        /** Copies the successfully inspected contract and affine registration for one retrieval operation. */
        SourceIngestion(ITerrainSourceRetrieval::Inspection expected, TerrainImage::Registration registration)
            : expected_(std::move(expected)), registration_(registration) {}
        /** Validates exactly one submission, copies its bytes and delegates canonical admission to M2b. */
        bool AcceptBc1(const ITerrainSourceRetrieval::Inspection& source,
            std::span<const std::uint8_t> blocks) override
        {
            if (submitted_ || !SameInspection(source, expected_) ||
                !Processing::TerrainBc1Codec::Validate(blocks, source.WidthPixels, source.HeightPixels))
            {
                invalid_ = true;
                return false;
            }
            submitted_ = true;
            std::vector<std::uint8_t> owned(blocks.begin(), blocks.end());
            auto image = TerrainService::IngestBc1(source.Source, source.WidthPixels, source.HeightPixels,
                registration_, source.SRgb, std::move(owned));
            if (!image) return false;
            image_ = std::make_shared<const TerrainImage>(*image);
            return true;
        }
        /** Rejects raw source variants; this milestone admits already-compressed BC1 only. */
        bool AcceptRgb(const ITerrainSourceRetrieval::Inspection&, std::span<const std::uint8_t>) override
        {
            invalid_ = true;
            return false;
        }
        /** Returns the sole immutable submission only if the complete sink interaction was valid. */
        std::shared_ptr<const TerrainImage> GetImage() const noexcept
        {
            return invalid_ ? nullptr : image_;
        }
    private:
        ITerrainSourceRetrieval::Inspection expected_;
        TerrainImage::Registration registration_;
        std::shared_ptr<const TerrainImage> image_;
        bool submitted_ = false;
        bool invalid_ = false;
    };

    /** Captures physical selection evidence separately from the requested source sample. */
    TerrainImageAcquisition::TerrainImageAcquisition(TerrainService::ImageRequestParameters parameters,
        std::shared_ptr<ITerrainDataProvider> provider, ITerrainDataProvider::Position physicalPosition)
        : parameters_(std::move(parameters)), provider_(std::move(provider))
    {
        query_ = {parameters_.Context.WorldId, parameters_.RegionId, physicalPosition, parameters_.SamplePosition};
        delivery_.Context = parameters_.Context;
        delivery_.RegionId = parameters_.RegionId;
        delivery_.State = TerrainImageDelivery::Availability::Pending;
    }

    /** Borrows only CPU result state; service/public handles copy it for passive observation. */
    const TerrainImageDelivery& TerrainImageAcquisition::GetDelivery() const noexcept { return delivery_; }

    /** Pending source availability remains explicitly advanced, never a hidden asynchronous provider job. */
    bool TerrainImageAcquisition::IsPending() const noexcept
    {
        return phase_ != Phase::Done && delivery_.State == TerrainImageDelivery::Availability::Pending;
    }

    /** Dispatches exactly one bounded phase; no native/file-operation loop runs in an advancement. */
    void TerrainImageAcquisition::Advance(TerrainService& service)
    {
        if (!service.IsRequestCurrent(*this)) return;
        switch (phase_)
        {
        case Phase::Describe:
            Describe(service);
            break;
        case Phase::Inspect:
            Inspect(service);
            break;
        case Phase::AssociateCache:
            AssociateCache(service);
            break;
        case Phase::ReadCache:
            ReadCache(service);
            break;
        case Phase::Retrieve:
            Retrieve(service);
            break;
        case Phase::Store:
            Store(service);
            break;
        case Phase::Admit:
            Admit();
            break;
        case Phase::ValidateDelivery:
            ValidateDelivery(service);
            break;
        case Phase::Done:
            break;
        }
    }

    /** Drops capability/full-source owners after the final native call has retained its local shares. */
    void TerrainImageAcquisition::ReleaseSource() noexcept
    {
        provider_.reset();
        description_.Acquisition = std::monostate{};
        sourceImage_.reset();
        preparedImage_.reset();
#if defined(MODLOADER_CLIENT_BUILD)
        parameters_.MapNativeCacheKey = {};
#endif
    }

    /** Preserves the original stamp/region/source as withdrawal evidence, with no allocation or native cleanup. */
    void TerrainImageAcquisition::Withdraw() noexcept
    {
        phase_ = Phase::Done;
        delivery_.State = TerrainImageDelivery::Availability::Unavailable;
        delivery_.Image.reset();
        delivery_.Error.clear();
        ReleaseSource();
    }

    /** Makes validation/provider exceptions visible without retrying an interrupted operation. */
    void TerrainImageAcquisition::Fail(std::string error)
    {
        phase_ = Phase::Done;
        delivery_.State = TerrainImageDelivery::Availability::Failed;
        delivery_.Image.reset();
        delivery_.Error = std::move(error);
        ReleaseSource();
    }

    /** Keeps source resolution behind the selected strong registration and checks reentrant invalidation. */
    void TerrainImageAcquisition::Describe(TerrainService& service)
    {
        const auto provider = provider_;
        auto description = provider->DescribeSource(query_);
        if (!service.IsRequestCurrent(*this)) return;
        if (std::holds_alternative<std::monostate>(description.Acquisition))
        {
            Withdraw();
            delivery_.Error = "Terrain source unavailable.";
            return;
        }
        const auto* retrieval = std::get_if<ITerrainDataProvider::RetrievalSource>(&description.Acquisition);
        if (!description.Source.IsValid() || !retrieval || !retrieval->Capability ||
            retrieval->Locator.Value.empty() || !retrieval->ImageRegistration.IsValid())
        {
            Fail("A valid BC1 retrieval source is required; capture acquisition is not implemented.");
            return;
        }
        delivery_.Source = description.Source;
        description_ = std::move(description);
        provider_.reset();
        phase_ = Phase::Inspect;
    }

    /** Preserves Pending for unavailable readiness, and distinguishes changed/failed source evidence. */
    bool TerrainImageAcquisition::AcceptStatus(ITerrainSourceRetrieval::Status status)
    {
        using Status = ITerrainSourceRetrieval::Status;
        if (status == Status::Available) return true;
        if (status == Status::Unavailable)
        {
            phase_ = Phase::Inspect;
            delivery_.Error = "Terrain source not yet available.";
        }
        else if (status == Status::SourceChanged)
        {
            Withdraw();
            delivery_.Error = "Terrain source changed; replace the request.";
        }
        else Fail("Terrain source inspection or retrieval failed.");
        return false;
    }

    /** Native inspection always precedes disk lookup or retained-source reuse for this request. */
    void TerrainImageAcquisition::Inspect(TerrainService& service)
    {
        const auto retrieval = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition);
        auto inspection = retrieval.Capability->Inspect(retrieval.Locator);
        if (!service.IsRequestCurrent(*this) || !AcceptStatus(inspection.State)) return;
        inspection_ = std::move(inspection);
        if (!IsValidInspection())
        {
            Fail("Terrain inspection does not match the source or packed BC1 rectangle.");
            return;
        }
        delivery_.Error.clear();
        phase_ = Phase::AssociateCache;
    }

    /** Checks metadata without interpreting application source-state or tile indices. */
    bool TerrainImageAcquisition::IsValidInspection() const noexcept
    {
        const auto& retrieval = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition);
        const auto size = TerrainImage::CalculateByteCount(inspection_.WidthPixels, inspection_.HeightPixels);
        return size && inspection_.Source == description_.Source &&
            inspection_.Locator.Value == retrieval.Locator.Value &&
            inspection_.Encoding == ITerrainSourceRetrieval::SourceEncoding::Bc1 &&
            inspection_.RowStrideBytes == ((static_cast<std::size_t>(inspection_.WidthPixels) + 3u) / 4u) * 8u &&
            parameters_.PixelX < inspection_.WidthPixels && parameters_.PixelY < inspection_.HeightPixels &&
            parameters_.WidthPixels <= inspection_.WidthPixels - parameters_.PixelX &&
            parameters_.HeightPixels <= inspection_.HeightPixels - parameters_.PixelY &&
            GetRequestedRegistration().IsValid();
    }

    /** Gives the application only copied source/inspection evidence, never cache ownership. */
    void TerrainImageAcquisition::AssociateCache(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        const auto mapping = parameters_.MapNativeCacheKey;
        if (mapping)
        {
            const auto description = description_;
            const auto inspection = inspection_;
            const auto key = mapping(query_, description, inspection);
            if (!service.IsRequestCurrent(*this)) return;
            if (key && !IsValidCacheKey(*key))
            {
                Fail("Application cache association does not match inspected native BC1 layout.");
                return;
            }
            cacheKey_ = key;
        }
#else
        (void)service;
#endif
        phase_ = Phase::ReadCache;
    }

    /** Cache hits are independent canonical chunks, never concatenated into a full-image block layout. */
    void TerrainImageAcquisition::ReadCache(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (cacheKey_ && IsCacheChunk())
        {
            std::vector<std::uint8_t> blocks;
            const auto result = service.TryLoadNativeChunk(*cacheKey_, parameters_.PixelX / 256u,
                parameters_.PixelY / 256u, blocks);
            if (!service.IsRequestCurrent(*this)) return;
            if (result.Status == TerrainService::NativeCacheStatus::Hit)
            {
                auto image = TerrainService::IngestBc1(description_.Source, parameters_.WidthPixels,
                    parameters_.HeightPixels, GetRequestedRegistration(), inspection_.SRgb, std::move(blocks));
                if (!image) Fail("Cached terrain chunk failed canonical BC1 admission.");
                else
                {
                    preparedImage_ = std::make_shared<const TerrainImage>(*image);
                    phase_ = Phase::ValidateDelivery;
                }
                return;
            }
        }
#else
        (void)service;
#endif
        phase_ = Phase::Retrieve;
    }

    /** Exact metadata equality makes a sink submission evidence of the inspected source, not a new source. */
    bool TerrainImageAcquisition::SameInspection(const ITerrainSourceRetrieval::Inspection& left,
        const ITerrainSourceRetrieval::Inspection& right) noexcept
    {
        return left.State == right.State && left.Locator.Value == right.Locator.Value &&
            left.Source == right.Source && left.Encoding == right.Encoding &&
            left.WidthPixels == right.WidthPixels && left.HeightPixels == right.HeightPixels &&
            left.RowStrideBytes == right.RowStrideBytes && left.SRgb == right.SRgb;
    }

    /** Shares only one completed native tile, tied to this selected registration and full source evidence. */
    bool TerrainImageAcquisition::CanReuseSource(const TerrainService& service) const noexcept
    {
#if defined(MODLOADER_CLIENT_BUILD)
        const auto& retained = service.retainedNativeSource_;
        if (!cacheKey_ || retained.Provider != parameters_.Provider || retained.Key != cacheKey_ || !retained.Image)
            return false;
        const auto& image = *retained.Image;
        const auto& actual = image.GetRegistration();
        const auto& expected = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition).ImageRegistration;
        return image.GetIdentity() == inspection_.Source && image.GetWidthPixels() == inspection_.WidthPixels &&
            image.GetHeightPixels() == inspection_.HeightPixels && image.IsSRgb() == inspection_.SRgb &&
            actual.Origin.X == expected.Origin.X && actual.Origin.Y == expected.Origin.Y &&
            actual.U.X == expected.U.X && actual.U.Y == expected.U.Y &&
            actual.V.X == expected.V.X && actual.V.Y == expected.V.Y;
#else
        (void)service;
        return false;
#endif
    }

    /** Copies borrowed native bytes into canonical immutable storage before Retrieve returns. */
    void TerrainImageAcquisition::Retrieve(TerrainService& service)
    {
        if (CanReuseSource(service))
        {
            sourceImage_ = service.retainedNativeSource_.Image;
            phase_ = Phase::Admit;
            return;
        }
        const auto retrieval = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition);
        const auto inspection = inspection_;
        SourceIngestion ingestion(inspection, retrieval.ImageRegistration);
        const auto status = retrieval.Capability->Retrieve(inspection, ingestion);
        if (!service.IsRequestCurrent(*this) || !AcceptStatus(status)) return;
        sourceImage_ = ingestion.GetImage();
        if (!sourceImage_)
        {
            Fail("Successful terrain retrieval did not supply exactly one valid BC1 image.");
            return;
        }
#if defined(MODLOADER_CLIENT_BUILD)
        if (cacheKey_)
            service.retainedNativeSource_ = {parameters_.Provider, sourceImage_, cacheKey_};
#endif
        phase_ = Phase::Store;
    }

    /** Existing cache ownership handles complete compatible persistence; usable data survives write failure. */
    void TerrainImageAcquisition::Store(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (cacheKey_)
        {
            const auto image = sourceImage_;
            const auto bytes = image->GetCompressedBytes();
            const auto result = service.StoreNativeSourceTile(*cacheKey_, bytes.data(), bytes.size());
            if (!service.IsRequestCurrent(*this)) return;
            if (result.Status != TerrainService::NativeCacheStatus::Stored)
                cacheWarning_ = "Terrain cache persistence did not complete; status " +
                    std::to_string(static_cast<int>(result.Status)) + ".";
        }
#else
        (void)service;
#endif
        phase_ = Phase::Admit;
    }

    /** Reuses M2b extraction: compressed block copies preserve exact logical registration and extent. */
    void TerrainImageAcquisition::Admit()
    {
        auto image = TerrainService::ExtractImage(*sourceImage_, parameters_.PixelX, parameters_.PixelY,
            parameters_.WidthPixels, parameters_.HeightPixels);
        if (!image) Fail("Retrieved terrain rectangle failed canonical BC1 extraction.");
        else
        {
            preparedImage_ = std::make_shared<const TerrainImage>(*image);
            sourceImage_.reset();
            phase_ = Phase::ValidateDelivery;
        }
    }

    /** A cache hit or retrieval from an earlier callback cannot authorize publication of a changed source. */
    void TerrainImageAcquisition::ValidateDelivery(TerrainService& service)
    {
        const auto retrieval = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition);
        const auto current = retrieval.Capability->Inspect(retrieval.Locator);
        if (!service.IsRequestCurrent(*this)) return;
        if (current.State == ITerrainSourceRetrieval::Status::Unavailable)
        {
            delivery_.Error = "Terrain source not yet available for publication.";
            return;
        }
        if (!AcceptStatus(current.State)) return;
        if (!SameInspection(inspection_, current))
        {
            Withdraw();
            delivery_.Error = "Terrain source changed before publication.";
            return;
        }
        Publish(service, preparedImage_);
    }

    /** Final service stamp check separates CPU image ownership from later viewport GPU receipt ownership. */
    void TerrainImageAcquisition::Publish(TerrainService& service, std::shared_ptr<const TerrainImage> image)
    {
        if (!service.IsRequestCurrent(*this)) return;
        delivery_.Image = std::move(image);
        delivery_.Error = std::move(cacheWarning_);
        delivery_.State = TerrainImageDelivery::Availability::Ready;
        phase_ = Phase::Done;
        ReleaseSource();
    }

    /** Affine subimage coverage is derived from logical pixels, excluding BC1 block padding. */
    TerrainImage::Registration TerrainImageAcquisition::GetRequestedRegistration() const noexcept
    {
        const auto& original = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition).ImageRegistration;
        const double width = inspection_.WidthPixels;
        const double height = inspection_.HeightPixels;
        const double u = parameters_.WidthPixels / width;
        const double v = parameters_.HeightPixels / height;
        return {original.ImageToWorld(parameters_.PixelX / width, parameters_.PixelY / height),
            {original.U.X * u, original.U.Y * u}, {original.V.X * v, original.V.Y * v}};
    }

#if defined(MODLOADER_CLIENT_BUILD)
    /** Version-one numeric format/layout knowledge belongs to the compatible cache, not game interpretation. */
    bool TerrainImageAcquisition::IsValidCacheKey(const TerrainService::NativeCacheKey& key) const noexcept
    {
        return key.SourceState >= 0 && key.SourceWidth == Cache::NativeTerrainCacheRecord::SourceDimension &&
            key.SourceHeight == Cache::NativeTerrainCacheRecord::SourceDimension && key.PixelFormat == 5 &&
            static_cast<std::uint32_t>(key.SourceWidth) == inspection_.WidthPixels &&
            static_cast<std::uint32_t>(key.SourceHeight) == inspection_.HeightPixels && key.SRGB == inspection_.SRgb;
    }

    /** A compatible record supplies one 256-square image at a chunk-aligned pixel origin. */
    bool TerrainImageAcquisition::IsCacheChunk() const noexcept
    {
        return parameters_.WidthPixels == 256u && parameters_.HeightPixels == 256u &&
            parameters_.PixelX % 256u == 0 && parameters_.PixelY % 256u == 0;
    }
#endif
}
