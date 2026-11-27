// Retrieves the three authored FE BC1 RCDATA images through the existing ingestion contract.
// Never decodes, converts, caches, publishes images, detects FE occupancy or owns GPU resources.
#include "ForgottenEngineTerrainProvider.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include "resource.h"
#include <windows.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace MiniMapTerrainSource
{
    using Retrieval = Services::Map::Terrain::ITerrainSourceRetrieval;
    int ForgottenEngineTerrainProvider::resourceModuleAnchor_ = 0;

    /** Construction validates copied metadata only; the module is not inspected here. */
    ForgottenEngineTerrainProvider::ForgottenEngineTerrainProvider(Configuration configuration)
        : configuration_(std::move(configuration))
    {
        ValidateConfiguration(configuration_);
    }

    /** Restricts world names to a safe subset so the complete collection is valid without cache helpers. */
    void ForgottenEngineTerrainProvider::ValidateConfiguration(const Configuration& configuration)
    {
        if (configuration.WorldId.empty() || !configuration.WorldGeneration ||
            configuration.InstanceId != "ForgottenEngine1" || configuration.ContentRevision.empty() ||
            configuration.ContentRevision.find('\0') != std::string::npos)
            throw std::invalid_argument("FE terrain requires world/generation, ForgottenEngine1 and artwork revision.");
        for (const char character : configuration.WorldId)
        {
            const bool letter = (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z');
            const bool digit = character >= '0' && character <= '9';
            if (!letter && !digit && character != '_' && character != '-')
                throw std::invalid_argument("FE terrain world names require ASCII letters/digits/underscore/hyphen.");
        }
        if (GetPageIdentity(configuration, Candidate::CandidateZ5074).CollectionKey.size() > 128)
            throw std::invalid_argument("FE terrain collection exceeds 128 ASCII characters.");
        Coverage coverage{"StarRupture.ForgottenEngineTerrain", configuration.WorldId, CoverageRole::Alternate, {}};
        for (const auto& region : configuration.Regions)
        {
            coverage.Regions.push_back(region.Coverage);
            if (region.Artwork) GetCandidateIndex(*region.Artwork);
        }
        if (!coverage.IsValid()) throw std::invalid_argument("FE terrain requires valid nonoverlapping Alternate regions.");
    }

    /** A closed enum prevents invented floors from silently indexing another resource. */
    std::size_t ForgottenEngineTerrainProvider::GetCandidateIndex(Candidate candidate)
    {
        switch (candidate)
        {
        case Candidate::CandidateZ5074: return 0;
        case Candidate::CandidateZ6274: return 1;
        case Candidate::CandidateZ7474: return 2;
        }
        throw std::invalid_argument("Unsupported FE terrain candidate.");
    }

    /** Candidate names retain the existing precise elevation identity, without classifying a position. */
    const char* ForgottenEngineTerrainProvider::GetCandidateId(Candidate candidate)
    {
        constexpr std::array names{"CandidateZ5074.529cm", "CandidateZ6274.529cm", "CandidateZ7474.529cm"};
        return names[GetCandidateIndex(candidate)];
    }

    /** Resource IDs identify only newly embedded raw BC1 artwork. */
    int ForgottenEngineTerrainProvider::GetResourceId(Candidate candidate)
    {
        constexpr std::array ids{IDR_FE_FLOOR_BC1_5074, IDR_FE_FLOOR_BC1_6274, IDR_FE_FLOOR_BC1_7474};
        return ids[GetCandidateIndex(candidate)];
    }

    /** Revision includes supplied artwork identity and the fixed offline encoder/opaque RGB policy. */
    Services::Map::Terrain::TerrainImage::Identity ForgottenEngineTerrainProvider::GetIdentity(
        const Configuration& configuration, Candidate candidate)
    {
        return {"StarRupture.ForgottenEngineTerrain/" + configuration.WorldId + "/" +
            configuration.InstanceId + "/" + GetCandidateId(candidate),
            configuration.ContentRevision + "/TerrainBc1Codec.Rgb565.PairBounds.Refine421.v1/OpaqueRgb8"};
    }

    /** Floor numbers are candidate order, not authored level labels; placement comes solely from affine data. */
    Services::Map::Terrain::TerrainPageIdentity ForgottenEngineTerrainProvider::GetPageIdentity(
        const Configuration& configuration, Candidate candidate)
    {
        return {configuration.WorldId + "_" + configuration.InstanceId + "_Floor" +
            std::to_string(GetCandidateIndex(candidate) + 1), 0, 0};
    }

    /** Centers the original artwork in a 250-metre canvas; +U follows world +X, +V world -Y. */
    Services::Map::Terrain::TerrainImage::Registration ForgottenEngineTerrainProvider::GetRegistration()
    {
        return {{-2124.5304861843215, 453405.29135455645},
            {25000.0, 0.0}, {0.0, -25000.0}};
    }

    /** Immutable resource metadata never derives collection indices or logical size from physical positions. */
    Retrieval::Inspection ForgottenEngineTerrainProvider::DescribeInspection(
        const Configuration& configuration, Candidate candidate)
    {
        Retrieval::Inspection inspection;
        inspection.Source = GetIdentity(configuration, candidate);
        inspection.Locator.Value = inspection.Source.SourceId;
        inspection.Page = GetPageIdentity(configuration, candidate);
        inspection.Encoding = Retrieval::SourceEncoding::Bc1;
        inspection.WidthPixels = 512;
        inspection.HeightPixels = 512;
        inspection.RowStrideBytes = 1024;
        inspection.SRgb = true;
        return inspection;
    }

    /** Thread check precedes world access; no gameplay object/pointer is retained or raw-memory probed. */
    Retrieval::Status ForgottenEngineTerrainProvider::GetSessionStatus(const Configuration& configuration)
    {
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread || !native->player.isGameThread())
            return Retrieval::Status::Unavailable;
        if (MiniMapMap::GetWorldGeneration() != configuration.WorldGeneration)
            return Retrieval::Status::SourceChanged;
        if (!MiniMapMap::GameplayReady()) return Retrieval::Status::Unavailable;
        const auto* world = MiniMapMap::GetWorld();
        if (!world) return Retrieval::Status::Unavailable;
        if (world->GetName() != configuration.WorldId ||
            MiniMapMap::GetWorldGeneration() != configuration.WorldGeneration)
            return Retrieval::Status::SourceChanged;
        return Retrieval::Status::Available;
    }

    /** Coverage comes from application policy, not resource registration or the gameplay-status flag. */
    ForgottenEngineTerrainProvider::Coverage ForgottenEngineTerrainProvider::DescribeCoverage() const
    {
        Coverage coverage{"StarRupture.ForgottenEngineTerrain", configuration_.WorldId, CoverageRole::Alternate, {}};
        for (const auto& region : configuration_.Regions) coverage.Regions.push_back(region.Coverage);
        return coverage;
    }

    /** Creates independent strong capability ownership without acquiring resource bytes or canonical images. */
    ForgottenEngineTerrainProvider::SourceDescription ForgottenEngineTerrainProvider::DescribeSource(
        const SourceQuery& query)
    {
        if (query.WorldId != configuration_.WorldId ||
            !query.PhysicalPosition.IsFinite() || !query.SamplePosition.IsFinite() ||
            GetSessionStatus(configuration_) != Retrieval::Status::Available) return {};
        for (const auto& region : configuration_.Regions)
        {
            if (region.Coverage.Id != query.RegionId) continue;
            if (!region.Artwork) return {};
            const auto candidate = *region.Artwork;
            const auto inspection = DescribeInspection(configuration_, candidate);
            RetrievalSource retrieval{inspection.Locator,
                std::make_shared<SourceRetrieval>(configuration_, candidate), GetRegistration()};
            return {inspection.Source, std::move(retrieval), inspection.Page};
        }
        return {};
    }

    /** Owns only immutable copied strings/scalars; destruction performs no native or GPU operation. */
    ForgottenEngineTerrainProvider::SourceRetrieval::SourceRetrieval(Configuration configuration, Candidate candidate)
        : configuration_(std::move(configuration))
        , candidate_(candidate)
    {
    }

    /** Inspection reads only resource metadata; retrieval optionally borrows mapped data for this call. */
    Retrieval::Status ForgottenEngineTerrainProvider::SourceRetrieval::InspectResource(
        const SourceLocator& source, Inspection& inspection, std::span<const std::uint8_t>* bytes) const
    {
        if (bytes) *bytes = {};
        inspection = DescribeInspection(configuration_, candidate_);
        if (source.Value != inspection.Locator.Value) return Status::Failed;
        const auto status = GetSessionStatus(configuration_);
        if (status != Status::Available) return status;
        HMODULE module = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&resourceModuleAnchor_), &module))
            return Status::Failed;
        const HRSRC resource = FindResourceW(module,
            MAKEINTRESOURCEW(GetResourceId(candidate_)), RT_RCDATA);
        if (!resource || SizeofResource(module, resource) != 131'072u) return Status::Failed;
        if (bytes)
        {
            const HGLOBAL loaded = LoadResource(module, resource);
            const auto* data = loaded ? static_cast<const std::uint8_t*>(LockResource(loaded)) : nullptr;
            if (!data) return Status::Failed;
            *bytes = {data, 131'072u};
        }
        const auto current = GetSessionStatus(configuration_);
        if (current != Status::Available) return current;
        inspection.State = Status::Available;
        return Status::Available;
    }

    /** No pointer escapes inspection; all returned source/page/layout evidence is copied. */
    Retrieval::Inspection ForgottenEngineTerrainProvider::SourceRetrieval::Inspect(const SourceLocator& source)
    {
        Inspection inspection;
        inspection.State = InspectResource(source, inspection, nullptr);
        return inspection;
    }

    /** Full comparison prevents a caller from substituting source, page, color or byte-layout evidence. */
    bool ForgottenEngineTerrainProvider::SourceRetrieval::SameInspection(
        const Inspection& left, const Inspection& right) noexcept
    {
        return left.State == right.State && left.Locator.Value == right.Locator.Value &&
            left.Source == right.Source && left.Encoding == right.Encoding &&
            left.WidthPixels == right.WidthPixels && left.HeightPixels == right.HeightPixels &&
            left.RowStrideBytes == right.RowStrideBytes && left.SRgb == right.SRgb && left.Page == right.Page;
    }

    /** Service ingestion consumes the span synchronously; reentrant session changes cannot report success. */
    Retrieval::Status ForgottenEngineTerrainProvider::SourceRetrieval::Retrieve(
        const Inspection& expectedSource, Ingestion& destination)
    {
        if (expectedSource.State != Status::Available) return Status::Failed;
        Inspection current;
        std::span<const std::uint8_t> bytes;
        const auto status = InspectResource(expectedSource.Locator, current, &bytes);
        if (status != Status::Available) return status;
        if (!SameInspection(expectedSource, current)) return Status::SourceChanged;
        const bool accepted = destination.AcceptBc1(current, bytes);
        const auto afterIngestion = GetSessionStatus(configuration_);
        if (afterIngestion != Status::Available) return afterIngestion;
        return accepted ? Status::Available : Status::Failed;
    }
}
#endif
