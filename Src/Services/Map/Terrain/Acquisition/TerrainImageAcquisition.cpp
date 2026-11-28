// Implements demand-driven BC1 source inspection, private page cache coordination and immutable delivery.
// Each advancement completes available work for one request; native/provider calls remain synchronous and application-admitted.
#include "TerrainImageAcquisition.h"
#include "../Processing/TerrainBc1Codec.h"
#include <algorithm>
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
                !TerrainImage::IsValidSourceDimensions(source.WidthPixels, source.HeightPixels) ||
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

    /** Completes immediately available work for one request; unavailable/backward transitions yield.
     * Each successful transition moves forward through the finite acquisition graph. Synchronous
     * provider and filesystem calls are non-preemptible; this is not an execution-time guarantee.
     */
    void TerrainImageAcquisition::Advance(TerrainService& service)
    {
        while (service.IsRequestCurrent(*this))
        {
            const auto previous = phase_;
            AdvancePhase(service);
            if (phase_ <= previous) return;
        }
    }

    /** Dispatches one phase; the caller rechecks admission and yields on unavailable readiness. */
    void TerrainImageAcquisition::AdvancePhase(TerrainService& service)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        if (service.imageRequestTrace_ && service.imageRequestTrace_->Progress)
            service.imageRequestTrace_->Progress(service.imageRequestTrace_->Cache.Context, false);
#endif
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
        case Phase::WaitForCaptureBatch:
            WaitForCaptureBatch(service);
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
            AdmitObserved(service);
#else
            Admit();
#endif
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
        const auto* capture = std::get_if<ITerrainDataProvider::CaptureDefinition>(&description.Acquisition);
        if (capture && !TerrainImage::IsValidSourceDimensions(capture->WidthPixels, capture->HeightPixels))
        {
            Fail("Terrain source dimensions must be positive multiples of 256 within image limits.");
            return;
        }
#if defined(MODLOADER_CLIENT_BUILD)
        if (capture)
        {
            if (description.Source != capture->Source || description.Page != capture->Page ||
                !capture->ImageRegistration.IsValid() ||
                parameters_.PixelX >= capture->WidthPixels || parameters_.PixelY >= capture->HeightPixels ||
                parameters_.WidthPixels > capture->WidthPixels - parameters_.PixelX ||
                parameters_.HeightPixels > capture->HeightPixels - parameters_.PixelY ||
                parameters_.PixelX % 4u || parameters_.PixelY % 4u)
            {
                Fail("Terrain capture definition does not match the admitted source or requested rectangle.");
                return;
            }
            delivery_.Source = description.Source;
            description_ = std::move(description);
            provider_.reset();
            phase_ = Phase::AssociateCache;
            return;
        }
#endif
        const auto* retrieval = std::get_if<ITerrainDataProvider::RetrievalSource>(&description.Acquisition);
        if (!description.Source.IsValid() || !retrieval || !retrieval->Capability ||
            retrieval->Locator.Value.empty() || !retrieval->ImageRegistration.IsValid())
        {
#if defined(MODLOADER_CLIENT_BUILD)
            Fail("A valid BC1 retrieval source is required.");
#else
            Fail("A valid BC1 retrieval source is required; capture acquisition is not implemented.");
#endif
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

    /** Source inspection always precedes disk lookup or retained-source reuse for this request. */
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
        return size && TerrainImage::IsValidSourceDimensions(inspection_.WidthPixels, inspection_.HeightPixels) &&
            inspection_.Source == description_.Source && inspection_.Page == description_.Page &&
            inspection_.Locator.Value == retrieval.Locator.Value &&
            inspection_.Encoding == ITerrainSourceRetrieval::SourceEncoding::Bc1 &&
            inspection_.RowStrideBytes == ((static_cast<std::size_t>(inspection_.WidthPixels) + 3u) / 4u) * 8u &&
            parameters_.PixelX < inspection_.WidthPixels && parameters_.PixelY < inspection_.HeightPixels &&
            parameters_.WidthPixels <= inspection_.WidthPixels - parameters_.PixelX &&
            parameters_.HeightPixels <= inspection_.HeightPixels - parameters_.PixelY &&
            GetRequestedRegistration().IsValid();
    }

    /** Derives private cache evidence without another provider/native call; preserves this explicit phase. */
    void TerrainImageAcquisition::AssociateCache(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        Cache::TerrainCacheIdentity identity;
        if (const auto* capture = std::get_if<ITerrainDataProvider::CaptureDefinition>(&description_.Acquisition))
        {
            if (service.CreateCaptureCacheIdentity(*capture, identity)) cacheKey_ = std::move(identity);
        }
        else
        {
            const auto& retrieval = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition);
            identity = {inspection_.Source, inspection_.Locator.Value,
                inspection_.WidthPixels, inspection_.HeightPixels, inspection_.Encoding,
                inspection_.SRgb, retrieval.ImageRegistration, inspection_.Page};
            if (identity.IsValid()) cacheKey_ = std::move(identity);
        }
#else
        (void)service;
#endif
        phase_ = Phase::ReadCache;
    }

    /** Separates retrieval tile misses from capture page misses without starting capture work. */
    void TerrainImageAcquisition::ReadCache(TerrainService& service)
    {
        if (const auto* capture = std::get_if<ITerrainDataProvider::CaptureDefinition>(&description_.Acquisition))
        {
            ReadCaptureCache(service, *capture);
            return;
        }
#if defined(MODLOADER_CLIENT_BUILD)
        if (cacheKey_ && IsCacheChunk())
        {
            std::vector<std::uint8_t> blocks;
            const auto result = service.TryLoadCacheTile(*cacheKey_,
                parameters_.PixelX / Cache::TerrainCacheIdentity::TileDimension,
                parameters_.PixelY / Cache::TerrainCacheIdentity::TileDimension, blocks
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
                , service.imageRequestTrace_ ? &service.imageRequestTrace_->Cache : nullptr
#endif
            );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
            if (service.imageRequestTrace_ && service.imageRequestTrace_->ReadResult)
                service.imageRequestTrace_->ReadResult(service.imageRequestTrace_->Cache.Context, result);
#endif
            if (!service.IsRequestCurrent(*this)) return;
            if (result.Status == TerrainService::CacheStatus::Hit)
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

    /** Reads every existing cache tile into one temporary canonical page, then extracts the demand. */
    void TerrainImageAcquisition::ReadCaptureCache(TerrainService& service,
        const ITerrainDataProvider::CaptureDefinition& definition)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (IsCacheChunk())
        {
            ReadCaptureCacheChunk(service, definition);
            return;
        }
        ReadCaptureCachePage(service, definition);
#else
        (void)service;
        (void)definition;
        ReportCaptureCacheUnavailable("Capture cache is unavailable in this build.");
#endif
    }

#if defined(MODLOADER_CLIENT_BUILD)
    /** Reads one existing cache tile and admits it with the capture's requested affine registration. */
    void TerrainImageAcquisition::ReadCaptureCacheChunk(TerrainService& service,
        const ITerrainDataProvider::CaptureDefinition& definition)
    {
        if (!cacheKey_)
        {
            ReportCaptureCacheUnavailable("Capture cache identity is incompatible with the existing page cache.");
            return;
        }
        std::vector<std::uint8_t> blocks;
        const auto result = service.TryLoadCacheTile(*cacheKey_,
            parameters_.PixelX / Cache::TerrainCacheIdentity::TileDimension,
            parameters_.PixelY / Cache::TerrainCacheIdentity::TileDimension, blocks
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
            , service.imageRequestTrace_ ? &service.imageRequestTrace_->Cache : nullptr
#endif
        );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        if (service.imageRequestTrace_ && service.imageRequestTrace_->ReadResult)
            service.imageRequestTrace_->ReadResult(service.imageRequestTrace_->Cache.Context, result);
#endif
        if (!service.IsRequestCurrent(*this)) return;
        if (result.Status != TerrainService::CacheStatus::Hit)
        {
            std::string reason;
            if (result.Status == TerrainService::CacheStatus::Miss)
                reason = "Capture cache miss.";
            else
                reason = "Capture cache read unavailable; status " +
                    std::to_string(static_cast<int>(result.Status)) + ".";
            WaitForCaptureOutputOrReportUnavailable(service, definition, std::move(reason));
            return;
        }
        auto image = TerrainService::IngestBc1(definition.Source, parameters_.WidthPixels,
            parameters_.HeightPixels, GetRequestedRegistration(), definition.SRgb, std::move(blocks));
        if (!image)
        {
            Fail("Cached capture tile failed canonical BC1 admission.");
            return;
        }
        preparedImage_ = std::make_shared<const TerrainImage>(*image);
        phase_ = Phase::ValidateDelivery;
    }

    /** Reconstructs the existing page only for a demand that spans multiple cache tiles. */
    void TerrainImageAcquisition::ReadCaptureCachePage(TerrainService& service,
        const ITerrainDataProvider::CaptureDefinition& definition)
    {
        if (!cacheKey_)
        {
            ReportCaptureCacheUnavailable("Capture cache identity is incompatible with the existing page cache.");
            return;
        }
        std::vector<std::uint8_t> page;
        TerrainService::CacheResult result;
        if (!LoadCaptureCacheTiles(service, definition, page, result))
        {
            if (!service.IsRequestCurrent(*this)) return;
            std::string reason;
            if (result.Status == TerrainService::CacheStatus::Miss)
                reason = "Capture cache miss.";
            else
                reason = "Capture cache read unavailable; status " +
                    std::to_string(static_cast<int>(result.Status)) + ".";
            WaitForCaptureOutputOrReportUnavailable(service, definition, std::move(reason));
            return;
        }
        auto fullImage = TerrainService::IngestBc1(definition.Source, definition.WidthPixels,
            definition.HeightPixels, definition.ImageRegistration, definition.SRgb, std::move(page));
        if (!fullImage)
        {
            Fail("Cached capture page failed canonical BC1 admission.");
            return;
        }
        auto image = TerrainService::ExtractImage(*fullImage, parameters_.PixelX, parameters_.PixelY,
            parameters_.WidthPixels, parameters_.HeightPixels);
        if (!image)
        {
            Fail("Cached capture page failed canonical image extraction.");
            return;
        }
        preparedImage_ = std::make_shared<const TerrainImage>(*image);
        phase_ = Phase::ValidateDelivery;
    }

    /** Copies one complete cache page in Y-major/X-minor 256-square tile order. */
    bool TerrainImageAcquisition::LoadCaptureCacheTiles(TerrainService& service,
        const ITerrainDataProvider::CaptureDefinition& definition, std::vector<std::uint8_t>& page,
        TerrainService::CacheResult& result)
    {
        const auto pageByteCount = TerrainImage::CalculateByteCount(definition.WidthPixels, definition.HeightPixels);
        if (!pageByteCount)
        {
            result.Status = TerrainService::CacheStatus::InvalidInput;
            return false;
        }
        page.resize(*pageByteCount);
        const std::uint32_t tilesPerAxis = definition.WidthPixels / Cache::TerrainCacheIdentity::TileDimension;
        const std::size_t pageRowBytes = (static_cast<std::size_t>(definition.WidthPixels) / 4u) * 8u;
        const std::size_t tileRowBytes = (Cache::TerrainCacheIdentity::TileDimension / 4u) * 8u;
        for (std::uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY)
        {
            for (std::uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX)
            {
                std::vector<std::uint8_t> tile;
                result = service.TryLoadCacheTile(*cacheKey_, static_cast<int>(tileX),
                    static_cast<int>(tileY), tile
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
                    , service.imageRequestTrace_ ? &service.imageRequestTrace_->Cache : nullptr
#endif
                );
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
                if (service.imageRequestTrace_ && service.imageRequestTrace_->ReadResult)
                    service.imageRequestTrace_->ReadResult(service.imageRequestTrace_->Cache.Context, result);
#endif
                if (!service.IsRequestCurrent(*this)) return false;
                if (result.Status != TerrainService::CacheStatus::Hit)
                {
                    return false;
                }
                for (std::size_t row = 0; row < Cache::TerrainCacheIdentity::TileDimension / 4u; ++row)
                {
                    const std::size_t destinationOffset =
                        (static_cast<std::size_t>(tileY *
                            (Cache::TerrainCacheIdentity::TileDimension / 4u)) + row) * pageRowBytes +
                        tileX * tileRowBytes;
                    const std::size_t sourceOffset = row * tileRowBytes;
                    std::copy_n(tile.data() + sourceOffset, tileRowBytes, page.data() + destinationOffset);
                }
            }
        }
        return true;
    }
#endif

    /** Ends capture image demand as an explicit unavailable cache-only request. */
    void TerrainImageAcquisition::ReportCaptureCacheUnavailable(std::string reason)
    {
        delivery_.State = TerrainImageDelivery::Availability::Unavailable;
        delivery_.Error = std::move(reason);
        phase_ = Phase::Done;
        ReleaseSource();
    }

    /** Keeps a miss pending only while this session's exact output is awaiting admission or generation. */
    void TerrainImageAcquisition::WaitForCaptureOutputOrReportUnavailable(TerrainService& service,
        const ITerrainDataProvider::CaptureDefinition& definition, std::string reason)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        const auto state = service.GetPrivateCaptureOutputState(definition);
        if (state == TerrainService::CaptureOutputState::AwaitingBatchAdmission ||
            state == TerrainService::CaptureOutputState::Generating)
        {
            delivery_.Error = std::move(reason);
            phase_ = Phase::WaitForCaptureBatch;
            return;
        }
        ReportCaptureCacheUnavailable(std::move(reason));
#else
        (void)service;
        (void)definition;
        ReportCaptureCacheUnavailable(std::move(reason));
#endif
    }

    /** Advances through the existing pending request queue without touching the filesystem. */
    void TerrainImageAcquisition::WaitForCaptureBatch(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        const auto* definition = std::get_if<ITerrainDataProvider::CaptureDefinition>(
            &description_.Acquisition);
        if (!definition)
        {
            Fail("Capture batch wait lost its capture definition.");
            return;
        }
        const auto state = service.GetPrivateCaptureOutputState(*definition);
        if (state == TerrainService::CaptureOutputState::AwaitingBatchAdmission ||
            state == TerrainService::CaptureOutputState::Generating)
            return;
        if (state == TerrainService::CaptureOutputState::CacheReady)
        {
            delivery_.Error.clear();
            phase_ = Phase::ReadCache;
            return;
        }
        std::string reason = "Capture batch ended before the requested image became available.";
        if (!delivery_.Error.empty()) reason = std::move(delivery_.Error);
        ReportCaptureCacheUnavailable(std::move(reason));
#else
        (void)service;
        ReportCaptureCacheUnavailable("Capture batch is unavailable in this build.");
#endif
    }

    /** Exact metadata equality makes a sink submission evidence of the inspected source, not a new source. */
    bool TerrainImageAcquisition::SameInspection(const ITerrainSourceRetrieval::Inspection& left,
        const ITerrainSourceRetrieval::Inspection& right) noexcept
    {
        return left.State == right.State && left.Locator.Value == right.Locator.Value &&
            left.Source == right.Source && left.Encoding == right.Encoding &&
            left.WidthPixels == right.WidthPixels && left.HeightPixels == right.HeightPixels &&
            left.RowStrideBytes == right.RowStrideBytes && left.SRgb == right.SRgb && left.Page == right.Page;
    }

    /** Reuses one full image only for the issuing registration and all inspected source/placement evidence. */
    bool TerrainImageAcquisition::CanReuseSource(const TerrainService& service) const noexcept
    {
        const auto& retained = service.retainedSource_;
        if (retained.Provider != parameters_.Provider || !retained.Image ||
            !SameInspection(retained.Inspection, inspection_)) return false;
        const auto& image = *retained.Image;
        const auto& actual = image.GetRegistration();
        const auto& expected = std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition).ImageRegistration;
        return image.GetIdentity() == inspection_.Source && image.GetWidthPixels() == inspection_.WidthPixels &&
            image.GetHeightPixels() == inspection_.HeightPixels && image.IsSRgb() == inspection_.SRgb &&
            actual.Origin.X == expected.Origin.X && actual.Origin.Y == expected.Origin.Y &&
            actual.U.X == expected.U.X && actual.U.Y == expected.U.Y &&
            actual.V.X == expected.V.X && actual.V.Y == expected.V.Y;
    }

    /** Copies borrowed native bytes into canonical immutable storage before Retrieve returns. */
    void TerrainImageAcquisition::Retrieve(TerrainService& service)
    {
        if (CanReuseSource(service))
        {
            sourceImage_ = service.retainedSource_.Image;
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
        service.retainedSource_ = {parameters_.Provider, sourceImage_, inspection_};
        phase_ = Phase::Store;
    }

    /** Service cache ownership handles complete page persistence; usable data survives write failure. */
    void TerrainImageAcquisition::Store(TerrainService& service)
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (cacheKey_)
        {
            const auto image = sourceImage_;
            const auto bytes = image->GetCompressedBytes();
            const auto result = service.StoreCachePage(*cacheKey_, bytes.data(), bytes.size()
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
                , service.imageRequestTrace_ ? &service.imageRequestTrace_->Cache : nullptr
#endif
            );
            if (!service.IsRequestCurrent(*this)) return;
            if (result.Status != TerrainService::CacheStatus::Stored)
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
    /** Observation spans this phase only; staged work in other callbacks is not charged again. */
    void TerrainImageAcquisition::AdmitObserved(TerrainService& service)
    {
        const auto* trace = service.imageRequestTrace_;
        if (trace && trace->Extraction) trace->Extraction(trace->Cache.Context, true);
        try
        {
            Admit();
        }
        catch (...)
        {
            if (trace && trace->Extraction) trace->Extraction(trace->Cache.Context, false);
            throw;
        }
        if (trace && trace->Extraction) trace->Extraction(trace->Cache.Context, false);
    }
#endif

    /** A cache hit or retrieval from an earlier callback cannot authorize publication of a changed source. */
    void TerrainImageAcquisition::ValidateDelivery(TerrainService& service)
    {
        if (const auto* capture = std::get_if<ITerrainDataProvider::CaptureDefinition>(&description_.Acquisition))
        {
            if (!service.IsRequestCurrent(*this)) return;
            const auto& image = *preparedImage_;
            const auto& registration = image.GetRegistration();
            const auto expectedRegistration = GetRequestedRegistration();
            if (image.GetIdentity() != capture->Source ||
                image.GetWidthPixels() != parameters_.WidthPixels ||
                image.GetHeightPixels() != parameters_.HeightPixels || image.IsSRgb() != capture->SRgb ||
                registration.Origin.X != expectedRegistration.Origin.X ||
                registration.Origin.Y != expectedRegistration.Origin.Y ||
                registration.U.X != expectedRegistration.U.X ||
                registration.U.Y != expectedRegistration.U.Y ||
                registration.V.X != expectedRegistration.V.X ||
                registration.V.Y != expectedRegistration.V.Y ||
                !Processing::TerrainBc1Codec::Validate(image.GetCompressedBytes(),
                    image.GetWidthPixels(), image.GetHeightPixels()))
            {
                Fail("Cached capture image no longer matches its admitted definition.");
                return;
            }
            Publish(service, preparedImage_);
            return;
        }
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        if (service.imageRequestTrace_ && service.imageRequestTrace_->Progress)
            service.imageRequestTrace_->Progress(service.imageRequestTrace_->Cache.Context, true);
#endif
        phase_ = Phase::Done;
        ReleaseSource();
    }

    /** Affine subimage coverage is derived from logical pixels, excluding BC1 block padding. */
    TerrainImage::Registration TerrainImageAcquisition::GetRequestedRegistration() const noexcept
    {
        const auto* capture = std::get_if<ITerrainDataProvider::CaptureDefinition>(&description_.Acquisition);
        const auto& original = capture ? capture->ImageRegistration :
            std::get<ITerrainDataProvider::RetrievalSource>(description_.Acquisition).ImageRegistration;
        const double width = capture ? capture->WidthPixels : inspection_.WidthPixels;
        const double height = capture ? capture->HeightPixels : inspection_.HeightPixels;
        const double u = parameters_.WidthPixels / width;
        const double v = parameters_.HeightPixels / height;
        return {original.ImageToWorld(parameters_.PixelX / width, parameters_.PixelY / height),
            {original.U.X * u, original.U.Y * u}, {original.V.X * v, original.V.Y * v}};
    }

#if defined(MODLOADER_CLIENT_BUILD)
    /** A compatible record supplies one 256-square image at a chunk-aligned pixel origin. */
    bool TerrainImageAcquisition::IsCacheChunk() const noexcept
    {
        return parameters_.WidthPixels == Cache::TerrainCacheIdentity::TileDimension &&
            parameters_.HeightPixels == Cache::TerrainCacheIdentity::TileDimension &&
            parameters_.PixelX % Cache::TerrainCacheIdentity::TileDimension == 0 &&
            parameters_.PixelY % Cache::TerrainCacheIdentity::TileDimension == 0;
    }
#endif
}
