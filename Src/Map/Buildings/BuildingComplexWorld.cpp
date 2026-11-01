#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexWorld.h"
#include "BuildingComplexSourceAcquisition.h"
#include "Config/Config.h"
#include "BuildingScheduler.h"
#include "BuildingSpriteRenderer.h"
#include "BuildingCache.h"
#include "BuildingSpriteGeometryCore.h"
#include "BuildingSplineOcclusionAdapter.h"
#include "Map/Map.h"
#include "Map/Terrain/TerrainSource.h"
#include "Native/MassLifecycle.h"
#include "Native/NativeApi.h"
#include "Native/TexturePublication.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace C = MiniMapBuildingCollector;
    namespace A = MiniMapBuildingSplineOcclusionAdapter;
    namespace O = MiniMapBuildingSplineOcclusion;
    namespace L = MiniMapMassLifecycle;
    using Event = L::Event;
    using Handle = MiniMapNative::MassEntityHandle;
    constexpr size_t kQuerySize = 0x350;
    constexpr size_t kQueryAlignment = 0x10;
    constexpr size_t kEventBatch = 256;

    struct MassKeyHash
    {
        size_t operator()(B::MassKey key) const noexcept
        {
            const uint64_t packed = uint64_t(uint32_t(key.Index)) |
                (uint64_t(uint32_t(key.Serial)) << 32);
            return std::hash<uint64_t>{}(packed);
        }
    };

    B::Index Index;
    MiniMapBuildingComplexWorld::Status Current;
    SDK::UScriptStruct* BuildingTag = nullptr;
    bool BuildingTagRooted = false;
    uint64_t AppliedEpoch = 0;
    uint64_t AppliedSequence = 0;
    std::deque<B::MassKey> SourceQueue;
    std::unordered_set<B::MassKey, MassKeyHash> SourceQueued;

    struct CaptureTrack
    {
        uint64_t RequestId = 0;
        uint64_t SourceRevision = 0;
        std::string CanonicalBlueprintIdentity;
        std::shared_ptr<const B::SourcePayload> Snapshot;
        bool Terminal = false;
    };
    std::unordered_map<B::MassKey, CaptureTrack, MassKeyHash> CaptureRequests;
    std::mutex CaptureImagesMutex;
    struct CompletedCaptureImage
    {
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0;
        uint64_t ImageRevision = 0;
        std::string CanonicalBlueprintIdentity;
        std::shared_ptr<const MiniMapBuildingCache::Sprite> Image;
    };
    std::unordered_map<B::MassKey, CompletedCaptureImage, MassKeyHash> CaptureImages;
    std::set<B::SectionKey> PendingOcclusionSections;
    bool PendingGlobalOcclusion = false;
    struct DeferredCaptureRetry
    {
        B::MassKey Key;
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0;
        std::string CanonicalBlueprintIdentity;
    };
    std::vector<DeferredCaptureRetry> DeferredCaptureRetries;
    uint64_t NextCaptureRequestId = 1;
    uint64_t NextCaptureImageRevision = 1;

    // Game-thread owner state, like membership servicing. Global revisions
    // remain audit metadata and are identity fields only when certification is unavailable.
    struct CanonicalScope
    {
        uint64_t WorldGeneration = 0, RestoreEpoch = 0;
        uint64_t SourceRevision = 0, DependencyRevision = 0;
        double OriginX = 0, OriginY = 0;
        bool RenderDroneRails = true;
        B::CellValidity Validity;
        bool operator==(const CanonicalScope& rhs) const noexcept
        {
            if (WorldGeneration != rhs.WorldGeneration || RestoreEpoch != rhs.RestoreEpoch ||
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
        // Includes absent eligible captures (ImageRevision == 0), even when
        // the rasterizer could not draw them into its successful manifest.
        std::vector<MiniMapBuildingComplexCellRasterizer::CaptureRevision> Captures;
        bool operator==(const CanonicalIdentity& rhs) const noexcept
        {
            if (!(Scope == rhs.Scope) || Key != rhs.Key || HasUnresolvedContributor != rhs.HasUnresolvedContributor ||
                Captures != rhs.Captures) return false;
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

    void NotifyCaptureValidityChange(B::MassKey key) noexcept
    {
        B::ChangeSet changes;
        Index.NotifyCaptureChange(key, Current.WorldGeneration, changes);
        QueueOcclusionChanges(changes);
    }

    void InvalidateCanonicalCapture(B::MassKey key) noexcept
    {
        NotifyCaptureValidityChange(key);
        for (auto it = CanonicalResults.begin(); it != CanonicalResults.end();)
        {
            const auto& captures = it->second->Identity.Captures;
            if (std::any_of(captures.begin(), captures.end(),
                [&](const auto& capture) { return capture.Building == key; }))
                it = CanonicalResults.erase(it);
            else ++it;
        }
    }

    CanonicalIdentity IdentifyCanonicalRequest(const B::CellSnapshot& snapshot,
        uint64_t restoreEpoch, bool renderDroneRails)
    {
        CanonicalIdentity identity;
        identity.Scope = {snapshot.WorldGeneration, restoreEpoch, snapshot.SourceRevision,
            snapshot.DependencyRevision, snapshot.Origin.X, snapshot.Origin.Y, renderDroneRails, snapshot.Validity};
        identity.Key = snapshot.Key;
        identity.CellRevision = snapshot.CellRevision;
        identity.Validity = snapshot.Validity;
        identity.HasUnresolvedContributor = snapshot.HasUnresolvedContributor;
        identity.Captures.reserve(snapshot.Records.size());
        // Metadata only: no image acquisition, capture operation or engine call.
        std::lock_guard lock(CaptureImagesMutex);
        for (const auto& record : snapshot.Records)
        {
            if (!record || !record->Source || !record->Source->InteriorKnown ||
                record->Source->InteriorOnly || record->Source->HasSplineSource) continue;
            const auto& source = *record->Source;
            uint64_t revision = 0;
            const auto found = CaptureImages.find(record->Key);
            if (source.SourceGeneration == snapshot.WorldGeneration && source.BlueprintIdentity &&
                found != CaptureImages.end() && found->second.WorldGeneration == snapshot.WorldGeneration &&
                found->second.SourceRevision == source.SourceRevision && found->second.Image &&
                found->second.CanonicalBlueprintIdentity == *source.BlueprintIdentity)
                revision = found->second.ImageRevision;
            identity.Captures.push_back({record->Key, source.SourceRevision, revision});
        }
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

    bool ValidBounds(const B::WorldBounds& bounds) noexcept
    {
        return bounds.Valid && std::isfinite(bounds.MinX) && std::isfinite(bounds.MinY) &&
            std::isfinite(bounds.MaxX) && std::isfinite(bounds.MaxY) &&
            bounds.MinX <= bounds.MaxX && bounds.MinY <= bounds.MaxY;
    }

    void ClassifyVisualSupport(B::SourcePayload& source) noexcept
    {
        source.Support = B::SupportState::Unknown;
        source.VisualSupport = {};
        if (!source.InteriorKnown) return;
        if (source.InteriorOnly)
        {
            // The existing collector deliberately excludes this known class.
            source.Support = B::SupportState::ExplicitlyUnsupported;
            return;
        }
        if (!source.BlueprintIdentity || source.BlueprintIdentity->empty() ||
            !MiniMapBuildingGeometryTypes::Valid(source.Transform)) return;

        // Base Core is the existing final sprite overlay. Other spline records
        // are represented only by the complete prepared MiniMap ribbon source.
        if (!source.BaseCoreOverlay && source.HasSplineSource)
        {
            if (source.SplinePreparation == B::SplinePreparationState::Complete &&
                !source.SplineSections.empty() && ValidBounds(source.PreparedSplineSupport))
            {
                source.VisualSupport = source.PreparedSplineSupport;
                source.Support = B::SupportState::Supported;
            }
            return;
        }

        // Ordinary and Base Core representations require a valid captured
        // assembly bound/crop certificate, not merely source acquisition.
        if (source.SpriteCaptureSupportMetadata && source.SpriteQuadValid &&
            ValidBounds(source.OrdinarySpriteSupport))
        {
            source.VisualSupport = source.OrdinarySpriteSupport;
            source.Support = B::SupportState::Supported;
        }
    }

    void EraseCaptureImage(B::MassKey key) noexcept
    {
        InvalidateCanonicalCapture(key);
        std::lock_guard lock(CaptureImagesMutex);
        CaptureImages.erase(key);
    }

    void ClearCaptureImages() noexcept
    {
        Index.InvalidateSafety();
        CanonicalResults.clear();
        std::lock_guard lock(CaptureImagesMutex);
        CaptureImages.clear();
    }

    void EnqueueSource(B::MassKey key)
    {
        if (SourceQueued.insert(key).second) SourceQueue.push_back(key);
    }

    void ClearSourceQueue() noexcept
    {
        SourceQueue.clear();
        SourceQueued.clear();
    }

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

        std::stable_sort(ordinary.begin(), ordinary.end(), [](const auto& left, const auto& right) {
            const auto& a = *left->Source; const auto& b = *right->Source;
            return std::tuple(1, a.Transform.Translation[2], a.Transform.Translation[1], a.Transform.Translation[0],
                       left->Key.Serial, left->Key.Index) <
                std::tuple(1, b.Transform.Translation[2], b.Transform.Translation[1], b.Transform.Translation[0],
                       right->Key.Serial, right->Key.Index);
        });

        std::vector<O::Occluder> occluders;
        std::vector<B::SplineOccluderDependency> identities;
        occluders.reserve(ordinary.size()); identities.reserve(ordinary.size());
        for (const auto& record : ordinary)
        {
            if (!record || !record->Source) { unresolvedOrdinary = true; continue; }
            std::shared_ptr<const MiniMapBuildingCache::Sprite> image;
            if (!MiniMapBuildingComplexWorld::TryGetCaptureImage(record->Key, generation,
                record->Source->SourceRevision, image) || !image)
            { unresolvedOrdinary = true; continue; }

            C::Record collectorRecord;
            collectorRecord.Generation = generation;
            collectorRecord.Index = record->Key.Index; collectorRecord.Serial = record->Key.Serial;
            collectorRecord.TransformValid = MiniMapBuildingGeometryTypes::Valid(record->Source->Transform);
            collectorRecord.HasSpline = record->Source->HasSplineSource;
            collectorRecord.Position = record->Source->Transform.Translation;
            collectorRecord.Scale = record->Source->Transform.Scale;
            collectorRecord.Rotation = record->Source->Transform.Rotation;

            A::OrdinarySubmission submission;
            submission.RecordIndex = occluders.size();
            submission.Submitted = true;
            submission.Min = image->Bounds.Min; submission.Max = image->Bounds.Max;
            for (size_t i = 0; i < submission.Quad.size(); ++i)
                submission.Quad[i] = {record->Source->SpriteQuadWorld[i][0], record->Source->SpriteQuadWorld[i][1]};
            O::Occluder value;
            if (!A::MakeOccluder(collectorRecord, submission, uint64_t(occluders.size()), value))
            { unresolvedOrdinary = true; continue; }
            occluders.push_back(value);
            B::SplineOccluderDependency dependency;
            dependency.Key = record->Key;
            dependency.SourceRevision = record->Source->SourceRevision;
            dependency.Corners = value.Corners;
            dependency.TopZ = value.TopZ;
            identities.push_back(dependency);
        }

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
        // Retry nodes enter a separate queue, not the local work set. Failed
        // work is eligible on the next admitted service pass, never in this loop.
    }

    void InvalidateCapture(B::MassKey key) noexcept
    {
        const auto found = CaptureRequests.find(key);
        if (found != CaptureRequests.end())
        {
            MiniMapBuildingSpriteRenderer::CancelBuildingComplex(found->second.RequestId);
            CaptureRequests.erase(found);
        }
        EraseCaptureImage(key);
    }

    void CancelCaptureRequest(B::MassKey key) noexcept
    {
        const auto found = CaptureRequests.find(key);
        if (found == CaptureRequests.end()) return;
        MiniMapBuildingSpriteRenderer::CancelBuildingComplex(found->second.RequestId);
        CaptureRequests.erase(found);
    }

    void ClearCaptureRequests() noexcept
    {
        try
        {
            MiniMapBuildingSpriteRenderer::CancelPendingBuildingComplex();
            MiniMapBuildingScheduler::FeedbackQueue().RemoveAll(MiniMapSpriteQueues::RequestKind::BuildingComplex);
            CaptureRequests.clear();
            ClearCaptureImages();
            DeferredCaptureRetries.clear();
        }
        catch (...) { CaptureRequests.clear(); ClearCaptureImages(); DeferredCaptureRetries.clear(); }
    }

    void ResetIndex(uint64_t generation) noexcept
    {
        // Revoke the parallel certificate scope before cancellation/native
        // callbacks; ResetWorld advances it again before clearing index state.
        Index.InvalidateSafety();
        CanonicalResults.clear();
        MiniMapTexturePublication::InvalidateAll();
        ClearCaptureRequests();
        Index.ResetWorld(generation);
        ClearSourceQueue();
        ClearPendingOcclusion();
    }

    bool EnsureMembershipRecord(B::MassKey key, uint64_t generation)
    {
        std::shared_ptr<const B::BuildingRecord> existing;
        if (Index.TryGetRecord(key, generation, existing))
        {
            EnqueueSource(key);
            return true;
        }
        auto source = std::make_shared<B::SourcePayload>();
        source->SourceGeneration = generation;
        source->Support = B::SupportState::Unknown;
        B::BuildingRecord record;
        record.Key = key;
        record.WorldGeneration = generation;
        record.Source = std::move(source);
        B::ChangeSet changes;
        const auto result = Index.Upsert(std::move(record), changes);
        if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure) return false;
        QueueOcclusionChanges(changes);
        EnqueueSource(key);
        return true;
    }

    bool AcquireBuildingTag()
    {
        if (BuildingTag) return true;
        auto* objects = MiniMap::Services::TryObjects();
        const auto* native = MiniMapNative::Get();
        if (!objects || !objects->Ready() || !native || !native->asset.retainObject ||
            !native->asset.releaseObject) return false;
        try
        {
            const auto found = objects->FindResident(nullptr, nullptr,
                L"/Script/Chimera.CrMassBuildingTag", false);
            auto* tag = found.value ? static_cast<SDK::UScriptStruct*>(*found.value) : nullptr;
            if (!tag) return false;
            native->asset.retainObject(tag);
            BuildingTag = tag;
            BuildingTagRooted = true;
            return true;
        }
        catch (...) { return false; }
    }

    struct Query
    {
        alignas(kQueryAlignment) std::array<std::byte, kQuerySize> Storage{};
        bool Constructed = false;
        const MiniMapNative::NativeApi* Native = nullptr;
        ~Query() { if (Constructed && Native && Native->mass.queryDestruct) Native->mass.queryDestruct(Storage.data()); }
    };

    bool BuildCensus(uint64_t generation, L::SeedToken token)
    {
        const auto* native = MiniMapNative::Get();
        auto* world = MiniMapMap::GetWorld();
        if (!native || !world || !AcquireBuildingTag() ||
            !native->mass.getMassEntitySubsystem || !native->mass.queryConstruct ||
            !native->mass.queryDestruct || !native->mass.addTagRequirement ||
            !native->mass.getMatchingEntityHandles || !native->mass.isEntityValid ||
            !native->texture.memoryFree) return false;

        B::WorldOrigin origin;
        if (!MiniMapTerrainSource::TryGetWorldOrigin(origin.X, origin.Y)) return false;
        origin.Valid = true;

        auto* subsystem = native->mass.getMassEntitySubsystem(world);
        const void* manager = nullptr;
        // Match the established MiniMap Mass query contract: the subsystem's
        // manager shared-pointer storage is supplied to the typed query API.
        if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
        if (!manager) return false;

        Query query; query.Native = native;
        try
        {
            native->mass.queryConstruct(query.Storage.data(), reinterpret_cast<const uint8_t*>(subsystem) + 0x38);
            query.Constructed = true;
            native->mass.addTagRequirement(query.Storage.data(), BuildingTag, SDK::EMassFragmentPresence::All);
        }
        catch (...) { return false; }

        MiniMapNative::MassEntityHandleArray handles{};
        struct HandlesOwner
        {
            const MiniMapNative::NativeApi& Native;
            MiniMapNative::MassEntityHandleArray& Handles;
            ~HandlesOwner() { if (Handles.Data) Native.texture.memoryFree(Handles.Data); }
        } handlesOwner{*native, handles};

        MiniMapNative::MassEntityHandleArray* returned = nullptr;
        try { returned = native->mass.getMatchingEntityHandles(query.Storage.data(), &handles); }
        catch (...) { return false; }
        if (returned != &handles || handles.Num < 0 || handles.Max < handles.Num ||
            (handles.Num && !handles.Data)) return false;

        const auto lifecycle = L::Status();
        if (!L::InAdmittedService() || lifecycle.WorldGeneration != generation ||
            lifecycle.Epoch != token.Epoch || lifecycle.LossRevision != token.LossRevision ||
            MiniMapMap::GetWorldGeneration() != generation || MiniMapMap::GetWorld() != world) return false;

        if (!Index.BeginWorld(generation, {origin.X, origin.Y, true})) return false;
        ClearSourceQueue();
        Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;

        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const Handle id = handles.Data[i];
            if (id.Index < 0 || id.SerialNumber <= 0 || !native->mass.isEntityValid(manager, id))
            { ResetIndex(generation); return false; }

            if (!EnsureMembershipRecord({id.Index, id.SerialNumber}, generation))
            { ResetIndex(generation); return false; }
        }

        // Replay only events strictly after the seed watermark. AcceptSeed is
        // the lifecycle's atomic empty-journal handoff to incremental service.
        uint64_t replaySequence = token.Watermark;
        std::array<Event, kEventBatch> events{};
        for (;;)
        {
            const size_t count = L::Drain(events);
            if (!count) break;
            for (size_t i = 0; i < count; ++i)
            {
                const Event& event = events[i];
                if (event.Epoch != token.Epoch)
                { ResetIndex(generation); return false; }
                if (event.Sequence <= token.Watermark) continue;
                if (replaySequence == (std::numeric_limits<uint64_t>::max)() || event.Sequence != replaySequence + 1)
                { ResetIndex(generation); return false; }
                replaySequence = event.Sequence;

                B::ChangeSet changes;
                if (event.Kind == L::EventKind::Add)
                {
                    // The observer event establishes BuildingTag membership;
                    // recheck only liveness so a stale Add cannot resurrect a
                    // removed handle. Visual fragments may still be absent.
                    auto* currentSubsystem = native->mass.getMassEntitySubsystem(world);
                    const void* currentManager = nullptr;
                    if (currentSubsystem) std::memcpy(&currentManager,
                        reinterpret_cast<const uint8_t*>(currentSubsystem) + 0x38, sizeof(currentManager));
                    if (currentManager != manager || !native->mass.isEntityValid(manager, event.Entity))
                        continue;
                    if (!EnsureMembershipRecord({event.Entity.Index, event.Entity.SerialNumber}, generation))
                    { ResetIndex(generation); return false; }
                }
                else
                {
                    const auto result = Index.Remove({event.Entity.Index, event.Entity.SerialNumber}, generation, changes);
                    if (result == B::RemoveResult::AllocationFailure)
                    { ResetIndex(generation); return false; }
                    QueueOcclusionChanges(changes);
                    const B::MassKey key{event.Entity.Index, event.Entity.SerialNumber};
                    SourceQueued.erase(key);
                    InvalidateCapture(key);
                }
            }
        }

        // The existing lifecycle API takes the current loading predicate. The
        // accepted admission contract here is its world/experience and observed
        // restore barriers; no private save state or extra readiness probe.
        if (!L::AcceptSeed(token, false) || MiniMapMap::GetWorldGeneration() != generation ||
            MiniMapMap::GetWorld() != world)
        { ResetIndex(generation); return false; }
        AppliedEpoch = token.Epoch;
        AppliedSequence = replaySequence;
        Current.RestoreEpoch = AppliedEpoch;
        Current.AppliedSequence = AppliedSequence;
        Current.State = MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished;
        return true;
    }

    bool ApplyIncremental(const MiniMapNative::NativeApi& native, const Event& event, uint64_t generation)
    {
        if (AppliedSequence == (std::numeric_limits<uint64_t>::max)() ||
            event.Epoch != AppliedEpoch || event.Sequence != AppliedSequence + 1 ||
            event.Entity.Index < 0 || event.Entity.SerialNumber <= 0) return false;
        B::ChangeSet changes;
        if (event.Kind == L::EventKind::Remove)
        {
            const B::MassKey key{event.Entity.Index, event.Entity.SerialNumber};
            if (Index.Remove(key, generation, changes) == B::RemoveResult::AllocationFailure)
                return false;
            QueueOcclusionChanges(changes);
            SourceQueued.erase(key);
            InvalidateCapture(key);
        }
        else
        {
            auto* world = MiniMapMap::GetWorld();
            auto* subsystem = native.mass.getMassEntitySubsystem ? native.mass.getMassEntitySubsystem(world) : nullptr;
            const void* manager = nullptr;
            if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
            if (!manager || !native.mass.isEntityValid || !native.mass.isEntityValid(manager, event.Entity))
            {
                // A candidate Add that is no longer live invalidates the
                // incremental handoff; the next census reconciles membership.
                return false;
            }
            if (!EnsureMembershipRecord({event.Entity.Index, event.Entity.SerialNumber}, generation)) return false;
        }
        AppliedSequence = event.Sequence;
        Current.AppliedSequence = AppliedSequence;
        return true;
    }

    bool SameCanonical(const B::SourcePayload& source, const std::string& canonical) noexcept
    {
        return source.BlueprintIdentity && !canonical.empty() && *source.BlueprintIdentity == canonical;
    }

    void AdmitCaptureIfEligible(B::MassKey key, uint64_t generation,
        const std::shared_ptr<const B::BuildingRecord>& record)
    {
        if (!record || !record->Source || record->WorldGeneration != generation ||
            record->Source->SourceGeneration != generation || !record->Source->SourceRevision ||
            !record->Source->BlueprintIdentity || record->Source->BlueprintIdentity->empty() ||
            !record->Source->InteriorKnown || record->Source->InteriorOnly) return;
        {
            std::lock_guard lock(CaptureImagesMutex);
            auto completedImage = CaptureImages.find(key);
            if (completedImage != CaptureImages.end() && completedImage->second.WorldGeneration == generation &&
                completedImage->second.SourceRevision == record->Source->SourceRevision &&
                completedImage->second.Image && completedImage->second.CanonicalBlueprintIdentity == *record->Source->BlueprintIdentity)
                return;
        }
        auto prior = CaptureRequests.find(key);
        if (prior != CaptureRequests.end())
        {
            if (prior->second.SourceRevision == record->Source->SourceRevision &&
                prior->second.CanonicalBlueprintIdentity == *record->Source->BlueprintIdentity &&
                (prior->second.Terminal || (prior->second.Snapshot &&
                    MiniMapBuildingComplexSourceAcquisition::IsSameSource(*prior->second.Snapshot, *record->Source)))) return;
            InvalidateCapture(key);
        }
        if (!NextCaptureRequestId) return; // Correlation IDs never wrap or alias.
        const uint64_t requestId = NextCaptureRequestId;
        NextCaptureRequestId = requestId == (std::numeric_limits<uint64_t>::max)() ? 0 : requestId + 1;
        MiniMapSpriteQueues::Request request;
        request.Kind = MiniMapSpriteQueues::RequestKind::BuildingComplex;
        request.RequestId = requestId;
        request.MassIndex = key.Index;
        request.MassSerial = key.Serial;
        request.WorldGeneration = generation;
        request.SourceRevision = record->Source->SourceRevision;
        request.CanonicalBlueprintIdentity = *record->Source->BlueprintIdentity;
        request.SourceSnapshot = record->Source;
        auto [track, inserted] = CaptureRequests.emplace(key,
            CaptureTrack{requestId, record->Source->SourceRevision, request.CanonicalBlueprintIdentity,
                record->Source, false});
        if (!inserted) return;
        if (!MiniMapBuildingSpriteRenderer::SubmitBuildingComplex(std::move(request)))
        {
            CaptureRequests.erase(track);
            // The source remains in the established acquisition retry path.
            EnqueueSource(key);
            return;
        }
    }

    void ConsumeCaptureResults(uint64_t generation)
    {
        auto& feedbackQueue = MiniMapBuildingScheduler::FeedbackQueue();
        MiniMapSpriteQueues::Feedback result;
        while (feedbackQueue.Remove(result, MiniMapSpriteQueues::RequestKind::BuildingComplex))
        {
            const B::MassKey key{result.MassIndex, result.MassSerial};
            auto track = CaptureRequests.find(key);
            if (track == CaptureRequests.end() || track->second.RequestId != result.RequestId) continue;
            if (track->second.Terminal) continue;
            const auto admittedSnapshot = track->second.Snapshot;
            if (!admittedSnapshot || result.WorldGeneration != generation || result.WorldGeneration != admittedSnapshot->SourceGeneration ||
                result.SourceRevision != track->second.SourceRevision ||
                result.SourceSnapshot != admittedSnapshot ||
                result.CanonicalBlueprintIdentity.empty() ||
                result.CanonicalBlueprintIdentity != track->second.CanonicalBlueprintIdentity ||
                !SameCanonical(*admittedSnapshot, result.CanonicalBlueprintIdentity))
            {
                CaptureRequests.erase(track);
                continue;
            }

            std::shared_ptr<const B::BuildingRecord> current;
            if (!Index.TryGetRecord(key, generation, current) || !current || !current->Source ||
                current->WorldGeneration != generation || current->Source->SourceGeneration != generation ||
                current->Source->SourceRevision != result.SourceRevision ||
                !MiniMapBuildingComplexSourceAcquisition::IsSameSource(*current->Source, *result.SourceSnapshot) ||
                !SameCanonical(*current->Source, result.CanonicalBlueprintIdentity))
            {
                CaptureRequests.erase(track);
                continue;
            }

            if (result.Status == MiniMapSpriteQueues::ResultStatus::RetryableFailure)
            {
                DeferredCaptureRetries.push_back({key, generation, result.SourceRevision,
                    result.CanonicalBlueprintIdentity});
                CaptureRequests.erase(track);
                continue;
            }
            if (result.Status != MiniMapSpriteQueues::ResultStatus::Success)
            {
                track->second.Terminal = true;
                track->second.Snapshot.reset();
                continue;
            }

            track->second.Terminal = true;
            track->second.Snapshot.reset();
            if (!result.Payload) continue;

            const auto& sprite = *result.Payload;
            if (!MiniMapBuildingCache::ValidSpatial(sprite.Width, sprite.Height, sprite.Bounds, sprite.Mapping) ||
                sprite.Rgba.size() != size_t(sprite.Width) * sprite.Height * 4) continue;
            const bool spriteRepresentation = current->Source->BaseCoreOverlay || !current->Source->HasSplineSource;
            std::array<std::array<double, 3>, 4> quad{};
            bool verifiedQuad = false;
            if (spriteRepresentation && sprite.AssemblyBoundsVerified)
            {
                const std::array<double, 4> sourceRect{
                    double(sprite.Mapping.X), double(sprite.Mapping.Y),
                    double(sprite.Mapping.X) + double(sprite.Width) * sprite.Mapping.Step,
                    double(sprite.Mapping.Y) + double(sprite.Height) * sprite.Mapping.Step};
                verifiedQuad = MiniMapBuildingSpriteGeometryCore::BuildWorldQuad(
                    current->Source->Transform.Translation, current->Source->Transform.Scale,
                    current->Source->Transform.Rotation, sprite.Bounds.Min, sprite.Bounds.Max,
                    sourceRect, quad);
                for (const auto& point : quad)
                    for (double value : point) verifiedQuad &= std::isfinite(value);
            }

            if (!NextCaptureImageRevision || NextCaptureImageRevision == (std::numeric_limits<uint64_t>::max)()) continue;
            const uint64_t imageRevision = NextCaptureImageRevision++;
            try
            {
                InvalidateCanonicalCapture(key);
                MiniMapTexturePublication::InvalidateCapture(key);
            }
            catch (...) { continue; }
            if (spriteRepresentation)
            {
                B::SourcePayload updated = *current->Source;
                updated.SpriteQuadWorld = {};
                updated.SpriteQuadValid = false;
                updated.SpriteCaptureSupportMetadata = 0;
                updated.OrdinarySpriteSupport = {};
                updated.Support = B::SupportState::Unknown;
                updated.VisualSupport = {};
                if (verifiedQuad)
                {
                    updated.SpriteQuadWorld = quad;
                    updated.SpriteQuadValid = true;
                    updated.SpriteCaptureSupportMetadata = imageRevision;
                    updated.OrdinarySpriteSupport.Valid = true;
                    updated.OrdinarySpriteSupport.MinX = updated.OrdinarySpriteSupport.MaxX = quad[0][0];
                    updated.OrdinarySpriteSupport.MinY = updated.OrdinarySpriteSupport.MaxY = quad[0][1];
                    for (const auto& point : quad)
                    {
                        updated.OrdinarySpriteSupport.MinX = (std::min)(updated.OrdinarySpriteSupport.MinX, point[0]);
                        updated.OrdinarySpriteSupport.MaxX = (std::max)(updated.OrdinarySpriteSupport.MaxX, point[0]);
                        updated.OrdinarySpriteSupport.MinY = (std::min)(updated.OrdinarySpriteSupport.MinY, point[1]);
                        updated.OrdinarySpriteSupport.MaxY = (std::max)(updated.OrdinarySpriteSupport.MaxY, point[1]);
                    }
                }
                ClassifyVisualSupport(updated);
                B::BuildingRecord published;
                published.Key = key;
                published.WorldGeneration = generation;
                published.Source = std::make_shared<const B::SourcePayload>(std::move(updated));
                B::ChangeSet changes;
                const auto upserted = Index.Upsert(std::move(published), changes);
                if (upserted == B::UpsertResult::Inserted || upserted == B::UpsertResult::Replaced)
                    QueueOcclusionChanges(changes);
                if (upserted != B::UpsertResult::Inserted && upserted != B::UpsertResult::Replaced &&
                    upserted != B::UpsertResult::Unchanged) continue;
            }
            // The index has now revoked old and new graphical/dependent cells.
            // Publish capture availability only after that certificate change.
            try
            {
                std::lock_guard lock(CaptureImagesMutex);
                CaptureImages.insert_or_assign(key, CompletedCaptureImage{
                    generation, result.SourceRevision, imageRevision,
                    result.CanonicalBlueprintIdentity, result.Payload});
            }
            catch (...) { continue; }
        }
    }

    void ServiceOneSource(const MiniMapNative::NativeApi& native, uint64_t generation, B::MassKey key)
    {
        auto retry = [&]() { EnqueueSource(key); };
        if (!MiniMapBuildingComplexSourceAcquisition::Initialize()) { retry(); return; }
        std::shared_ptr<const B::BuildingRecord> before;
        if (!Index.TryGetRecord(key, generation, before) || !before || !before->Source) return;

        auto* world = MiniMapMap::GetWorld();
        if (!world || MiniMapMap::GetWorldGeneration() != generation ||
            !native.mass.getMassEntitySubsystem) { retry(); return; }
        auto* subsystem = native.mass.getMassEntitySubsystem(world);
        const void* manager = nullptr;
        if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
        if (!manager) { retry(); return; }

        B::SourcePayload candidate;
        bool complete = false;
        const Handle entity{key.Index, key.Serial};
        if (!MiniMapBuildingComplexSourceAcquisition::TryAcquire(native, manager, entity,
            generation, candidate, complete)) { retry(); return; }

        auto* currentSubsystem = native.mass.getMassEntitySubsystem(world);
        const void* currentManager = nullptr;
        if (currentSubsystem) std::memcpy(&currentManager,
            reinterpret_cast<const uint8_t*>(currentSubsystem) + 0x38, sizeof(currentManager));
        if (MiniMapMap::GetWorldGeneration() != generation || MiniMapMap::GetWorld() != world ||
            currentManager != manager || !native.mass.isEntityValid(manager, entity) ||
            !native.mass.isEntityBuilt(manager, entity))
        { retry(); return; }
        std::shared_ptr<const B::BuildingRecord> current;
        if (!Index.TryGetRecord(key, generation, current) || current != before) { retry(); return; }

        if (!MiniMapBuildingComplexSourceAcquisition::IsSameSource(*before->Source, candidate))
        {
            if (before->Source->SourceRevision == (std::numeric_limits<uint64_t>::max)())
            { retry(); return; }
            if (!MiniMapBuildingComplexSourceAcquisition::PrepareSplineGeometry(key, candidate))
            { retry(); return; }
            candidate.SourceRevision = before->Source->SourceRevision + 1;
            ClassifyVisualSupport(candidate);
            const bool policyOnlyChange = MiniMapBuildingComplexSourceAcquisition::IsSameSourceExceptInteriorPolicy(
                *before->Source, candidate);
            if (policyOnlyChange)
            {
                CancelCaptureRequest(key);
                bool retainedVerifiedCapture = false;
                NotifyCaptureValidityChange(key);
                {
                    std::lock_guard lock(CaptureImagesMutex);
                    auto image = CaptureImages.find(key);
                    if (image != CaptureImages.end() && image->second.WorldGeneration == generation &&
                        image->second.SourceRevision == before->Source->SourceRevision && image->second.Image &&
                        image->second.CanonicalBlueprintIdentity == *candidate.BlueprintIdentity &&
                        image->second.Image->AssemblyBoundsVerified &&
                        MiniMapBuildingCache::ValidSpatial(image->second.Image->Width, image->second.Image->Height,
                            image->second.Image->Bounds, image->second.Image->Mapping) &&
                        image->second.Image->Rgba.size() == size_t(image->second.Image->Width) *
                            image->second.Image->Height * 4)
                    {
                        image->second.SourceRevision = candidate.SourceRevision;
                        candidate.SpriteQuadWorld = before->Source->SpriteQuadWorld;
                        candidate.SpriteQuadValid = before->Source->SpriteQuadValid;
                        candidate.SpriteCaptureSupportMetadata = image->second.ImageRevision;
                        candidate.OrdinarySpriteSupport = before->Source->OrdinarySpriteSupport;
                        retainedVerifiedCapture = true;
                    }
                    else
                        CaptureImages.erase(key);
                }
                if (!retainedVerifiedCapture)
                {
                    candidate.SpriteQuadWorld = {};
                    candidate.SpriteQuadValid = false;
                    candidate.SpriteCaptureSupportMetadata = 0;
                    candidate.OrdinarySpriteSupport = {};
                }
                ClassifyVisualSupport(candidate);
            }
            else InvalidateCapture(key);
            B::BuildingRecord updated;
            updated.Key = key;
            updated.WorldGeneration = generation;
            updated.Source = std::make_shared<const B::SourcePayload>(std::move(candidate));
            B::ChangeSet changes;
            const auto result = Index.Upsert(std::move(updated), changes);
            if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure)
            { retry(); return; }
            QueueOcclusionChanges(changes);
        }
        if (complete)
        {
            std::shared_ptr<const B::BuildingRecord> admittedSource;
            if (Index.TryGetRecord(key, generation, admittedSource))
                AdmitCaptureIfEligible(key, generation, admittedSource);
        }
        if (!complete) retry();
    }

    void ServicePendingSources(const MiniMapNative::NativeApi& native, uint64_t generation)
    {
        // Source values are currently reacquired after membership activity or
        // reseeding; ordinary transform or spline edits are not tracked here.
        const size_t queuedAtStart = SourceQueue.size();
        std::vector<B::MassKey> pass;
        pass.reserve(queuedAtStart);
        for (size_t i = 0; i < queuedAtStart && !SourceQueue.empty(); ++i)
        {
            const B::MassKey key = SourceQueue.front();
            SourceQueue.pop_front();
            if (SourceQueued.erase(key) == 0) continue;
            std::shared_ptr<const B::BuildingRecord> current;
            const bool deferredRetry = Index.TryGetRecord(key, generation, current) && current && current->Source &&
                std::any_of(DeferredCaptureRetries.begin(), DeferredCaptureRetries.end(),
                    [&](const DeferredCaptureRetry& retry) {
                        return retry.Key == key && retry.WorldGeneration == generation &&
                            retry.SourceRevision == current->Source->SourceRevision &&
                            current->Source->BlueprintIdentity &&
                            retry.CanonicalBlueprintIdentity == *current->Source->BlueprintIdentity;
                    });
            if (!deferredRetry) pass.push_back(key);
        }
        for (const B::MassKey key : pass)
            ServiceOneSource(native, generation, key);
    }

    void ReleaseDeferredCaptureRetries(uint64_t generation)
    {
        for (const auto& retry : DeferredCaptureRetries)
        {
            if (retry.WorldGeneration != generation) continue;
            std::shared_ptr<const B::BuildingRecord> current;
            if (!Index.TryGetRecord(retry.Key, generation, current) || !current || !current->Source ||
                current->Source->SourceRevision != retry.SourceRevision ||
                !current->Source->BlueprintIdentity ||
                *current->Source->BlueprintIdentity != retry.CanonicalBlueprintIdentity) continue;
            const auto outstanding = CaptureRequests.find(retry.Key);
            if (outstanding != CaptureRequests.end() &&
                outstanding->second.SourceRevision == retry.SourceRevision &&
                outstanding->second.CanonicalBlueprintIdentity == retry.CanonicalBlueprintIdentity) continue;
            EnqueueSource(retry.Key);
        }
        DeferredCaptureRetries.clear();
    }

    void ServiceImpl()
    {
        auto lifecycle = L::Status();
        const uint64_t generation = Current.WorldGeneration;
        if (!generation || !MiniMapMap::GameplayReady() || MiniMapMap::GetWorldGeneration() != generation ||
            lifecycle.WorldGeneration != generation || !L::InAdmittedService()) return;
        if (Index.ObserveRenderingPolicy(MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails))
        { CanonicalResults.clear(); MiniMapTexturePublication::InvalidateAll(); }

        if (lifecycle.ObservationLost || lifecycle.SeedRequired ||
            Current.State != MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished ||
            lifecycle.Epoch != AppliedEpoch)
        {
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            ResetIndex(generation);
            const auto token = L::BeginSeed(false);
            if (!token.Epoch) return;
            Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;
            if (!BuildCensus(generation, token))
            {
                if (MiniMapMap::GetWorldGeneration() == generation) ResetIndex(generation);
                Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            }
            return;
        }

        const auto* native = MiniMapNative::Get();
        if (!native)
        {
            ResetIndex(generation);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            return;
        }
        std::array<Event, kEventBatch> events{};
        for (;;)
        {
            const size_t count = L::Drain(events);
            if (!count) break;
            for (size_t i = 0; i < count; ++i)
                if (!ApplyIncremental(*native, events[i], generation))
                {
                    ResetIndex(generation);
                    Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
                    return;
                }
        }
        ConsumeCaptureResults(generation);
        if (!SourceQueue.empty()) ServicePendingSources(*native, generation);
        ReleaseDeferredCaptureRetries(generation);
        ServicePendingOcclusion(generation);
        lifecycle = L::Status();
        if (lifecycle.ObservationLost || lifecycle.Epoch != AppliedEpoch ||
            (lifecycle.Sequence != AppliedSequence && lifecycle.Queued == 0))
        {
            ResetIndex(generation);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
        }
    }

    void Service() noexcept
    {
        try
        {
            ServiceImpl();
            MiniMapTexturePublication::ProcessRequests();
        }
        catch (...)
        {
            if (Current.WorldGeneration) ResetIndex(Current.WorldGeneration);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
        }
    }
}

namespace MiniMapBuildingComplexWorld
{
    void BeginWorld(uint64_t generation) noexcept
    {
        Current = {};
        Current.WorldGeneration = generation;
        Current.State = generation ? MembershipState::InitialCensusPending : MembershipState::WorldUnavailable;
        AppliedEpoch = AppliedSequence = 0;
        ResetIndex(generation);
        if (!generation || !L::SetConsumer(&Service))
            Current.State = MembershipState::WorldUnavailable;
        else
        {
            const auto lifecycle = MiniMapMassLifecycle::Status();
            if (!lifecycle.Available || lifecycle.Cancelled || !lifecycle.Consumer)
                Current.State = MembershipState::WorldUnavailable;
        }
    }

    void EndWorld(uint64_t nextGeneration) noexcept
    {
        ResetIndex(nextGeneration);
        Current = {};
        Current.WorldGeneration = nextGeneration;
        Current.State = nextGeneration ? MembershipState::InitialCensusPending : MembershipState::WorldUnavailable;
        AppliedEpoch = AppliedSequence = 0;
    }

    void Shutdown() noexcept
    {
        EndWorld();
        MiniMapBuildingComplexSourceAcquisition::Shutdown();
        if (BuildingTag && BuildingTagRooted)
        {
            if (const auto* native = MiniMapNative::Get(); native && native->asset.releaseObject)
                native->asset.releaseObject(BuildingTag);
        }
        BuildingTag = nullptr;
        BuildingTagRooted = false;
    }

    Status GetStatus() noexcept
    {
        Status result = Current;
        if (result.State == MembershipState::MembershipEstablished)
        {
            const auto lifecycle = L::Status();
            if (lifecycle.WorldGeneration != result.WorldGeneration || lifecycle.ObservationLost ||
                lifecycle.SeedRequired || lifecycle.Epoch != AppliedEpoch ||
                lifecycle.Sequence != AppliedSequence || lifecycle.Queued || lifecycle.InFlight)
                result.State = MembershipState::EventReplayPending;
        }
        return result;
    }
    MiniMapBuildingComplexIndex::Index& GetIndex() noexcept { return Index; }

    bool TryGetCaptureImage(B::MassKey key, uint64_t generation, uint64_t sourceRevision,
        std::shared_ptr<const MiniMapBuildingCache::Sprite>& output, uint64_t* imageRevision) noexcept
    {
        output.reset();
        if (imageRevision) *imageRevision = 0;
        try
        {
            if (!generation || !sourceRevision) return false;
            std::shared_ptr<const B::BuildingRecord> record;
            if (!Index.TryGetRecord(key, generation, record) || !record || !record->Source ||
                record->Source->SourceRevision != sourceRevision ||
                record->Source->SourceGeneration != generation || !record->Source->BlueprintIdentity) return false;
            std::lock_guard lock(CaptureImagesMutex);
            const auto image = CaptureImages.find(key);
            if (image == CaptureImages.end() || image->second.WorldGeneration != generation ||
                image->second.SourceRevision != sourceRevision || !image->second.Image ||
                *record->Source->BlueprintIdentity != image->second.CanonicalBlueprintIdentity) return false;
            output = image->second.Image;
            if (imageRevision) *imageRevision = image->second.ImageRevision;
            return true;
        }
        catch (...) { output.reset(); return false; }
    }

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
            if (!generation || initialStatus.State != MembershipState::MembershipEstablished ||
                initialStatus.WorldGeneration != generation) return CellCompositionResult::Failed;

            if (Index.ObserveRenderingPolicy(MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails))
            { CanonicalResults.clear(); MiniMapTexturePublication::InvalidateAll(); }
            B::CellSnapshot snapshot;
            if (!Index.TrySnapshot(key, snapshot) || snapshot.WorldGeneration != generation || !snapshot.Origin.Valid)
                return CellCompositionResult::Failed;
            const bool renderDroneRails = MiniMapConfig::GetRenderSettings().Buildings.RenderDroneRails;
            auto identity = IdentifyCanonicalRequest(snapshot, initialStatus.RestoreEpoch, renderDroneRails);
            // A changed unrelated journal counter does not evict certified cells.
            // The demanded key is rechecked against its current local token below.
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
            sprites.reserve(snapshot.Records.size());
            for (const auto& record : snapshot.Records)
            {
                if (!record || !record->Source || !record->Source->InteriorKnown || record->Source->InteriorOnly ||
                    record->Source->HasSplineSource) continue;
                std::shared_ptr<const MiniMapBuildingCache::Sprite> image;
                uint64_t imageRevision = 0;
                if (TryGetCaptureImage(record->Key, generation, record->Source->SourceRevision, image, &imageRevision) && image)
                    sprites.push_back({record->Key, record->Source->SourceRevision, imageRevision, std::move(image)});
            }

            R::CellImage composed;
            const auto state = R::Compose(snapshot, sprites, renderDroneRails, composed);
            if (state == R::State::Failed) return CellCompositionResult::Failed;
            composed.RestoreEpoch = initialStatus.RestoreEpoch;
            composed.CaptureAvailability = work->Identity.Captures;

            if (!ValidateCellImage(composed)) return CellCompositionResult::Stale;

            // Also recheck absences/readiness, which the rasterizer's successful
            // capture manifest alone cannot certify for a Partial result.
            B::CellSnapshot current;
            if (!Index.TrySnapshot(key, current) || !current.Origin.Valid ||
                IdentifyCanonicalRequest(current, GetStatus().RestoreEpoch,
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
            const auto lifecycle = L::Status();
            if (!identity.WorldGeneration || status.State != MembershipState::MembershipEstablished ||
                lifecycle.SeedRequired || lifecycle.ObservationLost || lifecycle.Queued || lifecycle.InFlight ||
                (lifecycle.RestoreObserved && (!lifecycle.MassInitialized || !lifecycle.PostSaveCompleted)) ||
                status.WorldGeneration != identity.WorldGeneration || status.RestoreEpoch != identity.RestoreEpoch ||
                renderDroneRails != identity.RenderDroneRails) return false;
            B::CellSnapshot current;
            if (!Index.TrySnapshot(identity.Key, current) || current.WorldGeneration != identity.WorldGeneration ||
                !current.Origin.Valid || !identity.Origin.Valid || current.Origin.X != identity.Origin.X ||
                current.Origin.Y != identity.Origin.Y || current.Validity != identity.Validity) return false;
            const bool locallyCertified = current.Validity.SafetyGeneration != 0 && current.Validity.LocalEpoch != 0;
            if (!locallyCertified && (current.CellRevision != identity.CellRevision ||
                current.SourceRevision != identity.SourceRevision || current.DependencyRevision != identity.DependencyRevision)) return false;
            auto expected = IdentifyCanonicalRequest(current, status.RestoreEpoch, renderDroneRails);
            if (expected.Captures != identity.CaptureAvailability) return false;
            std::vector<MiniMapBuildingComplexCellRasterizer::CaptureRevision> available;
            for (const auto& capture : expected.Captures)
                if (capture.ImageRevision) available.push_back(capture);
            std::vector<bool> incorporated(available.size(), false);
            for (const auto& captured : identity.CaptureRevisions)
            {
                size_t match = 0;
                while (match < available.size() && (incorporated[match] || available[match] != captured)) ++match;
                if (match == available.size() || !captured.ImageRevision) return false;
                incorporated[match] = true;
            }
            if (identity.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
                (current.HasUnresolvedContributor || available.size() != expected.Captures.size() ||
                    identity.CaptureRevisions.size() != available.size())) return false;
            for (const auto& capture : available)
            {
                std::shared_ptr<const MiniMapBuildingCache::Sprite> source;
                uint64_t imageRevision = 0;
                if (!TryGetCaptureImage(capture.Building, identity.WorldGeneration,
                    capture.SourceRevision, source, &imageRevision) || !source ||
                    imageRevision != capture.ImageRevision)
                    return false;
            }
            return true;
        }
        catch (...) { return false; }
    }
}
#endif
