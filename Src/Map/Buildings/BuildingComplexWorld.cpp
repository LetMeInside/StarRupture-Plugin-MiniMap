#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexWorld.h"
#include "BuildingComplexApiInternal.h"
#include "BuildingCollector.h"
#include "BuildingComplexSourceAcquisition.h"
#include "Config/Config.h"
#include "BuildingSplinePolicy.h"
#include "BuildingSplineOcclusionAdapter.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "Native/TexturePublication.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace C = MiniMapBuildingCollector;
    namespace A = MiniMapBuildingSplineOcclusionAdapter;
    namespace O = MiniMapBuildingSplineOcclusion;

    B::Index Index;
    MiniMapBuildingComplexWorld::Status Current;
    std::unique_ptr<MiniMapBuildingCollector::BuildingCollector> ConsumerCollector;
    uint64_t ConsumerCollectorSession = 0;
    SDK::UWorld* ConsumerCollectorWorld = nullptr;
    std::shared_ptr<const C::Snapshot> LastCollectorSnapshot;
    std::map<B::MassKey, uint64_t> CollectorSourceRevisions;
    std::set<B::MassKey> IndexedCollectorKeys;
    uint64_t CollectorSourceRevision = 0;
    std::set<B::SectionKey> PendingOcclusionSections;
    bool PendingGlobalOcclusion = false;

    void ReleaseConsumerCollector() noexcept
    {
        if (ConsumerCollector) ConsumerCollector->ShutdownResources();
        ConsumerCollector.reset();
        ConsumerCollectorSession = 0;
        ConsumerCollectorWorld = nullptr;
        LastCollectorSnapshot.reset();
        CollectorSourceRevisions.clear();
        IndexedCollectorKeys.clear();
        CollectorSourceRevision = 0;
    }

    // Tile results are scoped to the accepted provider session and spatial revisions.
    struct CanonicalScope
    {
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0, DependencyRevision = 0;
        double OriginX = 0, OriginY = 0;
        bool RenderDroneRails = true;
        B::CellValidity Validity;
        bool operator==(const CanonicalScope& rhs) const noexcept
        {
            if (WorldGeneration != rhs.WorldGeneration ||
                OriginX != rhs.OriginX || OriginY != rhs.OriginY ||
                RenderDroneRails != rhs.RenderDroneRails || Validity != rhs.Validity) return false;
            return (Validity.SafetyGeneration && Validity.LocalEpoch) ||
                (SourceRevision == rhs.SourceRevision && DependencyRevision == rhs.DependencyRevision);
        }
    };
    struct CanonicalIdentity
    {
        CanonicalScope Scope;
        B::CellKey Key;
        uint64_t CellRevision = 0;
        B::CellValidity Validity;
        bool HasUnresolvedContributor = false;
        bool operator==(const CanonicalIdentity& rhs) const noexcept
        {
            if (!(Scope == rhs.Scope) || Key != rhs.Key || HasUnresolvedContributor != rhs.HasUnresolvedContributor) return false;
            return (Validity.SafetyGeneration && Validity.LocalEpoch) || CellRevision == rhs.CellRevision;
        }
    };
    struct CanonicalResult
    {
        CanonicalIdentity Identity;
        std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage> Image;
    };
    std::map<B::CellKey, std::shared_ptr<CanonicalResult>> CanonicalResults;

    void QueueOcclusionChanges(const B::ChangeSet& changes);

    CanonicalIdentity IdentifyCanonicalRequest(const B::CellSnapshot& snapshot, bool renderDroneRails)
    {
        CanonicalIdentity identity;
        identity.Scope = {snapshot.WorldGeneration, snapshot.SourceRevision,
            snapshot.DependencyRevision, snapshot.Origin.X, snapshot.Origin.Y, renderDroneRails, snapshot.Validity};
        identity.Key = snapshot.Key;
        identity.CellRevision = snapshot.CellRevision;
        identity.Validity = snapshot.Validity;
        identity.HasUnresolvedContributor = snapshot.HasUnresolvedContributor;
        return identity;
    }

    // Removes failed/stale/cancelled work on every exit, including exceptions.
    // A reset or reentrant replacement must never be erased by an older owner.
    struct CanonicalWorkScope
    {
        std::shared_ptr<CanonicalResult> Work;
        ~CanonicalWorkScope()
        {
            if (Work->Image) return;
            const auto found = CanonicalResults.find(Work->Identity.Key);
            if (found != CanonicalResults.end() && found->second == Work)
                CanonicalResults.erase(found);
        }
    };

    void QueueOcclusionChanges(const B::ChangeSet& changes)
    {
        if (changes.GlobalCompletenessInvalidated || changes.GlobalSplineOcclusionInvalidated || changes.LocalSafetyInvalidated)
            CanonicalResults.clear();
        else
            for (const auto key : changes.LocalInvalidatedCells) CanonicalResults.erase(key);
        if (changes.GlobalCompletenessInvalidated || changes.GlobalSplineOcclusionInvalidated || changes.LocalSafetyInvalidated)
            MiniMapTexturePublication::InvalidateAll();
        else if (!changes.LocalInvalidatedCells.empty())
            MiniMapTexturePublication::InvalidateCells(changes.LocalInvalidatedCells);
        if (changes.GlobalSplineOcclusionInvalidated)
        {
            PendingGlobalOcclusion = true;
            return;
        }
        PendingOcclusionSections.insert(changes.InvalidatedSections.begin(), changes.InvalidatedSections.end());
    }

    void ClearPendingOcclusion() noexcept
    {
        PendingOcclusionSections.clear();
        PendingGlobalOcclusion = false;
    }

    bool PrepareOccludedSection(B::SectionKey key, uint64_t generation)
    {
        std::shared_ptr<const B::BuildingRecord> spline;
        std::vector<std::shared_ptr<const B::BuildingRecord>> ordinary;
        bool unresolvedOrdinary = false;
        uint64_t dependencyRevision = 0;
        if (!Index.TryGetOcclusionInputs(key, spline, ordinary, unresolvedOrdinary, dependencyRevision) ||
            !spline || !spline->Source) return false;
        const auto& source = *spline->Source;
        const auto sectionIt = std::find_if(source.SplineSections.begin(), source.SplineSections.end(),
            [&](const auto& value) { return value.Identity == key.Identity; });
        if (sectionIt == source.SplineSections.end()) return false;

        std::vector<O::Occluder> occluders;
        std::vector<B::SplineOccluderDependency> identities;
        // Ordinary-building occluders need a sprite capture owned by the shared
        // renderer. This subsystem no longer captures or stores per-building images.
        unresolvedOrdinary = unresolvedOrdinary || !ordinary.empty();

        B::OccludedSplineSection output;
        output.Identity = key.Identity;
        output.DependencyRevision = dependencyRevision;
        std::vector<O::Triangle> input;
        input.reserve(sectionIt->Triangles.size());
        for (size_t i = 0; i < sectionIt->Triangles.size(); ++i)
        {
            O::Triangle triangle;
            triangle.Key = uint64_t(i);
            triangle.Vertices = sectionIt->Triangles[i].Vertices;
            input.push_back(triangle);
        }

        auto prepared = O::PrepareOccluders(occluders);
        if (prepared.GetStatus() != O::Status::Complete)
            unresolvedOrdinary = true;
        else
        {
            std::set<size_t> dependencyOrdinals;
            std::vector<size_t> candidates;
            for (const auto& triangle : input)
            {
                prepared.Candidates(O::Bounds(triangle.Vertices), candidates);
                dependencyOrdinals.insert(candidates.begin(), candidates.end());
            }
            for (size_t ordinal : dependencyOrdinals)
                if (ordinal < identities.size()) output.Dependencies.push_back(identities[ordinal]);

            auto plan = O::Plan(input, prepared);
            if (plan.State == O::Status::Complete)
            {
                output.VisibleTriangles.reserve(plan.Visible.size());
                for (const auto& triangle : plan.Visible)
                    output.VisibleTriangles.push_back({triangle.Vertices});
                bool insetComplete = true;
                const bool insetApplicable = MiniMapBuildingSplinePolicy::IsWalkwayDefinition(
                    true, source.CanonicalBuildingType);
                if (insetApplicable && sectionIt->HasWalkwayInsetBands)
                {
                    A::WalkwayInsetScratch scratch;
                    std::vector<O::Triangle> inset;
                    for (const auto& triangle : plan.Visible)
                        for (const auto& band : sectionIt->WalkwayInsetBands)
                            A::ClipWalkwayInsetTriangle(triangle, band.Vertices, scratch, inset, insetComplete);
                    output.WalkwayInsetTriangles.reserve(inset.size());
                    for (const auto& triangle : inset)
                        output.WalkwayInsetTriangles.push_back({triangle.Vertices});
                }
                const bool walkwayBandsComplete = !insetApplicable || sectionIt->HasWalkwayInsetBands;
                output.Complete = insetComplete && walkwayBandsComplete && !unresolvedOrdinary &&
                    source.SplinePreparation == B::SplinePreparationState::Complete;
            }
            else unresolvedOrdinary = true;
        }

        const auto state = output.Complete ? B::SplineOcclusionState::Complete :
            (unresolvedOrdinary ? B::SplineOcclusionState::Partial : B::SplineOcclusionState::Invalid);
        B::ChangeSet published;
        const bool success = Index.PublishSplineOcclusion(key, generation, std::move(output), state, published);
        if (success)
        {
            QueueOcclusionChanges(published);
            // The published result is current for this section; its changes
            // revoke cell images without scheduling the same section again.
            PendingOcclusionSections.erase(key);
        }
        return success;
    }

    void ServicePendingOcclusion(uint64_t generation)
    {
        if (PendingGlobalOcclusion)
        {
            const auto all = Index.SnapshotSplineSections();
            PendingOcclusionSections.insert(all.begin(), all.end());
            PendingGlobalOcclusion = false;
        }
        std::set<B::SectionKey> pending;
        pending.swap(PendingOcclusionSections);
        while (!pending.empty())
        {
            auto work = pending.extract(pending.begin());
            const auto section = work.value();
            // Source replacement and removal retire their old SectionKeys.
            // Check before work so obsolete identities are not retained.
            if (!Index.IsCurrentSplineSection(section, generation)) continue;
            bool prepared = false;
            try { prepared = PrepareOccludedSection(section, generation); }
            catch (...) { /* Keep this output unevaluated; source/index state remains valid. */ }
            if (!prepared && Index.IsCurrentSplineSection(section, generation))
                PendingOcclusionSections.insert(std::move(work));
        }
    }

    void ResetIndex(uint64_t generation) noexcept
    {
        // Revoke outstanding tile identities before clearing the spatial index.
        Index.InvalidateSafety();
        CanonicalResults.clear();
        MiniMapTexturePublication::InvalidateAll();
        Index.ResetWorld(generation);
        ClearPendingOcclusion();
    }

    void ServiceInternal(uint64_t consumerSession) noexcept
    {
        try
        {
            // Service is entered only by the accepted BCT consumer session.
            // Empty demand is a strict no-op for collector and tile work.
            if (!MiniMapBuildingComplexApiInternal::ServiceCollectorForVisibleRequests(consumerSession)) return;
            ServicePendingOcclusion(Current.WorldGeneration);
            MiniMapBuildingComplexApiInternal::ServiceRequests(consumerSession);
            MiniMapTexturePublication::ProcessRequests();
            MiniMapTexturePublication::Service();
        }
        catch (...)
        {
            // Failed tile and publication service does not invalidate spatial records.
        }
    }
}

namespace MiniMapBuildingComplexWorld
{
    void Service(uint64_t consumerSession) noexcept
    {
        ServiceInternal(consumerSession);
    }

    bool OpenCollectorSession(uint64_t consumerSession, SDK::UWorld* world,
        double originX, double originY) noexcept
    {
        if (!consumerSession || !world || !std::isfinite(originX) || !std::isfinite(originY) || ConsumerCollector ||
            ConsumerCollectorSession || ConsumerCollectorWorld || MiniMapMap::GetWorld() != world) return false;

        std::unique_ptr<MiniMapBuildingCollector::BuildingCollector> candidate;
        try
        {
            candidate = std::make_unique<MiniMapBuildingCollector::BuildingCollector>();
            if (!candidate->InitializeResources() || !candidate->BeginExplicitSession())
            {
                candidate->ShutdownResources();
                return false;
            }
        }
        catch (...)
        {
            if (candidate) candidate->ShutdownResources();
            return false;
        }

        if (MiniMapMap::GetWorld() != world)
        {
            candidate->ShutdownResources();
            return false;
        }

        ResetIndex(consumerSession);
        Index.BeginWorld(consumerSession, {originX, originY, true});
        Current.WorldGeneration = consumerSession;
        ConsumerCollector = std::move(candidate);
        ConsumerCollectorSession = consumerSession;
        ConsumerCollectorWorld = world;
        return true;
    }

    bool ServiceCollectorRegion(uint64_t consumerSession, const B::WorldBounds& region) noexcept
    {
        try
        {
            const auto* native = MiniMapNative::Get();
            if (!native || !native->player.isGameThread || !native->player.isGameThread() ||
                !region.Valid || !std::isfinite(region.MinX) || !std::isfinite(region.MinY) ||
                !std::isfinite(region.MaxX) || !std::isfinite(region.MaxY) ||
                region.MinX >= region.MaxX || region.MinY >= region.MaxY ||
                !ConsumerCollector || !consumerSession || ConsumerCollectorSession != consumerSession ||
                !ConsumerCollectorWorld || MiniMapMap::GetWorld() != ConsumerCollectorWorld)
                return false;

            C::Bounds collectorRegion{region.MinX, region.MinY, region.MaxX, region.MaxY, true};
            ConsumerCollector->SetRegion(collectorRegion);
            ConsumerCollector->SetRenderDroneRails(true);
            ConsumerCollector->OnTick(0.0f);
            if (ConsumerCollectorSession != consumerSession || MiniMapMap::GetWorld() != ConsumerCollectorWorld)
                return false;
            const auto snapshot = ConsumerCollector->GetSnapshot();
            const auto diagnostics = ConsumerCollector->GetDiagnostics();
            if (!snapshot || snapshot->Generation != diagnostics.PublicationGeneration ||
                !snapshot->Region.Valid || snapshot->Region.MinX != collectorRegion.MinX ||
                snapshot->Region.MinY != collectorRegion.MinY || snapshot->Region.MaxX != collectorRegion.MaxX ||
                snapshot->Region.MaxY != collectorRegion.MaxY)
                return true;

            std::map<B::MassKey, const C::Record*> previous;
            if (LastCollectorSnapshot)
                for (const auto& record : LastCollectorSnapshot->Records)
                    previous.emplace(B::MassKey{record.Index, record.Serial}, &record);

            std::set<B::MassKey> observed;
            for (const auto& record : snapshot->Records)
            {
                const B::MassKey key{record.Index, record.Serial};
                if (key.Index < 0 || key.Serial <= 0) continue;
                observed.insert(key);

                auto revision = CollectorSourceRevisions.find(key);
                bool sameSource = false;
                if (revision != CollectorSourceRevisions.end())
                {
                    const auto old = previous.find(key);
                    sameSource = old != previous.end() && old->second->Generation == record.Generation &&
                        old->second->BuildingId == record.BuildingId && old->second->TransformValid == record.TransformValid &&
                        old->second->DefinitionValid == record.DefinitionValid && old->second->HasSpline == record.HasSpline &&
                        old->second->InteriorKnown == record.InteriorKnown && old->second->InteriorOnly == record.InteriorOnly &&
                        old->second->Position == record.Position && old->second->Scale == record.Scale &&
                        old->second->Rotation == record.Rotation && old->second->Extent.MinX == record.Extent.MinX &&
                        old->second->Extent.MinY == record.Extent.MinY && old->second->Extent.MaxX == record.Extent.MaxX &&
                        old->second->Extent.MaxY == record.Extent.MaxY && old->second->Extent.Valid == record.Extent.Valid &&
                        ((!old->second->CanonicalBlueprint && !record.CanonicalBlueprint) ||
                            (old->second->CanonicalBlueprint && record.CanonicalBlueprint &&
                                *old->second->CanonicalBlueprint == *record.CanonicalBlueprint)) &&
                        old->second->Curve == record.Curve && old->second->DefinitionShape == record.DefinitionShape;
                }
                if (!sameSource)
                {
                    if (CollectorSourceRevision == (std::numeric_limits<uint64_t>::max)()) return false;
                    const uint64_t next = ++CollectorSourceRevision;
                    if (revision == CollectorSourceRevisions.end())
                        revision = CollectorSourceRevisions.emplace(key, next).first;
                    else revision->second = next;
                }

                auto source = std::make_shared<B::SourcePayload>();
                source->SourceRevision = revision->second;
                source->SourceGeneration = consumerSession;
                source->CanonicalBuildingType = record.BuildingId;
                source->BlueprintIdentity = record.CanonicalBlueprint;
                source->InteriorKnown = record.InteriorKnown;
                source->InteriorOnly = record.InteriorOnly;
                source->Transform = {record.Position, record.Scale, record.Rotation};
                source->Support = B::SupportState::Unknown;
                if (record.CanonicalBlueprint)
                    source->BaseCoreOverlay = *record.CanonicalBlueprint ==
                        "/Game/Chimera/Buildings/BaseCore/BP_BaseCore.BP_BaseCore_C";

                if (!MiniMapBuildingComplexSourceAcquisition::PrepareSplineGeometry(key, record, *source))
                {
                    source->SplineSections.clear();
                    source->PreparedSplineSupport = {};
                    source->SplinePreparation = record.HasSpline ?
                        B::SplinePreparationState::Invalid : B::SplinePreparationState::NotPresent;
                }

                B::BuildingRecord indexed;
                indexed.Key = key;
                indexed.WorldGeneration = consumerSession;
                indexed.Source = std::move(source);
                B::ChangeSet changes;
                const auto result = Index.Upsert(std::move(indexed), changes);
                if (result == B::UpsertResult::Inserted || result == B::UpsertResult::Replaced)
                {
                    IndexedCollectorKeys.insert(key);
                    QueueOcclusionChanges(changes);
                }
            }

            const bool safeAbsence = snapshot->Complete && diagnostics.PendingAcquisitionCount == 0;
            if (safeAbsence)
            {
                for (auto it = IndexedCollectorKeys.begin(); it != IndexedCollectorKeys.end();)
                {
                    if (observed.contains(*it)) { ++it; continue; }
                    B::ChangeSet changes;
                    const auto removed = Index.Remove(*it, consumerSession, changes);
                    if (removed == B::RemoveResult::Removed)
                    {
                        CollectorSourceRevisions.erase(*it);
                        QueueOcclusionChanges(changes);
                    }
                    it = IndexedCollectorKeys.erase(it);
                }
            }
            LastCollectorSnapshot = snapshot;
            return true;
        }
        catch (...) { return false; }
    }

    void CloseCollectorSession(uint64_t consumerSession) noexcept
    {
        if (ConsumerCollector && ConsumerCollectorSession == consumerSession)
        {
            ReleaseConsumerCollector();
            ResetIndex(0);
            Current = {};
        }
    }

    bool OwnsCollectorSession(uint64_t consumerSession, SDK::UWorld* world) noexcept
    {
        return consumerSession && world && ConsumerCollector &&
            ConsumerCollectorSession == consumerSession && ConsumerCollectorWorld == world;
    }

    void Shutdown() noexcept
    {
        ReleaseConsumerCollector();
        ResetIndex(0);
        Current = {};
    }

    Status GetStatus() noexcept
    {
        return Current;
    }
    MiniMapBuildingComplexIndex::Index& GetIndex() noexcept { return Index; }

    CellCompositionResult ComposeCell(B::CellKey key, uint64_t generation,
        std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage>& output) noexcept
    {
        namespace R = MiniMapBuildingComplexCellRasterizer;
        output.reset();
        try
        {
            const auto* native = MiniMapNative::Get();
            if (!native || !native->player.isGameThread || !native->player.isGameThread())
                return CellCompositionResult::Failed;
            const auto initialStatus = GetStatus();
            if (!generation || initialStatus.WorldGeneration != generation) return CellCompositionResult::Failed;

            if (Index.ObserveRenderingPolicy(MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails))
            { CanonicalResults.clear(); MiniMapTexturePublication::InvalidateAll(); }
            B::CellSnapshot snapshot;
            if (!Index.TrySnapshot(key, snapshot) || snapshot.WorldGeneration != generation || !snapshot.Origin.Valid)
                return CellCompositionResult::Failed;
            const bool renderDroneRails = MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails;
            auto identity = IdentifyCanonicalRequest(snapshot, renderDroneRails);
            // Recheck the demanded key against its current local token below.
            auto found = CanonicalResults.find(key);
            if (found != CanonicalResults.end())
            {
                const auto cached = found->second;
                if (cached->Identity == identity)
                {
                    // Synchronous GT callers cannot wait for their own reentrant
                    // stack. They observe the same work and may retry after it
                    // returns; they never start an independent rasterization.
                    if (!cached->Image) return CellCompositionResult::Pending;
                    if (ValidateCellImage(*cached->Image))
                    {
                        output = cached->Image;
                        return output->Status == R::State::Complete ?
                            CellCompositionResult::Complete : CellCompositionResult::Partial;
                    }
                }
                CanonicalResults.erase(key);
            }
            auto work = std::make_shared<CanonicalResult>();
            work->Identity = std::move(identity);
            CanonicalResults.emplace(key, work);
            CanonicalWorkScope workScope{work};
            std::vector<R::SpriteLease> sprites;

            R::CellImage composed;
            const auto state = R::Compose(snapshot, sprites, renderDroneRails, composed);
            if (state == R::State::Failed) return CellCompositionResult::Failed;
            if (!ValidateCellImage(composed)) return CellCompositionResult::Stale;

            // Recheck that the spatial snapshot still identifies this work.
            B::CellSnapshot current;
            if (!Index.TrySnapshot(key, current) || !current.Origin.Valid ||
                IdentifyCanonicalRequest(current,
                    MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails) != work->Identity)
                return CellCompositionResult::Stale;
            const auto owned = CanonicalResults.find(key);
            if (owned == CanonicalResults.end() || owned->second != work)
                return CellCompositionResult::Stale;
            work->Image = std::make_shared<const R::CellImage>(std::move(composed));
            output = work->Image;
            return state == R::State::Complete ? CellCompositionResult::Complete : CellCompositionResult::Partial;
        }
        catch (...) { output.reset(); return CellCompositionResult::Failed; }
    }

    CellCompositionResult RequestCellPublication(B::CellKey key, uint64_t generation,
        uint64_t& requestId) noexcept
    {
        requestId = 0;
        std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage> image;
        const auto result = ComposeCell(key, generation, image);
        if (result != CellCompositionResult::Complete || !image) return result;
        requestId = MiniMapTexturePublication::RequestCell(std::move(image));
        return requestId ? CellCompositionResult::Complete : CellCompositionResult::Failed;
    }

    bool ValidateCellImage(const MiniMapBuildingComplexCellRasterizer::CellImage& image) noexcept
    {
        try
        {
            return ValidateCellImage(MiniMapBuildingComplexCellRasterizer::Identify(image));
        }
        catch (...) { return false; }
    }

    bool ValidateCellImage(const MiniMapBuildingComplexCellRasterizer::CellImageIdentity& identity) noexcept
    {
        try
        {
            const bool renderDroneRails = MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails;
            if (Index.ObserveRenderingPolicy(renderDroneRails))
            { CanonicalResults.clear(); MiniMapTexturePublication::InvalidateAll(); }
            const auto status = GetStatus();
            if (!identity.WorldGeneration ||
                status.WorldGeneration != identity.WorldGeneration ||
                renderDroneRails != identity.RenderDroneRails) return false;
            B::CellSnapshot current;
            if (!Index.TrySnapshot(identity.Key, current) || current.WorldGeneration != identity.WorldGeneration ||
                !current.Origin.Valid || !identity.Origin.Valid || current.Origin.X != identity.Origin.X ||
                current.Origin.Y != identity.Origin.Y || current.Validity != identity.Validity) return false;
            const bool locallyCertified = current.Validity.SafetyGeneration != 0 && current.Validity.LocalEpoch != 0;
            if (!locallyCertified && (current.CellRevision != identity.CellRevision ||
                current.SourceRevision != identity.SourceRevision || current.DependencyRevision != identity.DependencyRevision)) return false;
            if (identity.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
                current.HasUnresolvedContributor) return false;
            return true;
        }
        catch (...) { return false; }
    }

    namespace
    {
        namespace B = MiniMapBuildingComplexIndex;
        constexpr uint32_t kTilePolicyR1 = 1;
        constexpr int64_t kCanonicalPixelsPerCell = 256;
        constexpr int64_t kCanonicalPixelsPerTile = 1024;
        constexpr int64_t kTileCells = 4;
        constexpr std::array<uint32_t, 4> kTileSides{128, 256, 512, 1024};

        struct GroupWorkKey
        {
            TileKey Key;
            uint32_t Side = 0;
            bool operator<(const GroupWorkKey& rhs) const noexcept
            { return Key < rhs.Key || (!(rhs.Key < Key) && Side < rhs.Side); }
        };
        std::set<GroupWorkKey> ActiveTileWork;

        bool MultiplyChecked(int64_t value, int64_t positive, int64_t& result) noexcept
        {
            if (positive <= 0 || value > (std::numeric_limits<int64_t>::max)() / positive ||
                value < (std::numeric_limits<int64_t>::min)() / positive) return false;
            result = value * positive;
            return true;
        }

        bool AddChecked(int64_t value, int64_t delta, int64_t& result) noexcept
        {
            if ((delta > 0 && value > (std::numeric_limits<int64_t>::max)() - delta) ||
                (delta < 0 && value < (std::numeric_limits<int64_t>::min)() - delta)) return false;
            result = value + delta;
            return true;
        }

        int64_t FloorDiv256(int64_t value) noexcept
        {
            int64_t quotient = value / kCanonicalPixelsPerCell;
            if (value % kCanonicalPixelsPerCell < 0) --quotient;
            return quotient;
        }

        bool RequiredCanonicalCells(TileKey key, uint32_t side,
            std::vector<B::CellKey>& output, int64_t& pixelOriginX, int64_t& pixelOriginY) noexcept
        {
            output.clear();
            if (std::find(kTileSides.begin(), kTileSides.end(), side) == kTileSides.end()) return false;
            int64_t baseCellX = 0, baseCellY = 0;
            if (!MultiplyChecked(key.X, kTileCells, baseCellX) || !MultiplyChecked(key.Y, kTileCells, baseCellY) ||
                !MultiplyChecked(key.X, kCanonicalPixelsPerTile, pixelOriginX) ||
                !MultiplyChecked(key.Y, kCanonicalPixelsPerTile, pixelOriginY)) return false;
            const int64_t stride = kCanonicalPixelsPerTile / int64_t(side);
            int64_t firstX = 0, firstY = 0, endX = 0, endY = 0;
            if (!AddChecked(pixelOriginX, -stride, firstX) || !AddChecked(pixelOriginY, -stride, firstY) ||
                !AddChecked(pixelOriginX, kCanonicalPixelsPerTile + stride, endX) ||
                !AddChecked(pixelOriginY, kCanonicalPixelsPerTile + stride, endY) ||
                endX <= firstX || endY <= firstY) return false;
            const int64_t cellMinX = FloorDiv256(firstX), cellMinY = FloorDiv256(firstY);
            const int64_t cellMaxX = FloorDiv256(endX - 1), cellMaxY = FloorDiv256(endY - 1);
            // One output sample is at most eight canonical texels; therefore
            // each edge and corner can depend on only the adjacent cell.
            if (cellMinX != baseCellX - 1 || cellMinY != baseCellY - 1 ||
                cellMaxX != baseCellX + kTileCells || cellMaxY != baseCellY + kTileCells) return false;
            try
            {
                output.reserve(36);
                for (int64_t x = cellMinX; x <= cellMaxX; ++x)
                    for (int64_t y = cellMinY; y <= cellMaxY; ++y)
                        output.push_back({x, y});
                return output.size() == 36;
            }
            catch (...) { output.clear(); return false; }
        }

        bool ValidTileBounds(TileKey key, B::WorldOrigin origin) noexcept
        {
            if (!origin.Valid || !std::isfinite(origin.X) || !std::isfinite(origin.Y)) return false;
            const long double minX = static_cast<long double>(origin.X) +
                static_cast<long double>(key.X) * 50000.0L;
            const long double minY = static_cast<long double>(origin.Y) +
                static_cast<long double>(key.Y) * 50000.0L;
            const long double maxX = minX + 50000.0L, maxY = minY + 50000.0L;
            const double convertedMinX = static_cast<double>(minX), convertedMinY = static_cast<double>(minY);
            const double convertedMaxX = static_cast<double>(maxX), convertedMaxY = static_cast<double>(maxY);
            return std::isfinite(minX) && std::isfinite(minY) && std::isfinite(maxX) && std::isfinite(maxY) &&
                std::isfinite(convertedMinX) && std::isfinite(convertedMinY) &&
                std::isfinite(convertedMaxX) && std::isfinite(convertedMaxY) &&
                convertedMinX < convertedMaxX && convertedMinY < convertedMaxY;
        }

        bool HalfToDouble(uint16_t half, double& value) noexcept
        {
            const uint16_t exponent = uint16_t((half >> 10) & 0x1F);
            const uint16_t mantissa = uint16_t(half & 0x03FF);
            if (exponent == 0x1F) return false;
            value = exponent == 0 ? std::ldexp(double(mantissa), -24) :
                std::ldexp(double(1024 + mantissa), int(exponent) - 25);
            if (half & 0x8000) value = -value;
            return std::isfinite(value);
        }

        uint16_t DoubleToHalf(double value) noexcept
        {
            const uint16_t sign = std::signbit(value) ? 0x8000 : 0;
            const double magnitude = std::abs(value);
            if (magnitude == 0) return sign;
            auto roundEven = [](double input) noexcept -> uint32_t {
                const double floorValue = std::floor(input);
                uint32_t integer = static_cast<uint32_t>(floorValue);
                const double fraction = input - floorValue;
                if (fraction > 0.5 || (fraction == 0.5 && (integer & 1))) ++integer;
                return integer;
            };
            if (magnitude < std::ldexp(1.0, -14))
            {
                const uint32_t subnormal = roundEven(std::ldexp(magnitude, 24));
                return uint16_t(sign | (subnormal >= 1024 ? 0x0400 : subnormal));
            }
            int exponent = 0;
            std::frexp(magnitude, &exponent);
            int halfExponent = exponent - 1 + 15;
            uint32_t significand = roundEven(std::ldexp(magnitude, 10 - (exponent - 1)));
            if (significand == 2048) { significand = 1024; ++halfExponent; }
            if (halfExponent >= 31) return uint16_t(sign | 0x7C00);
            return uint16_t(sign | (uint16_t(halfExponent) << 10) | uint16_t(significand - 1024));
        }

        bool ReadCanonical(const std::map<B::CellKey,
            std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage>>& cells,
            int64_t pixelX, int64_t pixelY, std::array<double, 4>& channels) noexcept
        {
            const int64_t cellX = FloorDiv256(pixelX), cellY = FloorDiv256(pixelY);
            const B::CellKey key{cellX, cellY};
            const auto found = cells.find(key);
            if (found == cells.end() || !found->second ||
                found->second->Rgba16F.size() != size_t(kCanonicalPixelsPerCell * kCanonicalPixelsPerCell * 8))
                return false;
            int64_t localX = pixelX - cellX * kCanonicalPixelsPerCell;
            int64_t localY = pixelY - cellY * kCanonicalPixelsPerCell;
            if (localX < 0 || localX >= kCanonicalPixelsPerCell || localY < 0 || localY >= kCanonicalPixelsPerCell)
                return false;
            const size_t offset = (size_t(localY) * size_t(kCanonicalPixelsPerCell) + size_t(localX)) * 8;
            for (size_t channel = 0; channel < channels.size(); ++channel)
            {
                const uint16_t half = uint16_t(found->second->Rgba16F[offset + channel * 2]) |
                    uint16_t(uint16_t(found->second->Rgba16F[offset + channel * 2 + 1]) << 8);
                if (!HalfToDouble(half, channels[channel]) || channels[channel] < 0 || channels[channel] > 1)
                    return false;
            }
            return true;
        }

        bool FilterTile(uint32_t side, int64_t pixelOriginX, int64_t pixelOriginY,
            const std::map<B::CellKey, std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage>>& cells,
            std::vector<uint8_t>& output) noexcept
        {
            try
            {
                const int64_t stride = kCanonicalPixelsPerTile / int64_t(side);
                const uint32_t physicalSide = side + 2;
                output.resize(size_t(physicalSide) * physicalSide * 8);
                std::array<double, 4> values{};
                for (int64_t j = -1; j <= int64_t(side); ++j)
                {
                    int64_t rowStart = 0;
                    if (!AddChecked(pixelOriginY, j * stride, rowStart)) return false;
                    for (int64_t i = -1; i <= int64_t(side); ++i)
                    {
                        int64_t columnStart = 0;
                        if (!AddChecked(pixelOriginX, i * stride, columnStart)) return false;
                        std::array<double, 4> sums{};
                        for (int64_t sy = 0; sy < stride; ++sy)
                            for (int64_t sx = 0; sx < stride; ++sx)
                            {
                                if (!ReadCanonical(cells, columnStart + sx, rowStart + sy, values)) return false;
                                for (size_t channel = 0; channel < sums.size(); ++channel) sums[channel] += values[channel];
                            }
                        const double divisor = double(stride * stride);
                        const size_t pixel = (size_t(j + 1) * physicalSide + size_t(i + 1)) * 8;
                        for (size_t channel = 0; channel < sums.size(); ++channel)
                        {
                            const double average = sums[channel] / divisor;
                            if (!std::isfinite(average) || average < 0 || average > 1) return false;
                            const uint16_t half = DoubleToHalf(average);
                            output[pixel + channel * 2] = uint8_t(half & 0xFF);
                            output[pixel + channel * 2 + 1] = uint8_t(half >> 8);
                        }
                    }
                }
                return true;
            }
            catch (...) { output.clear(); return false; }
        }

        bool IsClearImage(const std::vector<uint8_t>& pixels) noexcept
        {
            constexpr std::array<uint8_t, 8> clear{0, 0, 0, 0, 0, 0, 0, 0x3C};
            if (pixels.empty() || pixels.size() % clear.size()) return false;
            for (size_t offset = 0; offset < pixels.size(); offset += clear.size())
                if (!std::equal(clear.begin(), clear.end(), pixels.begin() + offset)) return false;
            return true;
        }

        struct TileWorkScope
        {
            GroupWorkKey Key;
            ~TileWorkScope() { ActiveTileWork.erase(Key); }
        };
    }

    int64_t FloorDiv4(int64_t value) noexcept
    {
        int64_t quotient = value / 4;
        if (value % 4 < 0) --quotient;
        return quotient;
    }

    TileKey TileForCell(B::CellKey key) noexcept
    { return {FloorDiv4(key.X), FloorDiv4(key.Y)}; }

    uint32_t SelectTileResolution(double projectedTilePixels, uint32_t currentSide) noexcept
    {
        if (!std::isfinite(projectedTilePixels) || projectedTilePixels < 0) return 0;
        auto initial = [&]() noexcept {
            for (const uint32_t side : kTileSides) if (double(side) >= projectedTilePixels) return side;
            return kTileSides.back();
        };
        if (std::find(kTileSides.begin(), kTileSides.end(), currentSide) == kTileSides.end()) return initial();
        if (projectedTilePixels > double(currentSide) ||
            projectedTilePixels <= 0.40 * double(currentSide)) return initial();
        return currentSide;
    }

    TileResult ComposeTile(TileKey key, uint64_t generation, uint32_t contentSide) noexcept
    {
        namespace R = MiniMapBuildingComplexCellRasterizer;
        TileResult result;
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.isGameThread || !native->player.isGameThread()) return result;
        GroupWorkKey workKey{key, contentSide};
        try
        {
            if (!ActiveTileWork.insert(workKey).second)
            { result.State = TileState::Pending; return result; }
        }
        catch (...) { return result; }
        TileWorkScope workScope{workKey};

        try
        {
            std::vector<B::CellKey> required;
            int64_t pixelOriginX = 0, pixelOriginY = 0;
            if (!RequiredCanonicalCells(key, contentSide, required, pixelOriginX, pixelOriginY)) return result;
            const Status initialStatus = GetStatus();
            if (!generation || initialStatus.WorldGeneration != generation)
            { result.State = TileState::Failed; return result; }

            std::map<B::CellKey, std::shared_ptr<const R::CellImage>> cells;
            bool partial = false;
            for (const auto cellKey : required)
            {
                std::shared_ptr<const R::CellImage> image;
                const auto composed = ComposeCell(cellKey, generation, image);
                if (composed == CellCompositionResult::Pending)
                { result.State = TileState::Pending; return result; }
                if (composed == CellCompositionResult::Stale)
                { result.State = TileState::Stale; return result; }
                if (composed == CellCompositionResult::Failed || !image)
                { result.State = TileState::Failed; return result; }
                partial |= composed == CellCompositionResult::Partial;
                cells.emplace(cellKey, std::move(image));
            }

            auto grouped = std::make_shared<GroupedTileImage>();
            auto& identity = grouped->Identity;
            identity.WorldGeneration = generation;
            identity.Key = key;
            identity.PolicyId = kTilePolicyR1;
            identity.ContentSide = contentSide;
            identity.RenderDroneRails = MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails;
            identity.Inputs.reserve(required.size());
            for (const auto cellKey : required)
            {
                const auto& image = cells.at(cellKey);
                if (!image->Origin.Valid || image->WorldGeneration != generation ||
                    image->RenderDroneRails != identity.RenderDroneRails) return result;
                if (identity.Inputs.empty()) identity.Origin = image->Origin;
                else if (identity.Origin.X != image->Origin.X || identity.Origin.Y != image->Origin.Y) return result;
                identity.Inputs.push_back(R::Identify(*image));
                if (image->Status != R::State::Complete) partial = true;
            }
            if (!ValidTileBounds(key, identity.Origin)) return result;

            for (const auto& input : identity.Inputs)
                if (!ValidateCellImage(input)) { result.State = TileState::Stale; return result; }
            if (partial)
            {
                identity.State = TileState::Partial;
                result.State = TileState::Partial;
                result.Image = std::move(grouped); // No unsafe partial pixels are exposed.
                return result;
            }

            if (!FilterTile(contentSide, pixelOriginX, pixelOriginY, cells, grouped->Rgba16F))
            { result.State = TileState::Failed; return result; }
            for (const auto& input : identity.Inputs)
                if (!ValidateCellImage(input)) { result.State = TileState::Stale; return result; }

            grouped->Width = grouped->Height = contentSide + 2;
            identity.State = IsClearImage(grouped->Rgba16F) ? TileState::Empty : TileState::Complete;
            result.State = identity.State;
            if (identity.State == TileState::Empty) grouped->Rgba16F.clear();
            result.Image = std::move(grouped);
            return result;
        }
        catch (...) { result.State = TileState::Failed; result.Image.reset(); return result; }
    }

    bool ValidateTile(const TileIdentity& identity) noexcept
    {
        try
        {
            namespace R = MiniMapBuildingComplexCellRasterizer;
            if (!identity.WorldGeneration || !ValidTileBounds(identity.Key, identity.Origin) || identity.PolicyId != kTilePolicyR1 ||
                std::find(kTileSides.begin(), kTileSides.end(), identity.ContentSide) == kTileSides.end() ||
                (identity.State != TileState::Partial && identity.State != TileState::Complete &&
                 identity.State != TileState::Empty)) return false;
            const auto status = GetStatus();
            if (status.WorldGeneration != identity.WorldGeneration)
                return false;
            const bool renderDroneRails = MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails;
            if (renderDroneRails != identity.RenderDroneRails) return false;

            std::vector<B::CellKey> required;
            int64_t pixelOriginX = 0, pixelOriginY = 0;
            if (!RequiredCanonicalCells(identity.Key, identity.ContentSide, required, pixelOriginX, pixelOriginY) ||
                required.size() != identity.Inputs.size()) return false;
            bool allComplete = true;
            bool hasPartial = false;
            for (size_t i = 0; i < required.size(); ++i)
            {
                const auto& input = identity.Inputs[i];
                if (input.Key != required[i] || input.WorldGeneration != identity.WorldGeneration ||
                    !input.Origin.Valid ||
                    input.Origin.X != identity.Origin.X || input.Origin.Y != identity.Origin.Y ||
                    input.RenderDroneRails != identity.RenderDroneRails ||
                    (input.Status != R::State::Complete && input.Status != R::State::Partial) ||
                    !ValidateCellImage(input)) return false;
                allComplete &= input.Status == R::State::Complete;
                hasPartial |= input.Status == R::State::Partial;
            }
            return identity.State == TileState::Partial ? hasPartial : allComplete;
        }
        catch (...) { return false; }
    }

    bool ValidateGroupedTileImage(const GroupedTileImage& image) noexcept
    {
        const auto& identity = image.Identity;
        if (!ValidateTile(identity) || identity.State != image.Identity.State ||
            (identity.State != TileState::Complete && identity.State != TileState::Empty) ||
            std::find(kTileSides.begin(), kTileSides.end(), identity.ContentSide) == kTileSides.end() ||
            image.Width != identity.ContentSide + 2 || image.Height != identity.ContentSide + 2)
            return false;
        if (identity.State == TileState::Empty) return image.Rgba16F.empty();
        const uint64_t expected = uint64_t(image.Width) * uint64_t(image.Height) * 8;
        if (!expected || expected != image.Rgba16F.size()) return false;
        for (size_t offset = 0; offset < image.Rgba16F.size(); offset += 2)
        {
            const uint16_t half = uint16_t(image.Rgba16F[offset]) |
                uint16_t(uint16_t(image.Rgba16F[offset + 1]) << 8);
            double value = 0;
            if (!HalfToDouble(half, value) || value < 0 || value > 1 ||
                (offset % 8 == 6 && value > 1)) return false;
        }
        return true;
    }
}
#endif
