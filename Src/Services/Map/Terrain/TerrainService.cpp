// Coordinates terrain selection, immutable BC1 delivery and opaque private capture-session requests.
// Native capture and residency operations remain owned by the existing capture session classes.
#include "TerrainService.h"
#include "Acquisition/TerrainImageAcquisition.h"
#include "Processing/TerrainBc1Codec.h"
#include "Services/Log.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Cache/TerrainCacheRecord.h"
#include "Cache/TerrainFileCache.h"
#endif
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
#include "Services/Map/Buildings/Cache/SpritePngCodec.h"
#include <fstream>
#endif

#include <limits>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Services::Map::Terrain
{
    /** Creates inert metadata/cache ownership without touching the filesystem or constructing providers. */
    TerrainService::TerrainService() = default;

    /** Releases CPU state; the application first acknowledges private native shutdown and drains cache operations. */
    TerrainService::~TerrainService()
    {
        InvalidateImageRequests();
    }

#if defined(MODLOADER_CLIENT_BUILD)

    /** Keeps one DLL-lifetime native owner even when a failed native submission leaves retirement debt. */
    bool TerrainService::RequestPrivateStreaming(const ITerrainDataProvider::CaptureDefinition& definition,
        SDK::UWorld* gameplayWorld, MiniMap::Services::INativeHooks& hooks)
    {
        if (!session_ || privateStreaming_ ||
            (privateCaptureBatch_.Status == CaptureBatchStatus::Running && !advancingCaptureBatch_))
            return false;
        auto streaming = Capture::PrivateStreamingDefinition::FromCaptureDefinition(definition);
        if (!streaming)
            return false;
        privateStreaming_ = std::make_unique<Capture::PrivateStreamingSession>();
        const bool accepted = privateStreaming_->Submit(std::move(*streaming), gameplayWorld, session_->Generation,
            definition, hooks);
        if (!accepted && privateStreaming_->GetStatus() == Capture::PrivateStreamingSession::Status::Inactive &&
            privateStreaming_->TryShutdown())
        {
            privateStreaming_.reset();
        }
        return accepted;
    }

    /** Routes one capture through the current gameplay generation and retained session owner. */
    bool TerrainService::RequestPrivateCapture(const ITerrainDataProvider::CaptureDefinition& definition)
    {
        if (!session_ || !privateStreaming_ ||
            (privateCaptureBatch_.Status == CaptureBatchStatus::Running && !advancingCaptureBatch_)) return false;
        const bool batchSubmission = privateCaptureBatch_.Status == CaptureBatchStatus::Running;
        const auto batchContext = privateCaptureBatch_.Context;
        auto admittedDefinition = definition;
        bool retainDiagnosticPixels = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        retainDiagnosticPixels = exportCaptureDiagnostics_;
#endif
        if (!privateStreaming_->RequestCapture(definition, session_->Generation, retainDiagnosticPixels))
            return false;
        if (batchSubmission && (privateCaptureBatch_.Status != CaptureBatchStatus::Running ||
            !session_ || *session_ != batchContext)) return false;
        privateCaptureDefinition_ = std::move(admittedDefinition);
        privateCaptureCacheResult_ = {};
        return true;
    }

    /** Transfers immutable completed output without exposing the private capture world or native target. */
    std::shared_ptr<const TerrainImage> TerrainService::TakePrivateCaptureImage()
    {
        if (!privateStreaming_ ||
            (privateCaptureBatch_.Status == CaptureBatchStatus::Running && !advancingCaptureBatch_)) return nullptr;
        auto image = privateStreaming_->TakeCompletedImage();
        if (!image || !privateCaptureDefinition_) return image;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (exportCaptureDiagnostics_ && MatchesCaptureDefinition(*image, *privateCaptureDefinition_))
        {
            const auto diagnosticPixels = privateStreaming_->TakeCompletedDiagnosticPixels();
            ExportPreCompressionCapture(diagnosticPixels, *privateCaptureDefinition_);
        }
#endif
        privateCaptureCacheResult_ = PersistCaptureImage(*image, *privateCaptureDefinition_);
        privateCaptureDefinition_.reset();
        return image;
    }

    /** Returns the exact persistence result without changing capture or native ownership. */
    TerrainService::CacheResult TerrainService::GetPrivateCaptureCacheResult() const noexcept
    {
        return privateCaptureCacheResult_;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Applies application-owned Debug export configuration without performing file operations. */
    void TerrainService::ConfigureCaptureDiagnosticExport(bool enabled) noexcept
    {
        exportCaptureDiagnostics_ = enabled;
    }
#endif

    /** Associates captures through stable output metadata and the provider's established page naming. */
    bool TerrainService::CreateCaptureCacheIdentity(
        const ITerrainDataProvider::CaptureDefinition& definition,
        Cache::TerrainCacheIdentity& identity) const
    {
        identity = {definition.Source, definition.Source.SourceId,
            definition.WidthPixels, definition.HeightPixels, ITerrainSourceRetrieval::SourceEncoding::Bc1,
            definition.SRgb, definition.ImageRegistration, definition.Page};
        return identity.IsValid();
    }

    /** Requires exact output, placement, interpretation, dimensions and canonical packed blocks. */
    bool TerrainService::MatchesCaptureDefinition(const TerrainImage& image,
        const ITerrainDataProvider::CaptureDefinition& definition) const
    {
        const auto& actual = image.GetRegistration();
        const auto& expected = definition.ImageRegistration;
        return definition.Source.IsValid() && expected.IsValid() &&
            TerrainImage::IsValidSourceDimensions(definition.WidthPixels, definition.HeightPixels) &&
            image.GetIdentity() == definition.Source &&
            image.GetWidthPixels() == definition.WidthPixels && image.GetHeightPixels() == definition.HeightPixels &&
            image.IsSRgb() == definition.SRgb && actual.Origin.X == expected.Origin.X &&
            actual.Origin.Y == expected.Origin.Y && actual.U.X == expected.U.X && actual.U.Y == expected.U.Y &&
            actual.V.X == expected.V.X && actual.V.Y == expected.V.Y &&
            Processing::TerrainBc1Codec::Validate(image.GetCompressedBytes(), definition.WidthPixels,
                definition.HeightPixels);
    }

    /** Keeps valid image ownership independent from cache admission, naming or filesystem failure. */
    TerrainService::CacheResult TerrainService::PersistCaptureImage(const TerrainImage& image,
        const ITerrainDataProvider::CaptureDefinition& definition)
    {
        if (!MatchesCaptureDefinition(image, definition)) return {CacheStatus::InvalidInput};
        try
        {
            Cache::TerrainCacheIdentity identity;
            if (!CreateCaptureCacheIdentity(definition, identity)) return {CacheStatus::InvalidInput};
            const auto blocks = image.GetCompressedBytes();
            return StoreCachePage(identity, blocks.data(), blocks.size());
        }
        catch (...)
        {
            return {CacheStatus::WriteFailure};
        }
    }

    /** Keeps exact capture validation authoritative independently of cache or optional export outcomes. */
    bool TerrainService::ValidateCompletedCaptureImage(const TerrainImage& image,
        const ITerrainDataProvider::CaptureDefinition& definition) const
    {
        if (!MatchesCaptureDefinition(image, definition)) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ExportCompletedCapture(image, definition);
#endif
        return true;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Writes one diagnostic payload and keeps every filesystem error outside capture outcome. */
    bool TerrainService::WriteCaptureDiagnosticFile(const std::filesystem::path& path,
        std::span<const std::uint8_t> bytes, const std::string& sourceId, const char* format) noexcept
    {
        try
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("open failed");
            output.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) throw std::runtime_error("write or close failed");
            return true;
        }
        catch (const std::exception& error)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=%s reason=%s",
                    sourceId.c_str(), format, error.what());
            return false;
        }
        catch (...)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=%s",
                    sourceId.c_str(), format);
            return false;
        }
    }

    /** Resolves a safe page basename and Diagnostics folder using the configured terrain cache root. */
    bool TerrainService::GetCaptureDiagnosticPath(
        const ITerrainDataProvider::CaptureDefinition& definition,
        std::filesystem::path& directory, std::wstring& baseName) const
    {
        std::filesystem::path terrainCacheRoot;
        if (!GetTerrainCacheRoot(terrainCacheRoot) ||
            !Cache::TerrainCacheRecord::GetFilename(definition.Page, baseName))
            return false;
        const std::wstring terrainSuffix = L".terrain";
        if (baseName.size() < terrainSuffix.size())
            return false;
        baseName.resize(baseName.size() - terrainSuffix.size());
        if (definition.Page.FilenameStem.empty() && definition.Page.X == 0 && definition.Page.Y == 0)
        {
            const std::wstring originSuffix = L"_0_0";
            if (baseName.size() < originSuffix.size())
                return false;
            baseName.resize(baseName.size() - originSuffix.size());
        }
        directory = terrainCacheRoot / L"Diagnostics";
        return true;
    }

    /** Converts borrowed opaque RGB bytes to RGBA and reuses the existing WIC PNG encoder. */
    bool TerrainService::WriteCaptureDiagnosticRgbPng(const std::filesystem::path& path,
        std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
        const std::string& sourceId) noexcept
    {
        try
        {
            if (!TerrainImage::IsValidSourceDimensions(width, height))
                throw std::runtime_error("invalid RGB PNG dimensions");
            const auto pixelCount = static_cast<std::size_t>(width) * height;
            if (pixelCount > (std::numeric_limits<std::size_t>::max)() / 4u || rgb.size() != pixelCount * 3u)
                throw std::runtime_error("invalid RGB PNG pixel buffer");
            std::vector<std::uint8_t> rgba(pixelCount * 4u);
            for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                rgba[pixel * 4u] = rgb[pixel * 3u];
                rgba[pixel * 4u + 1u] = rgb[pixel * 3u + 1u];
                rgba[pixel * 4u + 2u] = rgb[pixel * 3u + 2u];
                rgba[pixel * 4u + 3u] = 255;
            }
            return WriteCaptureDiagnosticRgbaPng(path, rgba, width, height, sourceId);
        }
        catch (const std::exception& error)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=raw-png reason=%s",
                    sourceId.c_str(), error.what());
            return false;
        }
        catch (...)
        {
            return false;
        }
    }

    /** Encodes opaque RGBA with the existing WIC codec and isolates encoder failures from capture status. */
    bool TerrainService::WriteCaptureDiagnosticRgbaPng(const std::filesystem::path& path,
        std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
        const std::string& sourceId) noexcept
    {
        try
        {
            if (!TerrainImage::IsValidSourceDimensions(width, height) ||
                rgba.size() != static_cast<std::size_t>(width) * height * 4u)
                throw std::runtime_error("invalid RGBA PNG input");
            std::vector<std::uint8_t> png;
            const auto encoded = ::Services::Map::Buildings::Cache::SpritePngCodec::Encode(
                rgba, width, height, png);
            if (encoded.Status != ::Services::Map::Buildings::Cache::SpritePngCodec::ResultStatus::Complete)
                throw std::runtime_error("PNG encode failed");
            return WriteCaptureDiagnosticFile(path, png, sourceId, "png");
        }
        catch (const std::exception& error)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=png reason=%s",
                    sourceId.c_str(), error.what());
            return false;
        }
        catch (...)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=png",
                    sourceId.c_str());
            return false;
        }
    }

    /** Decodes canonical BC1 and saves the same opaque RGB interpretation used by TerrainViewport. */
    bool TerrainService::WriteCaptureDiagnosticPng(const std::filesystem::path& path,
        const TerrainImage& image, const std::string& sourceId) noexcept
    {
        try
        {
            std::vector<std::uint8_t> rgba;
            if (!Processing::TerrainBc1Codec::Decode(image.GetCompressedBytes(), image.GetWidthPixels(),
                image.GetHeightPixels(), rgba, true))
                throw std::runtime_error("BC1 decode failed");
            return WriteCaptureDiagnosticRgbaPng(path, rgba, image.GetWidthPixels(),
                image.GetHeightPixels(), sourceId);
        }
        catch (...)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s format=BC1 PNG",
                    sourceId.c_str());
            return false;
        }
    }

    /** Writes transient pre-BC1 and supersampled samples under the configured diagnostics subtree. */
    void TerrainService::ExportPreCompressionCapture(const Capture::TerrainCaptureDiagnosticPixels& pixels,
        const ITerrainDataProvider::CaptureDefinition& definition) const noexcept
    {
        if (!exportCaptureDiagnostics_) return;
        try
        {
            if (pixels.FinalWidthPixels != definition.WidthPixels ||
                pixels.FinalHeightPixels != definition.HeightPixels ||
                pixels.SupersampledWidthPixels != definition.WidthPixels * 2u ||
                pixels.SupersampledHeightPixels != definition.HeightPixels * 2u)
                throw std::runtime_error("capture diagnostic dimensions do not match the admitted definition");
            std::filesystem::path diagnosticsDirectory;
            std::wstring baseName;
            if (!GetCaptureDiagnosticPath(definition, diagnosticsDirectory, baseName))
                throw std::runtime_error("terrain cache root or page identity is unavailable");
            std::filesystem::create_directories(diagnosticsDirectory);
            WriteCaptureDiagnosticRgbPng(diagnosticsDirectory / (baseName + L".png"),
                pixels.FinalRgbPixels, pixels.FinalWidthPixels, pixels.FinalHeightPixels,
                definition.Source.SourceId);
            WriteCaptureDiagnosticRgbPng(diagnosticsDirectory / (baseName + L"_Supersampled.png"),
                pixels.SupersampledRgbPixels, pixels.SupersampledWidthPixels,
                pixels.SupersampledHeightPixels, definition.Source.SourceId);
        }
        catch (const std::exception& error)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: pre-compression terrain diagnostic export failed source=%s reason=%s",
                    definition.Source.SourceId.c_str(), error.what());
        }
        catch (...)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: pre-compression terrain diagnostic export failed source=%s",
                    definition.Source.SourceId.c_str());
        }
    }

    /** Writes canonical BC1 bytes and their decoded PNG under the configured diagnostics subtree. */
    void TerrainService::ExportCompletedCapture(const TerrainImage& image,
        const ITerrainDataProvider::CaptureDefinition& definition) const noexcept
    {
        if (!exportCaptureDiagnostics_) return;
        try
        {
            std::filesystem::path diagnosticsDirectory;
            std::wstring baseName;
            if (!GetCaptureDiagnosticPath(definition, diagnosticsDirectory, baseName))
                throw std::runtime_error("terrain cache root or page identity is unavailable");
            std::filesystem::create_directories(diagnosticsDirectory);
            WriteCaptureDiagnosticFile(diagnosticsDirectory / (baseName + L".bc1"),
                image.GetCompressedBytes(), definition.Source.SourceId, "bc1");
            WriteCaptureDiagnosticPng(diagnosticsDirectory / (baseName + L".bc1.png"),
                image, definition.Source.SourceId);
        }
        catch (const std::exception& error)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s reason=%s",
                    definition.Source.SourceId.c_str(), error.what());
        }
        catch (...)
        {
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                    "MiniMap: terrain capture diagnostic export failed source=%s",
                    definition.Source.SourceId.c_str());
        }
    }
#endif

    /** Admits a validated finite collection, probes every page, then creates one shared resident world if needed. */
    bool TerrainService::RequestPrivateCaptureBatch(
        const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions,
        SDK::UWorld* gameplayWorld, MiniMap::Services::INativeHooks& hooks)
    {
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Running) return false;
        std::string validationError;
        if (!ValidatePrivateCaptureBatch(definitions, validationError))
        {
            privateCaptureBatch_ = {};
            privateCaptureBatch_.Status = CaptureBatchStatus::Failed;
            privateCaptureBatch_.TotalImages = definitions.size();
            privateCaptureBatch_.Definitions = definitions;
            if (session_) privateCaptureBatch_.Context = *session_;
            privateCaptureBatch_.Error = std::move(validationError);
            if (auto* log = MiniMap::Services::TryLog())
                MiniMap::Services::Logf(*log, MiniMap::Services::Level::Error,
                    "MiniMap: private terrain capture batch failed during validation: %s",
                    privateCaptureBatch_.Error.c_str());
            return false;
        }
        privateCaptureBatch_ = {};
        privateCaptureBatch_.Status = CaptureBatchStatus::Running;
        privateCaptureBatch_.TotalImages = definitions.size();
        try
        {
            privateCaptureBatch_.Definitions = definitions;
            privateCaptureBatch_.Context = *session_;
            privateCaptureBatch_.GameplayWorld = gameplayWorld;
            if (!CheckPrivateCaptureBatchCache()) return false;
        }
        catch (const std::exception& error)
        {
            FailPrivateCaptureBatch(error.what());
            throw;
        }
        catch (...)
        {
            FailPrivateCaptureBatch("Capture batch cache admission threw an unknown exception.");
            throw;
        }
        LogPrivateCaptureBatchCacheSummary();
        if (privateCaptureBatch_.MissingDefinitionIndices.empty())
        {
            CompletePrivateCaptureBatchFromCache();
            return true;
        }
        if (privateStreaming_ || !gameplayWorld)
        {
            FailPrivateCaptureBatch("A current gameplay world and an unused private streaming slot are required.");
            return false;
        }
        privateCaptureBatch_.Phase = CaptureBatchPhase::AwaitingResidency;
        const auto& firstMissing = privateCaptureBatch_.Definitions[
            privateCaptureBatch_.MissingDefinitionIndices.front()];
        return SubmitPrivateCaptureResidency(firstMissing, gameplayWorld, hooks);
    }

    /** Validates page hits with the same complete record identity and exposes only copied batch state. */
    TerrainService::CaptureBatchDelivery TerrainService::GetPrivateCaptureBatchDelivery() const
    {
        return {privateCaptureBatch_.Status, privateCaptureBatch_.CompletedImages,
            privateCaptureBatch_.TotalImages, privateCaptureBatch_.MissingCacheImages,
            privateCaptureBatch_.InvalidCacheRecords, privateCaptureBatch_.Cache,
            privateCaptureBatch_.Error, privateCaptureBatch_.UnpersistedImage};
    }

    /** Checks one exact immutable definition against cache admission and completed batch progression. */
    TerrainService::CaptureOutputState TerrainService::GetPrivateCaptureOutputState(
        const ITerrainDataProvider::CaptureDefinition& definition) const noexcept
    {
        if (!session_ || !session_->IsValid())
            return CaptureOutputState::Unavailable;
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Idle ||
            privateCaptureBatch_.Context != *session_)
            return CaptureOutputState::AwaitingBatchAdmission;
        std::size_t definitionIndex = privateCaptureBatch_.Definitions.size();
        for (std::size_t index = 0; index < privateCaptureBatch_.Definitions.size(); ++index)
        {
            if (HasSameCaptureOutput(definition, privateCaptureBatch_.Definitions[index]))
            {
                definitionIndex = index;
                break;
            }
        }
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Failed ||
            privateCaptureBatch_.Status == CaptureBatchStatus::Cancelled)
            return definitionIndex < privateCaptureBatch_.Definitions.size() ||
                privateCaptureBatch_.Definitions.empty()
                ? CaptureOutputState::CacheReady
                : CaptureOutputState::Unavailable;
        if (definitionIndex >= privateCaptureBatch_.Definitions.size())
            return CaptureOutputState::Unavailable;
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Completed)
            return CaptureOutputState::CacheReady;
        const auto missing = std::find(privateCaptureBatch_.MissingDefinitionIndices.begin(),
            privateCaptureBatch_.MissingDefinitionIndices.end(), definitionIndex);
        if (missing == privateCaptureBatch_.MissingDefinitionIndices.end())
            return CaptureOutputState::CacheReady;
        const auto missingPosition = static_cast<std::size_t>(std::distance(
            privateCaptureBatch_.MissingDefinitionIndices.begin(), missing));
        return missingPosition < privateCaptureBatch_.NextMissingIndex
            ? CaptureOutputState::CacheReady
            : CaptureOutputState::Generating;
    }

    /** Marks the current generation terminal while keeping exact definitions available for one cache check. */
    void TerrainService::RejectPrivateCaptureBatchAdmission(
        const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions, std::string reason)
    {
        if (!session_ || privateCaptureBatch_.Status == CaptureBatchStatus::Running ||
            (privateCaptureBatch_.Context == *session_ &&
                privateCaptureBatch_.Status != CaptureBatchStatus::Idle))
            return;
        privateCaptureBatch_ = {};
        privateCaptureBatch_.Status = CaptureBatchStatus::Failed;
        privateCaptureBatch_.Context = *session_;
        privateCaptureBatch_.TotalImages = definitions.size();
        privateCaptureBatch_.Definitions = definitions;
        privateCaptureBatch_.Error = std::move(reason);
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Error,
                "MiniMap: private terrain capture batch admission failed: %s",
                privateCaptureBatch_.Error.c_str());
    }

    /** Logs completed cache probe counts after the full output set was inspected successfully. */
    void TerrainService::LogPrivateCaptureBatchCacheSummary() const
    {
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: private terrain capture batch admitted images=%zu cacheHits=%zu cacheMisses=%zu invalidRecords=%zu",
                privateCaptureBatch_.TotalImages,
                privateCaptureBatch_.TotalImages - privateCaptureBatch_.MissingCacheImages,
                privateCaptureBatch_.MissingCacheImages,
                privateCaptureBatch_.InvalidCacheRecords);
    }

    /** Completes cache-only batches without creating private streaming ownership. */
    void TerrainService::CompletePrivateCaptureBatchFromCache()
    {
        privateCaptureBatch_.Status = CaptureBatchStatus::Completed;
        privateCaptureBatch_.Phase = CaptureBatchPhase::Idle;
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: private terrain capture batch completed from cache images=%zu",
                privateCaptureBatch_.CompletedImages);
    }

    /** Rejects unsupported definitions, duplicate output/page identities and divergent residency requirements. */
    bool TerrainService::ValidatePrivateCaptureBatch(
        const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions, std::string& error) const
    {
        if (!session_ || !session_->IsValid() || definitions.empty())
        {
            error = "Capture batch requires a current gameplay session and at least one definition.";
            return false;
        }
        std::vector<std::wstring> filenames;
        filenames.reserve(definitions.size());
        for (std::size_t index = 0; index < definitions.size(); ++index)
        {
            Cache::TerrainCacheIdentity identity;
            std::wstring filename;
            if (!ValidatePrivateCaptureDefinition(definitions[index], identity, filename))
            {
                error = "Capture batch contains an unsupported image, identity or cache page.";
                return false;
            }
            if (index && !HasSameCaptureResidency(definitions.front(), definitions[index]))
            {
                error = "Capture batch definitions do not share identical residency requirements.";
                return false;
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (definitions[previous].Source.SourceId == definitions[index].Source.SourceId)
                {
                    error = "Capture batch output identities must be independent.";
                    return false;
                }
                if (SameCaptureCacheFilename(filenames[previous], filename))
                {
                    error = "Capture batch page identities must name distinct cache records.";
                    return false;
                }
            }
            filenames.push_back(std::move(filename));
        }
        return true;
    }

    /** Uses capture geometry validation without resolving native names or preparing private streaming values. */
    bool TerrainService::ValidatePrivateCaptureDefinition(
        const ITerrainDataProvider::CaptureDefinition& definition,
        Cache::TerrainCacheIdentity& identity, std::wstring& filename) const
    {
        std::vector<std::uint8_t> metadata;
        std::uint64_t fileBytes = 0;
        return definition.Source.IsValid() &&
            !definition.OriginalWorldPackage.empty() && !definition.CookedCellPackages.empty() &&
            !definition.TargetGrids.empty() && definition.StreamingBounds.IsValid() &&
            Capture::TerrainImageCapture::IsGeometrySupported(definition) &&
            CreateCaptureCacheIdentity(definition, identity) &&
            Cache::TerrainCacheRecord::Describe(identity, metadata, fileBytes) &&
            Cache::TerrainCacheRecord::GetFilename(definition.Page, filename);
    }

    /** Applies the cache's existing ASCII case-insensitive Windows filename equivalence. */
    bool TerrainService::SameCaptureCacheFilename(std::wstring left, std::wstring right)
    {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (left[index] >= L'A' && left[index] <= L'Z') left[index] += L'a' - L'A';
            if (right[index] >= L'A' && right[index] <= L'Z') right[index] += L'a' - L'A';
            if (left[index] != right[index]) return false;
        }
        return true;
    }

    /** Reads every tile until a complete page, a known bad record or an operational IO failure is established. */
    TerrainService::CacheResult TerrainService::CheckPrivateCapturePage(
        const Cache::TerrainCacheIdentity& identity, bool& complete, bool& invalidRecord) const
    {
        complete = true;
        invalidRecord = false;
        const auto tilesPerAxis = identity.Width / Cache::TerrainCacheIdentity::TileDimension;
        for (std::uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY)
        {
            for (std::uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX)
            {
                std::vector<std::uint8_t> tile;
                const auto result = TryLoadCacheTile(identity, static_cast<int>(tileX),
                    static_cast<int>(tileY), tile);
                if (result.Status == CacheStatus::Hit) continue;
                complete = false;
                invalidRecord = result.Status == CacheStatus::InvalidFileSize ||
                    result.Status == CacheStatus::HeaderMismatch;
                return result;
            }
        }
        return {CacheStatus::Hit};
    }

    /** Separates genuine misses and invalid records from operational lookup failures for each output. */
    bool TerrainService::CheckPrivateCaptureBatchCache()
    {
        for (std::size_t index = 0; index < privateCaptureBatch_.Definitions.size(); ++index)
        {
            Cache::TerrainCacheIdentity identity;
            CreateCaptureCacheIdentity(privateCaptureBatch_.Definitions[index], identity);
            bool complete = false;
            bool invalidRecord = false;
            const auto result = CheckPrivateCapturePage(identity, complete, invalidRecord);
            privateCaptureBatch_.Cache = result;
            if (complete)
            {
                ++privateCaptureBatch_.CompletedImages;
                continue;
            }
            if (result.Status != CacheStatus::Miss && !invalidRecord)
            {
                FailPrivateCaptureBatch("Capture cache lookup failed; status " +
                    std::to_string(static_cast<int>(result.Status)) + ".", result);
                return false;
            }
            ++privateCaptureBatch_.MissingCacheImages;
            privateCaptureBatch_.MissingDefinitionIndices.push_back(index);
            if (invalidRecord) ++privateCaptureBatch_.InvalidCacheRecords;
        }
        return true;
    }

    /** Uses the existing explicit request API, preserving its session ownership and failure handling. */
    bool TerrainService::SubmitPrivateCaptureResidency(
        const ITerrainDataProvider::CaptureDefinition& definition,
        SDK::UWorld* gameplayWorld, MiniMap::Services::INativeHooks& hooks)
    {
        const auto submittedDefinition = definition;
        const auto batchContext = privateCaptureBatch_.Context;
        bool accepted = false;
        try
        {
            advancingCaptureBatch_ = true;
            accepted = RequestPrivateStreaming(submittedDefinition, gameplayWorld, hooks);
            advancingCaptureBatch_ = false;
        }
        catch (const std::exception& error)
        {
            advancingCaptureBatch_ = false;
            privateCaptureBatch_.OwnsStreamingSession = privateStreaming_ != nullptr;
            if (privateCaptureBatch_.Status == CaptureBatchStatus::Running &&
                session_ && *session_ == batchContext)
                FailPrivateCaptureBatch(error.what());
            throw;
        }
        catch (...)
        {
            advancingCaptureBatch_ = false;
            privateCaptureBatch_.OwnsStreamingSession = privateStreaming_ != nullptr;
            if (privateCaptureBatch_.Status == CaptureBatchStatus::Running &&
                session_ && *session_ == batchContext)
                FailPrivateCaptureBatch("Private capture streaming submission threw an unknown exception.");
            throw;
        }
        privateCaptureBatch_.OwnsStreamingSession = privateStreaming_ != nullptr;
        if (privateCaptureBatch_.Status != CaptureBatchStatus::Running ||
            !session_ || *session_ != batchContext) return false;
        if (!accepted)
        {
            const auto reason = GetPrivateStreamingReason();
            FailPrivateCaptureBatch(reason.empty()
                ? "Private capture streaming submission was rejected."
                : reason);
        }
        return accepted;
    }

    /** Advances the finite batch only when existing residency and capture-result contracts permit progress. */
    void TerrainService::AdvancePrivateCaptureBatch()
    {
        if (privateCaptureBatch_.Status != CaptureBatchStatus::Running) return;
        if (!session_ || *session_ != privateCaptureBatch_.Context)
        {
            CancelPrivateCaptureBatch("Gameplay session changed during the capture batch.");
            return;
        }
        if (!privateStreaming_)
        {
            FailPrivateCaptureBatch("Private streaming ownership ended before the capture batch completed.");
            return;
        }
        if (privateCaptureBatch_.Phase == CaptureBatchPhase::AwaitingResidency)
        {
            AdvanceCaptureBatchResidency();
            return;
        }
        if (privateCaptureBatch_.Phase == CaptureBatchPhase::Capturing)
        {
            AdvanceCaptureBatchImage();
            return;
        }
        if (privateCaptureBatch_.Phase == CaptureBatchPhase::AwaitingCaptureRetirement &&
            privateStreaming_->IsCaptureRetired())
        {
            ContinuePrivateCaptureBatch();
        }
    }

    /** Starts only after the retained session publishes positive Ready residency. */
    void TerrainService::AdvanceCaptureBatchResidency()
    {
        const auto status = privateStreaming_->GetStatus();
        if (status == Capture::PrivateStreamingSession::Status::Ready)
        {
            SubmitNextPrivateCapture();
            return;
        }
        if (status == Capture::PrivateStreamingSession::Status::Retiring ||
            status == Capture::PrivateStreamingSession::Status::Retired ||
            status == Capture::PrivateStreamingSession::Status::Inactive)
        {
            const auto reason = GetPrivateStreamingReason();
            FailPrivateCaptureBatch(reason.empty()
                ? "Private residency ended before required cells became ready."
                : reason);
        }
    }

    /** Waits for one existing capture result and consumes it exactly once. */
    void TerrainService::AdvanceCaptureBatchImage()
    {
        const auto status = privateStreaming_->GetCaptureResultStatus();
        if (status == Capture::PrivateStreamingSession::CaptureResultStatus::Pending) return;
        if (status == Capture::PrivateStreamingSession::CaptureResultStatus::ImageAvailable)
        {
            ConsumePrivateCaptureBatchImage();
            return;
        }
        const auto reason = GetPrivateCaptureResultReason();
        FailPrivateCaptureBatch(reason.empty()
            ? "Private capture failed before producing a completed image."
            : reason);
    }

    /** Submits one missing definition only after the preceding native capture has retired. */
    bool TerrainService::SubmitNextPrivateCapture()
    {
        if (privateCaptureBatch_.NextMissingIndex >= privateCaptureBatch_.MissingDefinitionIndices.size())
        {
            privateCaptureBatch_.Status = CaptureBatchStatus::Completed;
            privateCaptureBatch_.Phase = CaptureBatchPhase::Idle;
            RetirePrivateStreaming();
            return true;
        }
        const auto definitionIndex =
            privateCaptureBatch_.MissingDefinitionIndices[privateCaptureBatch_.NextMissingIndex];
        const auto definition = privateCaptureBatch_.Definitions[definitionIndex];
        const auto batchContext = privateCaptureBatch_.Context;
        bool accepted = false;
        try
        {
            advancingCaptureBatch_ = true;
            accepted = RequestPrivateCapture(definition);
            advancingCaptureBatch_ = false;
        }
        catch (...)
        {
            advancingCaptureBatch_ = false;
            throw;
        }
        if (privateCaptureBatch_.Status != CaptureBatchStatus::Running ||
            !session_ || *session_ != batchContext) return false;
        if (!accepted)
        {
            const auto reason = GetPrivateCaptureResultReason();
            FailPrivateCaptureBatch(reason.empty()
                ? "The next missing capture was rejected after residency readiness."
                : reason);
            return false;
        }
        privateCaptureBatch_.Phase = CaptureBatchPhase::Capturing;
        return true;
    }

    /** Preserves a valid image when persistence fails and stops the batch before another capture is submitted. */
    void TerrainService::ConsumePrivateCaptureBatchImage()
    {
        const auto definitionIndex = privateCaptureBatch_.MissingDefinitionIndices[
            privateCaptureBatch_.NextMissingIndex];
        const auto& definition = privateCaptureBatch_.Definitions[definitionIndex];
        std::shared_ptr<const TerrainImage> image;
        try
        {
            advancingCaptureBatch_ = true;
            image = TakePrivateCaptureImage();
            advancingCaptureBatch_ = false;
        }
        catch (...)
        {
            advancingCaptureBatch_ = false;
            throw;
        }
        const auto cache = GetPrivateCaptureCacheResult();
        privateCaptureBatch_.Cache = cache;
        if (!image)
        {
            const auto reason = GetPrivateCaptureResultReason();
            FailPrivateCaptureBatch(reason.empty()
                ? "Private capture reported an image without returning immutable output."
                : reason, cache);
            return;
        }
        if (!ValidateCompletedCaptureImage(*image, definition))
        {
            FailPrivateCaptureBatch("Completed capture image failed canonical definition validation.", cache, image);
            return;
        }
        if (cache.Status != CacheStatus::Stored)
        {
            FailPrivateCaptureBatch("Completed capture persistence failed; cache status " +
                std::to_string(static_cast<int>(cache.Status)) + ".", cache, image);
            return;
        }
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: private terrain capture image persisted source=%s completed=%zu/%zu",
                definition.Source.SourceId.c_str(), privateCaptureBatch_.CompletedImages + 1,
                privateCaptureBatch_.TotalImages);
        ++privateCaptureBatch_.CompletedImages;
        ++privateCaptureBatch_.NextMissingIndex;
        privateCaptureBatch_.Phase = CaptureBatchPhase::AwaitingCaptureRetirement;
    }

    /** Continues only after the prior capture owner reached its native retirement boundary. */
    void TerrainService::ContinuePrivateCaptureBatch()
    {
        if (privateCaptureBatch_.NextMissingIndex < privateCaptureBatch_.MissingDefinitionIndices.size())
        {
            SubmitNextPrivateCapture();
            return;
        }
        privateCaptureBatch_.Status = CaptureBatchStatus::Completed;
        privateCaptureBatch_.Phase = CaptureBatchPhase::Idle;
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: private terrain capture batch completed images=%zu cacheMisses=%zu",
                privateCaptureBatch_.CompletedImages, privateCaptureBatch_.MissingCacheImages);
        RetirePrivateStreaming();
    }

    /** Records terminal logical failure before requesting existing asynchronous session retirement. */
    void TerrainService::FailPrivateCaptureBatch(std::string reason, CacheResult cache,
        std::shared_ptr<const TerrainImage> unpersistedImage)
    {
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Failed)
            return;
        privateCaptureBatch_.Status = CaptureBatchStatus::Failed;
        privateCaptureBatch_.Phase = CaptureBatchPhase::Idle;
        privateCaptureBatch_.Error = std::move(reason);
        if (cache.Status != CacheStatus::Unavailable || cache.Error)
            privateCaptureBatch_.Cache = cache;
        privateCaptureBatch_.UnpersistedImage = std::move(unpersistedImage);
        privateCaptureDefinition_.reset();
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Error,
                "MiniMap: private terrain capture batch failed completed=%zu/%zu reason=%s",
                privateCaptureBatch_.CompletedImages, privateCaptureBatch_.TotalImages,
                privateCaptureBatch_.Error.c_str());
        if (privateCaptureBatch_.OwnsStreamingSession && privateStreaming_)
            privateStreaming_->RequestRetirement();
    }

    /** Withdraws logical batch progression without releasing any native session or capture owner. */
    void TerrainService::CancelPrivateCaptureBatch(std::string reason)
    {
        privateCaptureBatch_.Status = CaptureBatchStatus::Cancelled;
        privateCaptureBatch_.Phase = CaptureBatchPhase::Idle;
        privateCaptureBatch_.Error = std::move(reason);
        privateCaptureDefinition_.reset();
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
                "MiniMap: private terrain capture batch cancelled completed=%zu/%zu reason=%s",
                privateCaptureBatch_.CompletedImages, privateCaptureBatch_.TotalImages,
                privateCaptureBatch_.Error.c_str());
    }

    /** Compares the exact copied world/cell/grid/layer/streaming-volume evidence used by the session. */
    bool TerrainService::HasSameCaptureResidency(
        const ITerrainDataProvider::CaptureDefinition& left,
        const ITerrainDataProvider::CaptureDefinition& right) noexcept
    {
        const auto& leftBounds = left.StreamingBounds;
        const auto& rightBounds = right.StreamingBounds;
        return left.OriginalWorldPackage == right.OriginalWorldPackage &&
            left.CookedCellPackages == right.CookedCellPackages && left.TargetGrids == right.TargetGrids &&
            left.ActivatedDataLayerAssets == right.ActivatedDataLayerAssets &&
            leftBounds.Minimum.X == rightBounds.Minimum.X && leftBounds.Minimum.Y == rightBounds.Minimum.Y &&
            leftBounds.Minimum.Z == rightBounds.Minimum.Z && leftBounds.Maximum.X == rightBounds.Maximum.X &&
            leftBounds.Maximum.Y == rightBounds.Maximum.Y && leftBounds.Maximum.Z == rightBounds.Maximum.Z;
    }

    /** Rejects a matching identity when any residency, clipping, registration or output setting differs. */
    bool TerrainService::HasSameCaptureOutput(
        const ITerrainDataProvider::CaptureDefinition& left,
        const ITerrainDataProvider::CaptureDefinition& right) noexcept
    {
        const auto& leftCapture = left.CaptureBounds;
        const auto& rightCapture = right.CaptureBounds;
        const auto& leftRegistration = left.ImageRegistration;
        const auto& rightRegistration = right.ImageRegistration;
        return left.Source.SourceId == right.Source.SourceId &&
            left.Source.ContentRevision == right.Source.ContentRevision && left.Page == right.Page &&
            HasSameCaptureResidency(left, right) &&
            left.ExcludedStaticMeshAssets == right.ExcludedStaticMeshAssets &&
            leftCapture.Minimum.X == rightCapture.Minimum.X && leftCapture.Minimum.Y == rightCapture.Minimum.Y &&
            leftCapture.Minimum.Z == rightCapture.Minimum.Z && leftCapture.Maximum.X == rightCapture.Maximum.X &&
            leftCapture.Maximum.Y == rightCapture.Maximum.Y && leftCapture.Maximum.Z == rightCapture.Maximum.Z &&
            leftRegistration.Origin.X == rightRegistration.Origin.X &&
            leftRegistration.Origin.Y == rightRegistration.Origin.Y && leftRegistration.U.X == rightRegistration.U.X &&
            leftRegistration.U.Y == rightRegistration.U.Y && leftRegistration.V.X == rightRegistration.V.X &&
            leftRegistration.V.Y == rightRegistration.V.Y && left.WidthPixels == right.WidthPixels &&
            left.HeightPixels == right.HeightPixels && left.SRgb == right.SRgb &&
            left.OpaqueBackground == right.OpaqueBackground;
    }

    /** Reads the retained logical capture outcome without advancing native capture retirement. */
    Capture::PrivateStreamingSession::CaptureResultStatus TerrainService::GetPrivateCaptureResultStatus() const noexcept
    {
        return privateStreaming_
            ? privateStreaming_->GetCaptureResultStatus()
            : Capture::PrivateStreamingSession::CaptureResultStatus::None;
    }

    /** Copies a terminal capture failure explanation without changing capture or session ownership. */
    std::string TerrainService::GetPrivateCaptureResultReason() const
    {
        return privateStreaming_ ? privateStreaming_->GetCaptureResultReason() : std::string{};
    }

    /** Remains inert during ordinary terrain use; retirement advances independently of provider/image admission. */
    void TerrainService::AdvancePrivateStreaming()
    {
        try
        {
            if (privateStreaming_) privateStreaming_->Advance();
            AdvancePrivateCaptureBatch();
            if (privateStreaming_ && privateCaptureBatch_.Status != CaptureBatchStatus::Running &&
                privateStreaming_->GetStatus() == Capture::PrivateStreamingSession::Status::Retired &&
                privateStreaming_->TryShutdown())
            {
                privateStreaming_.reset();
                privateCaptureBatch_.OwnsStreamingSession = false;
            }
        }
        catch (const std::exception& error)
        {
            if (privateCaptureBatch_.Status == CaptureBatchStatus::Running)
                FailPrivateCaptureBatch(error.what());
            throw;
        }
        catch (...)
        {
            if (privateCaptureBatch_.Status == CaptureBatchStatus::Running)
                FailPrivateCaptureBatch("Private capture batch advancement threw an unknown exception.");
            throw;
        }
    }

    /** Logical cancellation never destroys the native session or its callback owners. */
    void TerrainService::RetirePrivateStreaming()
    {
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Running)
            CancelPrivateCaptureBatch("Private capture batch was cancelled by streaming retirement.");
        if (privateStreaming_)
            privateStreaming_->RequestRetirement();
    }

    /** Requires native unregistration acknowledgement before returning from the application before-EndPlay boundary. */
    bool TerrainService::BeforeGameplayWorldEnd(SDK::UWorld* world)
    {
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Running &&
            privateCaptureBatch_.GameplayWorld == world)
            CancelPrivateCaptureBatch("Gameplay world ended before the capture batch completed.");
        return !privateStreaming_ || privateStreaming_->BeforeGameplayWorldEnd(world);
    }

    /** Keeps event advancement, hook ownership and NativeApi alive when any native acknowledgement is still missing. */
    bool TerrainService::ShutdownPrivateStreaming()
    {
        if (privateCaptureBatch_.Status == CaptureBatchStatus::Running)
            CancelPrivateCaptureBatch("Private capture batch was cancelled for shutdown.");
        if (!privateStreaming_)
            return true;
        if (!privateStreaming_->TryShutdown())
            return false;
        privateStreaming_.reset();
        privateCaptureBatch_.OwnsStreamingSession = false;
        return true;
    }

    /** Reads state without submitting work or consulting native objects. */
    Capture::PrivateStreamingSession::Status TerrainService::GetPrivateStreamingStatus() const noexcept
    {
        return privateStreaming_ ? privateStreaming_->GetStatus() : Capture::PrivateStreamingSession::Status::Inactive;
    }

    /** Returns copied failure evidence without changing native ownership. */
    std::string TerrainService::GetPrivateStreamingReason() const
    {
        return privateStreaming_ ? privateStreaming_->GetReason() : std::string{};
    }
#endif

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
            !context.RequestGeneration || !TerrainImage::IsValidSourceDimensions(
                state->parameters_.WidthPixels, state->parameters_.HeightPixels) ||
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

    /** Completes available work for one queued request, without a phase cap or timing allowance. */
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
    /** Borrows one observer through the unchanged advancement, restoring it on every exception path. */
    void TerrainService::AdvanceImageRequests(const ImageRequestTrace& trace)
    {
        if (advancingImages_) return;
        imageRequestTrace_ = &trace;
        try
        {
            AdvanceImageRequests();
        }
        catch (...)
        {
            imageRequestTrace_ = nullptr;
            throw;
        }
        imageRequestTrace_ = nullptr;
    }
#endif

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
        retainedSource_ = {};
    }

    /** Validates metadata and transfers existing blocks without decompression or recompression. */
    std::optional<TerrainImage> TerrainService::IngestBc1(TerrainImage::Identity identity,
        std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
        bool sRgb, std::vector<std::uint8_t>&& blocks)
    {
        if (!identity.IsValid() || !registration.IsValid() ||
            !TerrainImage::IsValidSourceDimensions(width, height) ||
            !Processing::TerrainBc1Codec::Validate(blocks, width, height)) return std::nullopt;
        return TerrainImage(std::move(identity), width, height, registration, sRgb, std::move(blocks));
    }

    /** Encodes transient RGB8 into the sole immutable canonical image allocation. */
    std::optional<TerrainImage> TerrainService::IngestRgb(TerrainImage::Identity identity,
        std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
        bool sRgb, std::span<const std::uint8_t> pixels, std::size_t rowStride)
    {
        if (!identity.IsValid() || !registration.IsValid() ||
            !TerrainImage::IsValidSourceDimensions(width, height)) return std::nullopt;
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
        if (!TerrainImage::IsValidSourceDimensions(width, height) || x >= image.GetWidthPixels() || y >= image.GetHeightPixels() ||
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
    bool TerrainService::ConfigureTerrainCache(const CacheConfiguration& configuration)
    {
        if (fileCache_)
        {
            if (!fileCache_->HasConfiguration(configuration)) return false;
            fileCache_->Close();
            return true;
        }
        if (configuration.Root.empty() || !configuration.Root.is_absolute()) return false;
        fileCache_ = std::make_unique<Cache::TerrainFileCache>(configuration);
        return true;
    }

    /** Reports the configured subtree without creating it. */
    bool TerrainService::GetTerrainCacheRoot(std::filesystem::path& root) const
    {
        if (!fileCache_) return false;
        root = fileCache_->GetRoot();
        return true;
    }

    /** Opens only after root invalidation, without changing cache identity or configuration. */
    void TerrainService::OpenTerrainCache()
    {
        if (fileCache_) fileCache_->Open();
    }

    /** Closes the instance independently of application root-cache locks and metadata. */
    void TerrainService::CloseTerrainCache()
    {
        if (fileCache_) fileCache_->Close();
    }

    /** Delegates subtree clearing and concurrent IO exclusion to the cache owner. */
    bool TerrainService::ClearTerrainCache()
    {
        return fileCache_ && fileCache_->Clear();
    }

    /** Forwards complete generic page evidence to the sole file IO owner without source access. */
    TerrainService::CacheResult TerrainService::TryLoadCacheTile(const Cache::TerrainCacheIdentity& key,
        int chunkX, int chunkY, std::vector<std::uint8_t>& compressedChunk, const CacheTrace* trace) const
    {
        if (!fileCache_) return {};
        return fileCache_->TryLoadChunk(key, chunkX, chunkY, compressedChunk, trace);
    }

    /** Persists existing compressed source bytes without conversion or acquisition. */
    TerrainService::CacheResult TerrainService::StoreCachePage(const Cache::TerrainCacheIdentity& key,
        const std::uint8_t* sourceData, std::size_t sourceDataSize, const CacheTrace* trace) const
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
#if defined(MODLOADER_CLIENT_BUILD)
        RetirePrivateStreaming();
#endif
        AdvanceSelectionRevision();
        ClearSession();
        session_ = std::move(nextSession);
        return true;
    }

    /** Clears only the expected active session, ignoring obsolete world-end notifications. */
    bool TerrainService::StopSession(const Session& session)
    {
        if (!session_ || *session_ != session) return false;
#if defined(MODLOADER_CLIENT_BUILD)
        RetirePrivateStreaming();
#endif
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

    /** Membership is not player selection: Default rejects any Alternate's retained marker positions. */
    bool TerrainService::SelectionCoverage::ContainsMarker(ITerrainDataProvider::Position position) const noexcept
    {
        if (!Current.Provider.IsRegistered() || !position.IsFinite()) return false;
        if (SelectedBounds) return SelectedBounds->Contains(position);
        for (const auto& bounds : AlternateBounds)
            if (bounds.Contains(position)) return false;
        return true;
    }

    /** Uses only the immutable coverage copied at registration, including unavailable image sources. */
    std::optional<TerrainService::SelectionCoverage> TerrainService::GetSelectionCoverage() const
    {
        const auto selection = GetSelection();
        if (!selection) return std::nullopt;
        SelectionCoverage observation;
        observation.Current = *selection;
        for (const auto& entry : entries_)
        {
            if (entry.Coverage.Role != ITerrainDataProvider::CoverageRole::Alternate) continue;
            for (const auto& region : entry.Coverage.Regions)
            {
                observation.AlternateBounds.push_back(region.SelectionBounds);
                if (Registration(entry.Identity) == selection->Provider && region.Id == selection->RegionId)
                    observation.SelectedBounds = region.SelectionBounds;
            }
        }
        return observation;
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
