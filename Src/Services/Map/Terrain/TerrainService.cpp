// Coordinates selection, explicitly advanced source acquisition and immutable BC1 deliveries.
// Delegates compatible cache IO and processing to existing owners; never accesses game-native objects or renders.
#include "TerrainService.h"
#include "Acquisition/TerrainImageAcquisition.h"
#include "Processing/TerrainBc1Codec.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Cache/TerrainFileCache.h"
#endif

#include <limits>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Services::Map::Terrain
{
    /** Creates inert metadata/cache ownership without touching the filesystem or constructing providers. */
    TerrainService::TerrainService() = default;

    /** Releases CPU-only cache configuration and registrations; application drains file operations first. */
    TerrainService::~TerrainService()
    {
        InvalidateImageRequests();
    }

    /** Shares only CPU availability/image ownership; this call never progresses acquisition. */
    TerrainImageDelivery TerrainService::ImageRequest::GetDelivery() const
    {
        return state_ ? state_->GetDelivery() : TerrainImageDelivery{};
    }

    /** Queues an exact selection-stamped demand without calling the provider or touching files. */
    TerrainService::ImageRequest TerrainService::RequestImage(ImageRequestParameters parameters)
    {
        auto state = std::make_shared<Acquisition::TerrainImageAcquisition>(std::move(parameters),
            GetProvider(selectedProvider_), physicalPosition_.value_or(ITerrainDataProvider::Position{}));
        const auto& context = state->parameters_.Context;
        if (!IsRequestCurrent(*state) || !state->parameters_.SamplePosition.IsFinite() ||
            !context.RequestGeneration || !state->parameters_.WidthPixels || !state->parameters_.HeightPixels ||
            state->parameters_.PixelX % 4u || state->parameters_.PixelY % 4u ||
            (lastRequestStamp_ && context.RequestGeneration <= lastRequestStamp_->RequestGeneration))
        {
            state->Fail("Invalid, stale or reused terrain request identity or rectangle.");
            return ImageRequest(std::move(state));
        }
        auto admittedStamp = context;
        imageRequests_.push_back(state);
        try
        {
            pendingImageRequests_.push_back(state);
        }
        catch (...)
        {
            imageRequests_.pop_back();
            throw;
        }
        lastRequestStamp_ = std::move(admittedStamp);
        return ImageRequest(std::move(state));
    }

    /** Preserves old demand unless its replacement has been successfully admitted by this service. */
    TerrainService::ImageRequest TerrainService::ReplaceImageRequest(
        const ImageRequest& previous, ImageRequestParameters parameters)
    {
        if (!previous.state_ || std::find(imageRequests_.begin(), imageRequests_.end(), previous.state_) ==
            imageRequests_.end())
        {
            auto rejected = std::make_shared<Acquisition::TerrainImageAcquisition>(std::move(parameters),
                nullptr, ITerrainDataProvider::Position{});
            rejected->Fail("Cannot replace a foreign or withdrawn terrain request.");
            return ImageRequest(std::move(rejected));
        }
        auto replacement = RequestImage(std::move(parameters));
        if (replacement.state_->IsPending())
        {
            WithdrawImageRequest(previous);
            previous.state_->delivery_.Error = "Terrain request superseded.";
        }
        return replacement;
    }

    /** Withdrawal retains the exact original stamp/region on all copies of the observation handle. */
    TerrainImageDelivery TerrainService::WithdrawImageRequest(const ImageRequest& request)
    {
        const auto found = std::find(imageRequests_.begin(), imageRequests_.end(), request.state_);
        if (found == imageRequests_.end()) return request.GetDelivery();
        request.state_->Withdraw();
        imageRequests_.erase(found);
        std::erase(pendingImageRequests_, request.state_);
        request.state_->delivery_.Error = "Terrain request withdrawn.";
        return request.GetDelivery();
    }

    /** Runs at most one request phase per application-admitted call, without a timing allowance. */
    void TerrainService::AdvanceImageRequests()
    {
        if (advancingImages_ || pendingImageRequests_.empty()) return;
        auto request = pendingImageRequests_.front();
        pendingImageRequests_.pop_front();
        if (!IsRequestCurrent(*request)) return;
        advancingImages_ = true;
        try
        {
            request->Advance(*this);
            if (request->IsPending() && IsRequestCurrent(*request)) pendingImageRequests_.push_back(request);
            advancingImages_ = false;
        }
        catch (const std::exception& error)
        {
            advancingImages_ = false;
            if (IsRequestCurrent(*request)) request->Fail(error.what());
            throw;
        }
        catch (...)
        {
            advancingImages_ = false;
            if (IsRequestCurrent(*request)) request->Fail("Terrain acquisition threw an unknown exception.");
            throw;
        }
    }

    /** Uses existing selection and delivery identity; no second native-world or generation registry exists. */
    bool TerrainService::IsRequestCurrent(const Acquisition::TerrainImageAcquisition& request) const noexcept
    {
        const auto& parameters = request.parameters_;
        return request.IsPending() && session_ && physicalPosition_ &&
            parameters.Context.WorldId == session_->WorldId &&
            parameters.Context.SessionGeneration == session_->Generation &&
            parameters.Context.SelectionRevision == selectionRevision_ &&
            parameters.Provider == selectedProvider_ && selectedProvider_.IsRegistered() &&
            parameters.RegionId == selectedRegionId_;
    }

    /** Invalidates even externally retained completed handles; no source operation or filesystem work occurs. */
    void TerrainService::InvalidateImageRequests() noexcept
    {
        for (const auto& request : imageRequests_) request->Withdraw();
        imageRequests_.clear();
        pendingImageRequests_.clear();
        retainedNativeSource_ = {};
    }

    /** Validates metadata and transfers existing blocks without decompression or recompression. */
    std::optional<TerrainImage> TerrainService::IngestBc1(TerrainImage::Identity identity,
        std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
        bool sRgb, std::vector<std::uint8_t>&& blocks)
    {
        if (!identity.IsValid() || !registration.IsValid() ||
            !Processing::TerrainBc1Codec::Validate(blocks, width, height)) return std::nullopt;
        return TerrainImage(std::move(identity), width, height, registration, sRgb, std::move(blocks));
    }

    /** Encodes transient RGB8 into the sole immutable canonical image allocation. */
    std::optional<TerrainImage> TerrainService::IngestRgb(TerrainImage::Identity identity,
        std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
        bool sRgb, std::span<const std::uint8_t> pixels, std::size_t rowStride)
    {
        if (!identity.IsValid() || !registration.IsValid()) return std::nullopt;
        std::vector<std::uint8_t> blocks;
        if (!Processing::TerrainBc1Codec::EncodeRgb(pixels, width, height, rowStride, blocks))
            return std::nullopt;
        return TerrainImage(std::move(identity), width, height, registration, sRgb, std::move(blocks));
    }

    /** Scales the affine basis by logical pixel ratios, excluding compressed block padding. */
    std::optional<TerrainImage> TerrainService::ExtractImage(const TerrainImage& image,
        std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
    {
        if (x == 0 && y == 0 && width == image.GetWidthPixels() && height == image.GetHeightPixels())
            return image;
        if (width == 0 || height == 0 || x >= image.GetWidthPixels() || y >= image.GetHeightPixels() ||
            width > image.GetWidthPixels() - x || height > image.GetHeightPixels() - y || x % 4u || y % 4u)
            return std::nullopt;
        const auto& original = image.GetRegistration();
        const double u = static_cast<double>(width) / image.GetWidthPixels();
        const double v = static_cast<double>(height) / image.GetHeightPixels();
        TerrainImage::Registration registration{original.ImageToWorld(
            static_cast<double>(x) / image.GetWidthPixels(), static_cast<double>(y) / image.GetHeightPixels()),
            {original.U.X * u, original.U.Y * u}, {original.V.X * v, original.V.Y * v}};
        if (!registration.IsValid()) return std::nullopt;
        std::vector<std::uint8_t> blocks;
        if (!Processing::TerrainBc1Codec::Extract(image.GetCompressedBytes(), image.GetWidthPixels(),
            image.GetHeightPixels(), x, y, width, height, blocks)) return std::nullopt;
        return TerrainImage(image.GetIdentity(), width, height, registration, image.IsSRgb(), std::move(blocks));
    }

#if defined(MODLOADER_CLIENT_BUILD)
    /** Installs immutable application settings before any concurrent file operation. */
    bool TerrainService::ConfigureNativeCache(const NativeCacheConfiguration& configuration)
    {
        if (fileCache_)
        {
            if (!fileCache_->HasConfiguration(configuration)) return false;
            fileCache_->Open();
            return true;
        }
        if (configuration.Root.empty() || !configuration.Root.is_absolute()) return false;
        fileCache_ = std::make_unique<Cache::TerrainFileCache>(configuration);
        return true;
    }

    /** Reports the configured subtree without creating it. */
    bool TerrainService::GetNativeCacheRoot(std::filesystem::path& root) const
    {
        if (!fileCache_) return false;
        root = fileCache_->GetRoot();
        return true;
    }

    /** Closes the instance independently of application root-cache locks and metadata. */
    void TerrainService::CloseNativeCache()
    {
        if (fileCache_) fileCache_->Close();
    }

    /** Delegates subtree clearing and concurrent IO exclusion to the cache owner. */
    bool TerrainService::ClearNativeCache()
    {
        return fileCache_ && fileCache_->Clear();
    }

    /** Forwards a numeric key to the sole file IO owner; no native source access occurs here. */
    TerrainService::NativeCacheResult TerrainService::TryLoadNativeChunk(const NativeCacheKey& key,
        int chunkX, int chunkY, std::vector<std::uint8_t>& compressedChunk, const NativeCacheTrace* trace) const
    {
        if (!fileCache_) return {};
        return fileCache_->TryLoadChunk(key, chunkX, chunkY, compressedChunk, trace);
    }

    /** Persists existing compressed source bytes without conversion or acquisition. */
    TerrainService::NativeCacheResult TerrainService::StoreNativeSourceTile(const NativeCacheKey& key,
        const std::uint8_t* sourceData, std::size_t sourceDataSize, const NativeCacheTrace* trace) const
    {
        if (!fileCache_) return {};
        return fileCache_->StoreSourceTile(key, sourceData, sourceDataSize, trace);
    }
#endif

    /** Replaces prior session ownership only after copying/validating the next identity. */
    bool TerrainService::BeginSession(const Session& session)
    {
        if (!session.IsValid()) return false;
        if (session_ && *session_ == session) return true;
        Session nextSession = session;
        AdvanceSelectionRevision();
        ClearSession();
        session_ = std::move(nextSession);
        return true;
    }

    /** Clears only the expected active session, ignoring obsolete world-end notifications. */
    bool TerrainService::StopSession(const Session& session)
    {
        if (!session_ || *session_ != session) return false;
        AdvanceSelectionRevision();
        ClearSession();
        return true;
    }

    /** Validates/copies coverage before admitting externally constructed shared provider ownership. */
    TerrainService::Registration TerrainService::RegisterProvider(
        std::shared_ptr<ITerrainDataProvider> provider)
    {
        if (!session_ || !provider) return {};
        auto coverage = provider->DescribeCoverage();
        if (!coverage.IsValid() || coverage.WorldId != session_->WorldId ||
            !CanRegister(provider.get(), coverage)) return {};
        Entry entry{std::move(provider), std::move(coverage),
            std::make_shared<const RegistrationIdentity>()};
        Registration registration(entry.Identity);
        entries_.push_back(std::move(entry));
        try
        {
            RefreshSelection();
        }
        catch (...)
        {
            entries_.pop_back();
            throw;
        }
        return registration;
    }

    /** Withdraws a local registration immediately and selects any remaining applicable source. */
    bool TerrainService::UnregisterProvider(const Registration& registration)
    {
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
        {
            if (Registration(entry->Identity) != registration) continue;
            entries_.erase(entry);
            RefreshSelection();
            return true;
        }
        return false;
    }

    /** Provides explicit shared ownership for metadata/capability use, never an unowned durable provider pointer. */
    std::shared_ptr<ITerrainDataProvider> TerrainService::GetProvider(
        const Registration& registration) const
    {
        const auto* entry = FindEntry(registration);
        return entry ? entry->Provider : nullptr;
    }

    /** Records finite current-session physical position; stale notifications cannot replace current selection. */
    bool TerrainService::UpdatePlayerPosition(const PhysicalPlayerSample& sample)
    {
        if (!session_ || *session_ != sample.Context) return false;
        if (!sample.WorldPosition.IsFinite())
        {
            physicalPosition_.reset();
            RefreshSelection();
            return false;
        }
        physicalPosition_ = sample.WorldPosition;
        RefreshSelection();
        return true;
    }

    /** Returns copied observation identity without transferring service registration ownership. */
    std::optional<TerrainService::Selection> TerrainService::GetSelection() const
    {
        if (!session_ || !selectedProvider_.IsRegistered()) return std::nullopt;
        return Selection{*session_, selectedProvider_, selectedRegionId_, selectionRevision_};
    }

    /** Locates an entry by its unique weak ownership identity, also rejecting foreign-service handles. */
    const TerrainService::Entry* TerrainService::FindEntry(
        const Registration& registration) const noexcept
    {
        for (const auto& entry : entries_)
        {
            if (Registration(entry.Identity) == registration) return &entry;
        }
        return nullptr;
    }

    /** Rejects duplicate objects/identifiers/defaults and ambiguous alternate-region coverage. */
    bool TerrainService::CanRegister(const ITerrainDataProvider* provider,
        const ITerrainDataProvider::Coverage& coverage) const noexcept
    {
        for (const auto& entry : entries_)
        {
            if (entry.Provider.get() == provider || entry.Coverage.ProviderId == coverage.ProviderId)
                return false;
            if (entry.Coverage.Role == ITerrainDataProvider::CoverageRole::Default &&
                coverage.Role == ITerrainDataProvider::CoverageRole::Default) return false;
            if (HasRegionConflict(entry.Coverage, coverage)) return false;
        }
        return true;
    }

    /** Detects duplicate region IDs and positive-volume intersections, including regions of one world session. */
    bool TerrainService::HasRegionConflict(const ITerrainDataProvider::Coverage& left,
        const ITerrainDataProvider::Coverage& right) noexcept
    {
        for (const auto& leftRegion : left.Regions)
        {
            for (const auto& rightRegion : right.Regions)
            {
                if (leftRegion.Id == rightRegion.Id ||
                    leftRegion.SelectionBounds.Overlaps(rightRegion.SelectionBounds)) return true;
            }
        }
        return false;
    }

    /** Uses mutually exclusive alternate boxes first; a lone default requires a valid physical sample too. */
    void TerrainService::RefreshSelection()
    {
        if (!session_ || !physicalPosition_)
        {
            SetSelection({}, {});
            return;
        }
        const Entry* defaultEntry = nullptr;
        for (const auto& entry : entries_)
        {
            if (entry.Coverage.Role == ITerrainDataProvider::CoverageRole::Default)
                defaultEntry = &entry;
            for (const auto& region : entry.Coverage.Regions)
            {
                if (!region.SelectionBounds.Contains(*physicalPosition_)) continue;
                SetSelection(Registration(entry.Identity), region.Id);
                return;
            }
        }
        SetSelection(defaultEntry ? Registration(defaultEntry->Identity) : Registration{}, {});
    }

    /** Preserves revision on same-region motion; changes carry a newly issued observation revision. */
    void TerrainService::SetSelection(Registration registration, const std::string& regionId)
    {
        if (selectedProvider_ == registration && selectedRegionId_ == regionId) return;
        std::string nextRegionId = regionId;
        AdvanceSelectionRevision();
        InvalidateImageRequests();
        selectedProvider_ = std::move(registration);
        selectedRegionId_.swap(nextRegionId);
    }

    /** Fails explicitly before observation identity could wrap and alias an older selection. */
    void TerrainService::AdvanceSelectionRevision()
    {
        if (selectionRevision_ == (std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error("Terrain selection revision exhausted.");
        ++selectionRevision_;
    }

    /** Invalidates all handles before releasing providers; retains no synthetic async retirement state. */
    void TerrainService::ClearSession() noexcept
    {
        InvalidateImageRequests();
        lastRequestStamp_.reset();
        selectedProvider_ = {};
        selectedRegionId_.clear();
        physicalPosition_.reset();
        entries_.clear();
        session_.reset();
    }
}
