#include "BuildingComplexIndex.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace MiniMapBuildingComplexIndex
{
    namespace
    {
        bool ValidBounds(const WorldBounds& b) noexcept
        {
            return b.Valid && std::isfinite(b.MinX) && std::isfinite(b.MinY) &&
                std::isfinite(b.MaxX) && std::isfinite(b.MaxY) &&
                b.MinX <= b.MaxX && b.MinY <= b.MaxY;
        }

        bool ValidSupportCertification(const SourcePayload& source) noexcept
        {
            if (source.Support == SupportState::Unknown) return true;
            if (!source.InteriorKnown) return false;
            if (source.InteriorOnly)
                return source.Support == SupportState::ExplicitlyUnsupported && !source.VisualSupport.Valid;
            if (source.Support == SupportState::ExplicitlyUnsupported) return false;
            if (!source.BlueprintIdentity || source.BlueprintIdentity->empty() ||
                !MiniMapBuildingGeometryTypes::Valid(source.Transform) || !ValidBounds(source.VisualSupport)) return false;
            if (source.BaseCoreOverlay || !source.HasSplineSource)
                return source.SpriteCaptureSupportMetadata && source.SpriteQuadValid &&
                    ValidBounds(source.OrdinarySpriteSupport) && source.VisualSupport == source.OrdinarySpriteSupport;
            return source.SplinePreparation == SplinePreparationState::Complete &&
                !source.SplineSections.empty() && ValidBounds(source.PreparedSplineSupport) &&
                source.VisualSupport == source.PreparedSplineSupport;
        }

        bool AxisRange(double minimum, double maximum, double origin,
            int64_t& first, int64_t& last) noexcept
        {
            const long double lo = (static_cast<long double>(minimum) - origin) / kCellWorldSizeCm;
            const long double hi = (static_cast<long double>(maximum) - origin) / kCellWorldSizeCm;
            if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) return false;
            const long double floored = std::floor(lo);
            // A non-empty bound is treated as [min,max), so an exact maximum
            // grid edge belongs only to the cell on its left. Degenerate
            // point/line support uses floor and remains assigned to one cell.
            const long double ceiled = std::ceil(hi);
            const long double final = (minimum == maximum) ? floored : ceiled - 1.0L;
            constexpr long double low = static_cast<long double>((std::numeric_limits<int64_t>::min)());
            constexpr long double highExclusive = -low;
            if (floored < low || floored >= highExclusive || final < low || final >= highExclusive) return false;
            first = static_cast<int64_t>(floored);
            last = static_cast<int64_t>(final);
            return first <= last;
        }

        bool CellsFor(const WorldBounds& bounds, WorldOrigin origin, std::set<CellKey>& out)
        {
            if (!ValidBounds(bounds) || !origin.Valid || !std::isfinite(origin.X) || !std::isfinite(origin.Y)) return false;
            int64_t x0, x1, y0, y1;
            if (!AxisRange(bounds.MinX, bounds.MaxX, origin.X, x0, x1) ||
                !AxisRange(bounds.MinY, bounds.MaxY, origin.Y, y0, y1)) return false;
            for (int64_t x = x0;;)
            {
                for (int64_t y = y0;;)
                {
                    out.insert({x, y});
                    if (y == y1) break;
                    ++y;
                }
                if (x == x1) break;
                ++x;
            }
            return true;
        }

        bool DependencyCellsFor(const WorldBounds& bounds, WorldOrigin origin, std::set<CellKey>& out)
        {
            if (!ValidBounds(bounds)) return false;
            WorldBounds expanded = bounds;
            expanded.MinX = std::nextafter(expanded.MinX, -std::numeric_limits<double>::infinity());
            expanded.MinY = std::nextafter(expanded.MinY, -std::numeric_limits<double>::infinity());
            expanded.MaxX = std::nextafter(expanded.MaxX, std::numeric_limits<double>::infinity());
            expanded.MaxY = std::nextafter(expanded.MaxY, std::numeric_limits<double>::infinity());
            if (!ValidBounds(expanded)) return false;
            return CellsFor(expanded, origin, out);
        }

        bool ValidKey(MassKey key) noexcept { return key.Index >= 0 && key.Serial > 0; }
        bool UnresolvedContributor(const BuildingRecord& record) noexcept
        { return record.Source->Support == SupportState::Unknown && !(record.Source->InteriorKnown && record.Source->InteriorOnly); }
        bool Unbounded(const BuildingRecord& record) noexcept
        { return UnresolvedContributor(record) && !record.Source->VisualSupport.Valid; }
        bool PotentialOrdinaryOccluder(const SourcePayload& source) noexcept
        { return !source.HasSplineSource && !source.BaseCoreOverlay && !(source.InteriorKnown && source.InteriorOnly); }
        bool VerifiedOccluderGeometry(const SourcePayload& source) noexcept
        {
            if (!source.InteriorKnown || !source.SpriteCaptureSupportMetadata || !source.SpriteQuadValid ||
                !ValidBounds(source.OrdinarySpriteSupport) ||
                !MiniMapBuildingGeometryTypes::Valid(source.Transform)) return false;
            for (const auto& point : source.SpriteQuadWorld)
                for (double value : point) if (!std::isfinite(value)) return false;
            return true;
        }
        bool Intersects(const WorldBounds& a, const WorldBounds& b) noexcept
        { return ValidBounds(a) && ValidBounds(b) && a.MinX <= b.MaxX && b.MinX <= a.MaxX && a.MinY <= b.MaxY && b.MinY <= a.MaxY; }
        void Advance(uint64_t& revision) noexcept
        { if (revision != (std::numeric_limits<uint64_t>::max)()) ++revision; }
    }

    bool TryWorldCell(double worldX, double worldY, WorldOrigin origin, CellKey& output) noexcept
    {
        if (!origin.Valid || !std::isfinite(origin.X) || !std::isfinite(origin.Y) ||
            !std::isfinite(worldX) || !std::isfinite(worldY)) return false;
        const long double x = std::floor((static_cast<long double>(worldX) - origin.X) / kCellWorldSizeCm);
        const long double y = std::floor((static_cast<long double>(worldY) - origin.Y) / kCellWorldSizeCm);
        constexpr long double low = static_cast<long double>((std::numeric_limits<int64_t>::min)());
        constexpr long double highExclusive = -low;
        if (!std::isfinite(x) || !std::isfinite(y) || x < low || x >= highExclusive || y < low || y >= highExclusive) return false;
        output = {static_cast<int64_t>(x), static_cast<int64_t>(y)};
        return true;
    }

    void Index::AdvanceSafetyLocked() const noexcept
    {
        // Never wrap a certificate generation. Exhaustion permanently disables
        // this foundation; the legacy conservative paths remain operational.
        if (safetyGeneration_ == (std::numeric_limits<uint64_t>::max)())
            localValidityAvailable_ = false;
        else ++safetyGeneration_;
        localEpochs_.clear();
        localEpochSequence_ = 0;
    }

    CellValidity Index::SnapshotValidityLocked(CellKey key) const noexcept
    {
        if (!localValidityAvailable_ || !worldGeneration_ || !origin_.Valid) return {};
        try
        {
            auto found = localEpochs_.find(key);
            if (found == localEpochs_.end())
            {
                if (localEpochSequence_ == (std::numeric_limits<uint64_t>::max)())
                    AdvanceSafetyLocked();
                if (!localValidityAvailable_) return {};
                found = localEpochs_.emplace(key, ++localEpochSequence_).first;
            }
            return {safetyGeneration_, found->second};
        }
        catch (...)
        {
            // A bookkeeping failure revokes old local certificates, but must
            // not change legacy snapshot success/completeness or source state.
            AdvanceSafetyLocked();
            return {};
        }
    }

    void Index::PrepareLocalChangeLocked(ChangeSet& changes, const std::set<CellKey>& oldCells,
        const std::set<CellKey>& newCells, const std::vector<SectionKey>& affectedSections,
        const std::map<SectionKey, std::set<CellKey>>* newSections) const noexcept
    {
        try
        {
            changes.OldGraphicalCells.assign(oldCells.begin(), oldCells.end());
            changes.NewGraphicalCells.assign(newCells.begin(), newCells.end());
            std::set<CellKey> dependencies;
            for (const auto& section : affectedSections)
            {
                // Spatial query coverage includes newly entering candidates;
                // reverse edges alone cover only previously observed ones.
                const auto existing = sectionCells_.find(section);
                if (existing != sectionCells_.end())
                    dependencies.insert(existing->second.begin(), existing->second.end());
                if (newSections)
                {
                    const auto next = newSections->find(section);
                    if (next != newSections->end())
                        dependencies.insert(next->second.begin(), next->second.end());
                }
                // Current payload readiness is aggregate across the spline,
                // so invalidating one section can alter every owner cell.
                const auto owner = records_.find(section.Building);
                if (owner != records_.end())
                    dependencies.insert(owner->second.Cells.begin(), owner->second.Cells.end());
            }
            changes.DependencyOutputCells.assign(dependencies.begin(), dependencies.end());
            dependencies.insert(oldCells.begin(), oldCells.end());
            dependencies.insert(newCells.begin(), newCells.end());
            changes.LocalInvalidatedCells.assign(dependencies.begin(), dependencies.end());
        }
        catch (...)
        {
            changes.OldGraphicalCells.clear(); changes.NewGraphicalCells.clear();
            changes.DependencyOutputCells.clear(); changes.LocalInvalidatedCells.clear();
            changes.LocalSafetyInvalidated = true;
        }
    }

    void Index::ApplyLocalChangeLocked(ChangeSet& changes) noexcept
    {
        if (changes.LocalSafetyInvalidated || changes.GlobalCompletenessInvalidated ||
            changes.GlobalSplineOcclusionInvalidated)
        {
            changes.LocalSafetyInvalidated = true;
            AdvanceSafetyLocked();
        }
        else if (localValidityAvailable_)
        {
            for (const auto key : changes.LocalInvalidatedCells)
            {
                auto found = localEpochs_.find(key);
                // No token has ever escaped for an untracked cell in this
                // generation; its first future snapshot receives a fresh epoch.
                if (found == localEpochs_.end()) continue;
                if (localEpochSequence_ == (std::numeric_limits<uint64_t>::max)())
                {
                    changes.LocalSafetyInvalidated = true;
                    AdvanceSafetyLocked();
                    break;
                }
                found->second = ++localEpochSequence_;
            }
        }
        changes.SafetyGeneration = localValidityAvailable_ ? safetyGeneration_ : 0;
    }

    void Index::InvalidateSafety() noexcept
    {
        std::lock_guard lock(mutex_);
        AdvanceSafetyLocked();
    }

    void Index::ObserveRenderingPolicy(bool renderDroneRails) noexcept
    {
        std::lock_guard lock(mutex_);
        if (renderingPolicyKnown_ && renderDroneRails_ != renderDroneRails)
            AdvanceSafetyLocked();
        renderDroneRails_ = renderDroneRails;
        renderingPolicyKnown_ = true;
    }

    void Index::NotifyCaptureChange(MassKey key, uint64_t generation, ChangeSet& changes) noexcept
    {
        changes = {};
        std::lock_guard lock(mutex_);
        if (generation != worldGeneration_) return;
        const auto record = records_.find(key);
        // Removal has already invalidated the old support under this mutex.
        // A capture for a nonmember cannot be acquired through the world owner.
        if (record == records_.end()) return;
        changes.WorldGeneration = generation;
        changes.SourceRevision = sourceRevision_;
        changes.DependencyRevision = dependencyRevision_;
        changes.LocalSafetyInvalidated = Unbounded(*record->second.Record) ||
            (PotentialOrdinaryOccluder(*record->second.Record->Source) &&
             !VerifiedOccluderGeometry(*record->second.Record->Source) &&
             !record->second.Record->Source->VisualSupport.Valid);
        // Pixel availability changes do not change physical occluder geometry.
        // Changed capture bounds/quad metadata subsequently use Upsert, which
        // covers old/new support and dependent spline output before publication.
        PrepareLocalChangeLocked(changes, record->second.Cells, record->second.Cells, {});
        ApplyLocalChangeLocked(changes);
    }

    bool Index::BeginWorld(uint64_t generation, WorldOrigin origin) noexcept
    {
        if (!generation || !origin.Valid || !std::isfinite(origin.X) || !std::isfinite(origin.Y)) return false;
        try
        {
            std::lock_guard lock(mutex_);
            AdvanceSafetyLocked();
            records_.clear(); cells_.clear(); unresolved_.clear(); sectionCells_.clear(); occluderSections_.clear(); sectionDependencyRevisions_.clear();
            unboundedUnresolvedCount_ = 0;
            unresolvedOccluderCount_ = 0;
            dependencyRevision_ = 0;
            worldGeneration_ = generation; origin_ = origin;
            ++resetRevision_; Advance(sourceRevision_);
            return true;
        }
        catch (...) { return false; }
    }

    void Index::ResetWorld(uint64_t nextGeneration) noexcept
    {
        std::lock_guard lock(mutex_);
        AdvanceSafetyLocked();
        records_.clear(); cells_.clear(); unresolved_.clear(); sectionCells_.clear(); occluderSections_.clear(); sectionDependencyRevisions_.clear();
        unboundedUnresolvedCount_ = 0;
        unresolvedOccluderCount_ = 0;
        dependencyRevision_ = 0;
        worldGeneration_ = nextGeneration; origin_ = {};
        ++resetRevision_; Advance(sourceRevision_);
    }

    UpsertResult Index::Upsert(BuildingRecord input, ChangeSet& changes) noexcept
    {
        changes = {};
        if (!ValidKey(input.Key) || !input.Source || !input.WorldGeneration ||
            input.Source->SourceGeneration != input.WorldGeneration) return UpsertResult::InvalidInput;
        try
        {
            // Do not trust shared_ptr<const T> alone as an immutability proof:
            // the caller may still hold a mutable alias. Copy every payload
            // value once into index ownership, including the blueprint text.
            SourcePayload payload = *input.Source;
            if (input.Source->BlueprintIdentity)
                payload.BlueprintIdentity = std::make_shared<const std::string>(*input.Source->BlueprintIdentity);
            input.Source = std::make_shared<const SourcePayload>(std::move(payload));
            // Derived occlusion output is published only through its revision-
            // checked API, never carried across a source upsert.
            input.SplineOcclusion.reset();
            std::lock_guard lock(mutex_);
            if (input.WorldGeneration != worldGeneration_ || !origin_.Valid) return UpsertResult::InvalidInput;
            if (!ValidSupportCertification(*input.Source)) return UpsertResult::InvalidInput;
            if (input.Source->Support == SupportState::Supported && !ValidBounds(input.Source->VisualSupport)) return UpsertResult::InvalidInput;
            if (input.Source->InteriorOnly && !input.Source->InteriorKnown) return UpsertResult::InvalidInput;
            if (input.Source->Support == SupportState::ExplicitlyUnsupported && input.Source->VisualSupport.Valid &&
                !ValidBounds(input.Source->VisualSupport)) return UpsertResult::InvalidInput;
            if (input.Source->OrdinarySpriteSupport.Valid && !ValidBounds(input.Source->OrdinarySpriteSupport))
                return UpsertResult::InvalidInput;
            if (input.Source->OrdinarySpriteSupport.Valid != input.Source->SpriteQuadValid)
                return UpsertResult::InvalidInput;
            if (input.Source->SpriteQuadValid && !ValidBounds(input.Source->OrdinarySpriteSupport))
                return UpsertResult::InvalidInput;
            if (input.Source->SpriteQuadValid)
            {
                WorldBounds quadBounds;
                for (const auto& point : input.Source->SpriteQuadWorld)
                {
                    for (double value : point) if (!std::isfinite(value)) return UpsertResult::InvalidInput;
                    if (!quadBounds.Valid) quadBounds = {point[0], point[1], point[0], point[1], true};
                    else
                    {
                        quadBounds.MinX = (std::min)(quadBounds.MinX, point[0]);
                        quadBounds.MinY = (std::min)(quadBounds.MinY, point[1]);
                        quadBounds.MaxX = (std::max)(quadBounds.MaxX, point[0]);
                        quadBounds.MaxY = (std::max)(quadBounds.MaxY, point[1]);
                    }
                }
                if (quadBounds != input.Source->OrdinarySpriteSupport) return UpsertResult::InvalidInput;
            }
            if (input.Source->PreparedSplineSupport.Valid && !ValidBounds(input.Source->PreparedSplineSupport))
                return UpsertResult::InvalidInput;
            if (!input.Source->SplineSections.empty() && !input.Source->PreparedSplineSupport.Valid)
                return UpsertResult::InvalidInput;
            if ((!input.Source->HasSplineSource && input.Source->SplinePreparation != SplinePreparationState::NotPresent) ||
                (input.Source->SplinePreparation == SplinePreparationState::Complete && input.Source->SplineSections.empty()) ||
                (!input.Source->SplineSections.empty() &&
                    (input.Source->SplinePreparation == SplinePreparationState::NotPresent ||
                     input.Source->SplinePreparation == SplinePreparationState::Unsupported ||
                     input.Source->SplinePreparation == SplinePreparationState::Invalid)))
                return UpsertResult::InvalidInput;
            WorldBounds computedSplineSupport;
            size_t sectionOrdinal = 0;
            for (const auto& section : input.Source->SplineSections)
            {
                if (section.Identity != sectionOrdinal++ || section.SourceSegment >= input.Source->SplineCurve.size() ||
                    (section.SyntheticThin ? section.ProfileIndex != (std::numeric_limits<uint64_t>::max)() :
                        section.ProfileIndex >= input.Source->SplineProfiles.size()) ||
                    !std::isfinite(section.StartT) || !std::isfinite(section.EndT) ||
                    section.StartT < 0 || section.EndT > 1 || section.StartT >= section.EndT ||
                    !section.Bounds.Valid || !ValidBounds(section.Bounds) || section.Triangles.empty())
                    return UpsertResult::InvalidInput;
                for (double value : section.NumericalInputs)
                    if (!std::isfinite(value)) return UpsertResult::InvalidInput;
                WorldBounds computedSection;
                for (const auto& triangle : section.Triangles)
                    for (const auto& vertex : triangle.Vertices)
                    {
                        if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1]) || !std::isfinite(vertex[2]))
                            return UpsertResult::InvalidInput;
                        if (!computedSection.Valid)
                            computedSection = {vertex[0], vertex[1], vertex[0], vertex[1], true};
                        else
                        {
                            computedSection.MinX = (std::min)(computedSection.MinX, vertex[0]);
                            computedSection.MinY = (std::min)(computedSection.MinY, vertex[1]);
                            computedSection.MaxX = (std::max)(computedSection.MaxX, vertex[0]);
                            computedSection.MaxY = (std::max)(computedSection.MaxY, vertex[1]);
                        }
                    }
                if (computedSection != section.Bounds) return UpsertResult::InvalidInput;
                if (!computedSplineSupport.Valid) computedSplineSupport = computedSection;
                else
                {
                    computedSplineSupport.MinX = (std::min)(computedSplineSupport.MinX, computedSection.MinX);
                    computedSplineSupport.MinY = (std::min)(computedSplineSupport.MinY, computedSection.MinY);
                    computedSplineSupport.MaxX = (std::max)(computedSplineSupport.MaxX, computedSection.MaxX);
                    computedSplineSupport.MaxY = (std::max)(computedSplineSupport.MaxY, computedSection.MaxY);
                }
                if (section.HasWalkwayInsetBands)
                    for (const auto& band : section.WalkwayInsetBands)
                        for (const auto& vertex : band.Vertices)
                        {
                            if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1]) ||
                                vertex[0] < computedSection.MinX || vertex[0] > computedSection.MaxX ||
                                vertex[1] < computedSection.MinY || vertex[1] > computedSection.MaxY)
                                return UpsertResult::InvalidInput;
                        }
            }
            if (computedSplineSupport != input.Source->PreparedSplineSupport)
                return UpsertResult::InvalidInput;

            auto old = records_.find(input.Key);
            const auto oldRecord = old == records_.end() ? std::shared_ptr<const BuildingRecord>{} : old->second.Record;
            if (old != records_.end() && old->second.Record->Source->SourceRevision == input.Source->SourceRevision &&
                old->second.Record->Source->SourceGeneration == input.Source->SourceGeneration &&
                old->second.Record->Source->Support == input.Source->Support &&
                old->second.Record->Source->OrdinarySpriteSupport == input.Source->OrdinarySpriteSupport &&
                old->second.Record->Source->VisualSupport == input.Source->VisualSupport &&
                old->second.Record->Source->PreparedSplineSupport == input.Source->PreparedSplineSupport &&
                old->second.Record->Source->SplinePreparation == input.Source->SplinePreparation &&
                old->second.Record->Source->SplineSections == input.Source->SplineSections &&
                old->second.Record->Source->HasSplineSource == input.Source->HasSplineSource &&
                old->second.Record->Source->SplineCategory == input.Source->SplineCategory &&
                old->second.Record->Source->SplineSortZValid == input.Source->SplineSortZValid &&
                old->second.Record->Source->SplineSortZ == input.Source->SplineSortZ &&
                old->second.Record->Source->InteriorKnown == input.Source->InteriorKnown &&
                old->second.Record->Source->InteriorOnly == input.Source->InteriorOnly &&
                old->second.Record->Source->SpriteQuadValid == input.Source->SpriteQuadValid &&
                old->second.Record->Source->SpriteCaptureSupportMetadata == input.Source->SpriteCaptureSupportMetadata &&
                old->second.Record->Source->SpriteQuadWorld == input.Source->SpriteQuadWorld)
                return UpsertResult::Unchanged;

            std::set<CellKey> nextCells;
            auto addBounds = [&](const WorldBounds& bounds) {
                return !bounds.Valid || CellsFor(bounds, origin_, nextCells);
            };
            if (input.Source->Support == SupportState::Supported)
            {
                if (!CellsFor(input.Source->VisualSupport, origin_, nextCells) ||
                    !addBounds(input.Source->OrdinarySpriteSupport) ||
                    !addBounds(input.Source->PreparedSplineSupport)) return UpsertResult::InvalidInput;
                for (const auto& section : input.Source->SplineSections)
                    if (!CellsFor(section.Bounds, origin_, nextCells)) return UpsertResult::InvalidInput;
            }
            else if (input.Source->Support == SupportState::Unknown)
            {
                // Known contributor bounds target membership/invalidation while
                // Unbounded() continues to protect global completeness.
                if (!addBounds(input.Source->VisualSupport) || !addBounds(input.Source->OrdinarySpriteSupport) ||
                    !addBounds(input.Source->PreparedSplineSupport)) return UpsertResult::InvalidInput;
                for (const auto& section : input.Source->SplineSections)
                    if (!CellsFor(section.Bounds, origin_, nextCells)) return UpsertResult::InvalidInput;
            }
            else
            {
                // Explicitly unsupported contributors still have usable
                // geometry when captured; index it for occlusion/cell
                // invalidation without promoting visual completeness.
                if ((input.Source->OrdinarySpriteSupport.Valid &&
                        !CellsFor(input.Source->OrdinarySpriteSupport, origin_, nextCells)) ||
                    (input.Source->VisualSupport.Valid &&
                        !CellsFor(input.Source->VisualSupport, origin_, nextCells)) ||
                    (input.Source->PreparedSplineSupport.Valid &&
                        !CellsFor(input.Source->PreparedSplineSupport, origin_, nextCells)))
                    return UpsertResult::InvalidInput;
                for (const auto& section : input.Source->SplineSections)
                    if (!CellsFor(section.Bounds, origin_, nextCells)) return UpsertResult::InvalidInput;
            }
            const std::set<CellKey> oldCells = old == records_.end() ? std::set<CellKey>{} : old->second.Cells;

            std::set<CellKey> oldOccluderCells, newOccluderCells;
            if (old != records_.end() && old->second.Record->Source->OrdinarySpriteSupport.Valid &&
                !CellsFor(old->second.Record->Source->OrdinarySpriteSupport, origin_, oldOccluderCells))
                return UpsertResult::InvalidInput;
            if (old != records_.end() && PotentialOrdinaryOccluder(*old->second.Record->Source) &&
                !VerifiedOccluderGeometry(*old->second.Record->Source) && old->second.Record->Source->VisualSupport.Valid &&
                !CellsFor(old->second.Record->Source->VisualSupport, origin_, oldOccluderCells))
                return UpsertResult::InvalidInput;
            if (input.Source->OrdinarySpriteSupport.Valid &&
                !CellsFor(input.Source->OrdinarySpriteSupport, origin_, newOccluderCells))
                return UpsertResult::InvalidInput;
            if (PotentialOrdinaryOccluder(*input.Source) && !VerifiedOccluderGeometry(*input.Source) &&
                input.Source->VisualSupport.Valid &&
                !CellsFor(input.Source->VisualSupport, origin_, newOccluderCells))
                return UpsertResult::InvalidInput;

            std::map<SectionKey, std::set<CellKey>> nextSectionCells;
            for (const auto& section : input.Source->SplineSections)
            {
                std::set<CellKey> sectionCells;
                if (!DependencyCellsFor(section.Bounds, origin_, sectionCells)) return UpsertResult::InvalidInput;
                nextSectionCells.emplace(SectionKey{input.Key, input.Source->SourceRevision, section.Identity},
                    std::move(sectionCells));
            }

            std::set<CellKey> touched = oldCells;
            touched.insert(nextCells.begin(), nextCells.end());
            touched.insert(oldOccluderCells.begin(), oldOccluderCells.end());
            touched.insert(newOccluderCells.begin(), newOccluderCells.end());
            std::map<CellKey, std::set<MassKey>> staged;
            for (const auto& cellKey : touched)
            {
                auto cell = cells_.find(cellKey);
                staged.emplace(cellKey, cell == cells_.end() ? std::set<MassKey>{} : cell->second.Records);
                auto& keys = staged.find(cellKey)->second;
                keys.erase(input.Key);
                if (nextCells.find(cellKey) != nextCells.end()) keys.insert(input.Key);
            }

            auto owned = std::make_shared<const BuildingRecord>(std::move(input));
            const bool newRecord = old == records_.end();
            const bool unresolved = UnresolvedContributor(*owned);
            const bool wasUnbounded = old != records_.end() && Unbounded(*old->second.Record);
            const bool isUnbounded = Unbounded(*owned);
            std::vector<CellKey> invalidated(touched.begin(), touched.end());
            std::set<SectionKey> affectedSections;
            auto touches = [&](const std::set<CellKey>& cells) {
                for (const auto& cell : cells) if (touched.find(cell) != touched.end()) return true;
                return false;
            };
            for (const auto& pair : sectionCells_)
                if (touches(pair.second)) affectedSections.insert(pair.first);
            if (old != records_.end())
                affectedSections.insert(old->second.Sections.begin(), old->second.Sections.end());
            for (const auto& pair : nextSectionCells) affectedSections.insert(pair.first);
            if (auto reverse = occluderSections_.find(owned->Key); reverse != occluderSections_.end())
                affectedSections.insert(reverse->second.begin(), reverse->second.end());
            const bool oldPotentialOccluder = old != records_.end() && PotentialOrdinaryOccluder(*old->second.Record->Source);
            const bool newPotentialOccluder = PotentialOrdinaryOccluder(*owned->Source);
            const bool oldGeometryVerified = old != records_.end() && VerifiedOccluderGeometry(*old->second.Record->Source);
            const bool newGeometryVerified = VerifiedOccluderGeometry(*owned->Source);
            const bool wasUnresolvedOccluder = oldPotentialOccluder && !oldGeometryVerified &&
                !old->second.Record->Source->VisualSupport.Valid;
            const bool isUnresolvedOccluder = newPotentialOccluder && !newGeometryVerified &&
                !owned->Source->VisualSupport.Valid;
            const bool globalOcclusionInvalidated =
                wasUnresolvedOccluder || isUnresolvedOccluder;
            if (globalOcclusionInvalidated)
                for (const auto& pair : sectionCells_) affectedSections.insert(pair.first);
            for (const auto& pair : nextSectionCells) affectedSections.insert(pair.first);
            if (affectedSections.size() > (std::numeric_limits<uint64_t>::max)() - dependencyRevision_)
                return UpsertResult::InvalidInput;
            std::vector<SectionKey> invalidatedSections(affectedSections.begin(), affectedSections.end());
            // Prepare all allocating map nodes before committing any visible
            // membership changes. merge transfers the prepared nodes.
            std::map<CellKey, Cell> newCellNodes;
            for (const auto& pair : staged)
                if (cells_.find(pair.first) == cells_.end() && !pair.second.empty())
                    newCellNodes.emplace(pair.first, Cell{});
            std::map<MassKey, Stored> newRecordNode;
            std::set<SectionKey> ownedSections;
            for (const auto& pair : nextSectionCells) ownedSections.insert(pair.first);
            if (newRecord) newRecordNode.emplace(owned->Key, Stored{owned, nextCells, ownedSections});
            std::set<MassKey> unresolvedNode;
            if (unresolved && unresolved_.find(owned->Key) == unresolved_.end()) unresolvedNode.insert(owned->Key);
            std::map<SectionKey, std::set<CellKey>> newSectionNodes;
            for (const auto& pair : nextSectionCells) newSectionNodes.emplace(pair.first, pair.second);
            std::map<SectionKey, uint64_t> newDependencyNodes;
            for (const auto& key : affectedSections)
                if (sectionDependencyRevisions_.find(key) == sectionDependencyRevisions_.end())
                    newDependencyNodes.emplace(key, 0);

            PrepareLocalChangeLocked(changes, oldCells, nextCells, invalidatedSections, &nextSectionCells);
            records_.merge(newRecordNode);
            unresolved_.merge(unresolvedNode);
            if (!newRecord) old->second.Record = owned;
            cells_.merge(newCellNodes);
            for (auto& pair : staged)
            {
                auto cell = cells_.find(pair.first);
                if (pair.second.empty()) { if (cell != cells_.end()) cells_.erase(cell); continue; }
                auto& target = cell->second;
                target.Records.swap(pair.second);
                Advance(cellRevisionSequence_);
                target.Revision = cellRevisionSequence_;
            }
            if (!unresolved) unresolved_.erase(owned->Key);
            if (wasUnbounded && !isUnbounded) --unboundedUnresolvedCount_;
            else if (!wasUnbounded && isUnbounded) ++unboundedUnresolvedCount_;
            if (wasUnresolvedOccluder && !isUnresolvedOccluder) --unresolvedOccluderCount_;
            else if (!wasUnresolvedOccluder && isUnresolvedOccluder) ++unresolvedOccluderCount_;
            auto stored = records_.find(owned->Key);
            stored->second.Cells.swap(nextCells);
            if (!newRecord)
            {
                if (oldRecord && oldRecord->SplineOcclusion)
                    for (const auto& oldSection : oldRecord->SplineOcclusion->Sections)
                    {
                        const SectionKey oldKey{owned->Key, oldRecord->Source->SourceRevision, oldSection.Identity};
                        for (const auto& dependency : oldSection.Dependencies)
                        {
                            auto reverse = occluderSections_.find(dependency.Key);
                            if (reverse == occluderSections_.end()) continue;
                            reverse->second.erase(oldKey);
                            if (reverse->second.empty()) occluderSections_.erase(reverse);
                        }
                    }
                for (const auto& section : stored->second.Sections)
                {
                    sectionCells_.erase(section);
                    if (nextSectionCells.find(section) == nextSectionCells.end())
                        sectionDependencyRevisions_.erase(section);
                }
            }
            sectionCells_.merge(newSectionNodes);
            stored->second.Sections.swap(ownedSections);
            occluderSections_.erase(owned->Key);
            sectionDependencyRevisions_.merge(newDependencyNodes);
            Advance(sourceRevision_);
            for (const auto& section : affectedSections)
            {
                auto revision = sectionDependencyRevisions_.find(section);
                if (revision != sectionDependencyRevisions_.end())
                { Advance(dependencyRevision_); revision->second = dependencyRevision_; }
            }
            changes.WorldGeneration = worldGeneration_; changes.SourceRevision = sourceRevision_;
            changes.DependencyRevision = dependencyRevision_;
            changes.InvalidatedCells = std::move(invalidated);
            changes.InvalidatedSections = std::move(invalidatedSections);
            changes.GlobalCompletenessInvalidated = wasUnbounded || isUnbounded;
            changes.GlobalSplineOcclusionInvalidated = globalOcclusionInvalidated;
            ApplyLocalChangeLocked(changes);
            return newRecord ? UpsertResult::Inserted : UpsertResult::Replaced;
        }
        catch (const std::bad_alloc&) { changes = {}; return UpsertResult::AllocationFailure; }
        catch (const std::length_error&) { changes = {}; return UpsertResult::AllocationFailure; }
        catch (...) { changes = {}; return UpsertResult::InvalidInput; }
    }

    RemoveResult Index::Remove(MassKey key, uint64_t generation, ChangeSet& changes) noexcept
    {
        changes = {};
        try
        {
            std::lock_guard lock(mutex_);
            if (generation != worldGeneration_) return RemoveResult::NotFound;
            auto it = records_.find(key);
            if (it == records_.end()) return RemoveResult::NotFound;
            std::set<CellKey> touchedCells = it->second.Cells;
            if (it->second.Record->Source->OrdinarySpriteSupport.Valid &&
                !CellsFor(it->second.Record->Source->OrdinarySpriteSupport, origin_, touchedCells))
                return RemoveResult::AllocationFailure;
            changes.InvalidatedCells.assign(touchedCells.begin(), touchedCells.end());
            const bool wasUnbounded = Unbounded(*it->second.Record);
            std::set<SectionKey> affectedSections;
            for (const auto& pair : sectionCells_)
                for (const auto& cell : touchedCells)
                    if (pair.second.find(cell) != pair.second.end()) { affectedSections.insert(pair.first); break; }
            affectedSections.insert(it->second.Sections.begin(), it->second.Sections.end());
            if (auto reverse = occluderSections_.find(key); reverse != occluderSections_.end())
                affectedSections.insert(reverse->second.begin(), reverse->second.end());
            const bool potentialOccluder = PotentialOrdinaryOccluder(*it->second.Record->Source);
            const bool geometryVerified = VerifiedOccluderGeometry(*it->second.Record->Source);
            const bool globalOcclusionInvalidated = potentialOccluder && !geometryVerified &&
                !it->second.Record->Source->VisualSupport.Valid;
            if (globalOcclusionInvalidated)
                for (const auto& pair : sectionCells_) affectedSections.insert(pair.first);
            if (affectedSections.size() > (std::numeric_limits<uint64_t>::max)() - dependencyRevision_)
                return RemoveResult::AllocationFailure;
            std::vector<SectionKey> invalidatedSections(affectedSections.begin(), affectedSections.end());
            PrepareLocalChangeLocked(changes, it->second.Cells, {}, invalidatedSections);
            for (const auto& cellKey : it->second.Cells)
            {
                auto cell = cells_.find(cellKey);
                if (cell == cells_.end()) continue;
                cell->second.Records.erase(key);
                Advance(cellRevisionSequence_);
                cell->second.Revision = cellRevisionSequence_;
                if (cell->second.Records.empty()) cells_.erase(cell);
            }
            unresolved_.erase(key);
            if (wasUnbounded) --unboundedUnresolvedCount_;
            if (potentialOccluder && !geometryVerified && !it->second.Record->Source->VisualSupport.Valid)
                --unresolvedOccluderCount_;
            for (const auto& section : it->second.Sections)
            {
                sectionCells_.erase(section);
                sectionDependencyRevisions_.erase(section);
            }
            if (it->second.Record->SplineOcclusion)
                for (const auto& oldSection : it->second.Record->SplineOcclusion->Sections)
                {
                    const SectionKey oldKey{key, it->second.Record->Source->SourceRevision, oldSection.Identity};
                    for (const auto& dependency : oldSection.Dependencies)
                    {
                        auto reverse = occluderSections_.find(dependency.Key);
                        if (reverse == occluderSections_.end()) continue;
                        reverse->second.erase(oldKey);
                        if (reverse->second.empty()) occluderSections_.erase(reverse);
                    }
                }
            occluderSections_.erase(key);
            records_.erase(it);
            Advance(sourceRevision_);
            for (const auto& section : affectedSections)
            {
                auto revision = sectionDependencyRevisions_.find(section);
                if (revision != sectionDependencyRevisions_.end())
                { Advance(dependencyRevision_); revision->second = dependencyRevision_; }
            }
            changes.WorldGeneration = worldGeneration_; changes.SourceRevision = sourceRevision_;
            changes.DependencyRevision = dependencyRevision_;
            changes.InvalidatedSections = std::move(invalidatedSections);
            changes.GlobalCompletenessInvalidated = wasUnbounded;
            changes.GlobalSplineOcclusionInvalidated = globalOcclusionInvalidated;
            ApplyLocalChangeLocked(changes);
            return RemoveResult::Removed;
        }
        catch (const std::bad_alloc&) { changes = {}; return RemoveResult::AllocationFailure; }
        catch (const std::length_error&) { changes = {}; return RemoveResult::AllocationFailure; }
        catch (...) { changes = {}; return RemoveResult::NotFound; }
    }

    bool Index::TrySnapshot(CellKey key, CellSnapshot& output) const noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            CellSnapshot value; value.Key = key; value.WorldGeneration = worldGeneration_;
            value.SourceRevision = sourceRevision_;
            value.DependencyRevision = dependencyRevision_;
            value.Origin = origin_;
            auto cell = cells_.find(key);
            if (cell != cells_.end())
            {
                value.CellRevision = cell->second.Revision;
                value.Records.reserve(cell->second.Records.size());
                for (const auto& id : cell->second.Records)
                {
                    auto record = records_.find(id);
                    if (record != records_.end())
                    {
                        if (record->second.Record->Source->InteriorKnown && record->second.Record->Source->InteriorOnly)
                            continue;
                        auto visibleRecord = record->second.Record;
                        if (visibleRecord->SplineOcclusion)
                        {
                            std::shared_ptr<SplineOcclusionPayload> sanitized;
                            const bool payloadMismatch = visibleRecord->SplineOcclusion->WorldGeneration != visibleRecord->WorldGeneration ||
                                visibleRecord->SplineOcclusion->SourceRevision != visibleRecord->Source->SourceRevision;
                            if (payloadMismatch)
                            {
                                sanitized = std::make_shared<SplineOcclusionPayload>(*visibleRecord->SplineOcclusion);
                                sanitized->Sections.clear();
                                sanitized->State = SplineOcclusionState::Unevaluated;
                            }
                            for (size_t i = 0; !payloadMismatch && i < visibleRecord->SplineOcclusion->Sections.size(); ++i)
                            {
                                const auto& derived = visibleRecord->SplineOcclusion->Sections[i];
                                const SectionKey section{visibleRecord->Key, visibleRecord->Source->SourceRevision, derived.Identity};
                                auto current = sectionDependencyRevisions_.find(section);
                                if (current != sectionDependencyRevisions_.end() && current->second == derived.DependencyRevision) continue;
                                if (!sanitized) sanitized = std::make_shared<SplineOcclusionPayload>(*visibleRecord->SplineOcclusion);
                                auto& stale = sanitized->Sections[i];
                                stale.VisibleTriangles.clear();
                                stale.WalkwayInsetTriangles.clear();
                                stale.Complete = false;
                                sanitized->State = SplineOcclusionState::Unevaluated;
                            }
                            if (sanitized)
                            {
                                auto copy = std::make_shared<BuildingRecord>(*visibleRecord);
                                copy->SplineOcclusion = std::move(sanitized);
                                visibleRecord = std::move(copy);
                            }
                        }
                        if (unresolved_.find(id) != unresolved_.end()) value.HasUnresolvedContributor = true;
                        const auto& source = *visibleRecord->Source;
                        if (PotentialOrdinaryOccluder(source) && !VerifiedOccluderGeometry(source))
                            value.HasUnresolvedContributor = true;
                        if (source.HasSplineSource && source.SplinePreparation != SplinePreparationState::Complete)
                            value.HasUnresolvedContributor = true;
                        if (!source.SplineSections.empty())
                        {
                            const auto& occlusion = visibleRecord->SplineOcclusion;
                            if (!occlusion || occlusion->WorldGeneration != visibleRecord->WorldGeneration ||
                                occlusion->SourceRevision != source.SourceRevision ||
                                occlusion->State != SplineOcclusionState::Complete)
                                value.HasUnresolvedContributor = true;
                        }
                        value.Records.push_back(std::move(visibleRecord));
                    }
                }
            }
            // An unresolved unbounded source may affect every cell.
            if (unboundedUnresolvedCount_ || unresolvedOccluderCount_)
                value.HasUnresolvedContributor = true;
            value.Validity = SnapshotValidityLocked(key);
            output = std::move(value);
            return true;
        }
        catch (...) { return false; }
    }

    bool Index::TryGetRecord(MassKey key, uint64_t generation,
        std::shared_ptr<const BuildingRecord>& output) const noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            if (generation != worldGeneration_) return false;
            auto it = records_.find(key);
            if (it == records_.end()) return false;
            auto value = it->second.Record;
            if (value->SplineOcclusion)
            {
                std::shared_ptr<SplineOcclusionPayload> sanitized;
                const bool payloadMismatch = value->SplineOcclusion->WorldGeneration != value->WorldGeneration ||
                    value->SplineOcclusion->SourceRevision != value->Source->SourceRevision;
                if (payloadMismatch)
                {
                    sanitized = std::make_shared<SplineOcclusionPayload>(*value->SplineOcclusion);
                    sanitized->Sections.clear();
                    sanitized->State = SplineOcclusionState::Unevaluated;
                }
                for (size_t i = 0; !payloadMismatch && i < value->SplineOcclusion->Sections.size(); ++i)
                {
                    const auto& derived = value->SplineOcclusion->Sections[i];
                    const SectionKey section{value->Key, value->Source->SourceRevision, derived.Identity};
                    auto current = sectionDependencyRevisions_.find(section);
                    if (current != sectionDependencyRevisions_.end() && current->second == derived.DependencyRevision) continue;
                    if (!sanitized) sanitized = std::make_shared<SplineOcclusionPayload>(*value->SplineOcclusion);
                    auto& stale = sanitized->Sections[i];
                    stale.VisibleTriangles.clear();
                    stale.WalkwayInsetTriangles.clear();
                    stale.Complete = false;
                    sanitized->State = SplineOcclusionState::Unevaluated;
                }
                if (sanitized)
                {
                    auto copy = std::make_shared<BuildingRecord>(*value);
                    copy->SplineOcclusion = std::move(sanitized);
                    value = std::move(copy);
                }
            }
            output = std::move(value);
            return true;
        }
        catch (...) { output.reset(); return false; }
    }

    std::vector<SectionKey> Index::SnapshotSplineSections() const
    {
        std::lock_guard lock(mutex_);
        std::vector<SectionKey> result;
        result.reserve(sectionCells_.size());
        for (const auto& pair : sectionCells_) result.push_back(pair.first);
        return result;
    }

    bool Index::IsCurrentSplineSection(SectionKey section, uint64_t generation) const noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            auto owner = records_.find(section.Building);
            if (!generation || generation != worldGeneration_ || owner == records_.end() ||
                owner->second.Record->WorldGeneration != generation ||
                !owner->second.Record->Source || owner->second.Record->Source->SourceRevision != section.SourceRevision ||
                sectionCells_.find(section) == sectionCells_.end() ||
                sectionDependencyRevisions_.find(section) == sectionDependencyRevisions_.end())
                return false;
            return std::find_if(owner->second.Record->Source->SplineSections.begin(),
                owner->second.Record->Source->SplineSections.end(),
                [&](const auto& value) { return value.Identity == section.Identity; }) !=
                owner->second.Record->Source->SplineSections.end();
        }
        catch (...) { return false; }
    }

    bool Index::TryGetOcclusionInputs(SectionKey section,
        std::shared_ptr<const BuildingRecord>& spline,
        std::vector<std::shared_ptr<const BuildingRecord>>& ordinaryCandidates,
        bool& hasUnresolvedOrdinary, uint64_t& dependencyRevision) const noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            spline.reset(); ordinaryCandidates.clear(); hasUnresolvedOrdinary = false; dependencyRevision = 0;
            auto owner = records_.find(section.Building);
            if (owner == records_.end() || owner->second.Record->WorldGeneration != worldGeneration_ ||
                !owner->second.Record->Source || owner->second.Record->Source->SourceRevision != section.SourceRevision)
                return false;
            const auto sourceSection = std::find_if(owner->second.Record->Source->SplineSections.begin(),
                owner->second.Record->Source->SplineSections.end(), [&](const auto& value) { return value.Identity == section.Identity; });
            if (sourceSection == owner->second.Record->Source->SplineSections.end()) return false;
            auto sectionCells = sectionCells_.find(section);
            if (sectionCells == sectionCells_.end()) return false;
            auto revision = sectionDependencyRevisions_.find(section);
            if (revision == sectionDependencyRevisions_.end()) return false;
            dependencyRevision = revision->second;
            spline = owner->second.Record;

            std::set<MassKey> candidateKeys;
            for (const auto& cellKey : sectionCells->second)
            {
                auto cell = cells_.find(cellKey);
                if (cell != cells_.end()) candidateKeys.insert(cell->second.Records.begin(), cell->second.Records.end());
            }
            // Unknown classification or absent geometry can hide an eligible
            // ordinary occluder anywhere; keep this section unevaluable.
            hasUnresolvedOrdinary = unresolvedOccluderCount_ != 0;
            ordinaryCandidates.reserve(candidateKeys.size());
            for (const auto& key : candidateKeys)
            {
                auto candidate = records_.find(key);
                if (candidate == records_.end()) continue;
                const auto& source = *candidate->second.Record->Source;
                if (!source.InteriorKnown)
                { hasUnresolvedOrdinary = true; continue; }
                if (!PotentialOrdinaryOccluder(source)) continue;
                if (!VerifiedOccluderGeometry(source))
                {
                    if (!source.VisualSupport.Valid || Intersects(source.VisualSupport, sourceSection->Bounds))
                        hasUnresolvedOrdinary = true;
                    continue;
                }
                ordinaryCandidates.push_back(candidate->second.Record);
            }
            return true;
        }
        catch (...) { return false; }
    }

    bool Index::PublishSplineOcclusion(SectionKey section, uint64_t generation,
        OccludedSplineSection output, SplineOcclusionState state, ChangeSet& changes) noexcept
    {
        changes = {};
        try
        {
            std::lock_guard lock(mutex_);
            auto owner = records_.find(section.Building);
            auto revision = sectionDependencyRevisions_.find(section);
            if (generation != worldGeneration_ || owner == records_.end() || revision == sectionDependencyRevisions_.end() ||
                owner->second.Record->WorldGeneration != generation || !owner->second.Record->Source ||
                owner->second.Record->Source->SourceRevision != section.SourceRevision ||
                output.Identity != section.Identity || output.DependencyRevision != revision->second)
                return false;
            auto sourceSection = std::find_if(owner->second.Record->Source->SplineSections.begin(),
                owner->second.Record->Source->SplineSections.end(), [&](const auto& value) { return value.Identity == section.Identity; });
            if (sourceSection == owner->second.Record->Source->SplineSections.end()) return false;
            const auto dependencies = output.Dependencies;
            for (const auto& dependency : dependencies)
            {
                auto current = records_.find(dependency.Key);
                if (current == records_.end() || !current->second.Record->Source ||
                    current->second.Record->Source->SourceRevision != dependency.SourceRevision ||
                    !current->second.Record->Source->SpriteQuadValid)
                    return false;
                for (size_t i = 0; i < dependency.Corners.size(); ++i)
                    if (current->second.Record->Source->SpriteQuadWorld[i][0] != dependency.Corners[i][0] ||
                        current->second.Record->Source->SpriteQuadWorld[i][1] != dependency.Corners[i][1]) return false;
            }
            std::vector<SplineOccluderDependency> oldDependencies;
            if (owner->second.Record->SplineOcclusion)
            {
                const auto prior = std::find_if(owner->second.Record->SplineOcclusion->Sections.begin(),
                    owner->second.Record->SplineOcclusion->Sections.end(),
                    [&](const auto& value) { return value.Identity == section.Identity; });
                if (prior != owner->second.Record->SplineOcclusion->Sections.end()) oldDependencies = prior->Dependencies;
            }
            auto payload = owner->second.Record->SplineOcclusion ?
                std::make_shared<SplineOcclusionPayload>(*owner->second.Record->SplineOcclusion) :
                std::make_shared<SplineOcclusionPayload>();
            payload->WorldGeneration = generation;
            payload->SourceRevision = section.SourceRevision;
            payload->DependencyRevision = dependencyRevision_;
            auto storedSection = std::find_if(payload->Sections.begin(), payload->Sections.end(),
                [&](const auto& value) { return value.Identity == section.Identity; });
            if (storedSection == payload->Sections.end()) payload->Sections.push_back(std::move(output));
            else *storedSection = std::move(output);
            payload->State = state;
            if (payload->Sections.size() != owner->second.Record->Source->SplineSections.size())
                payload->State = SplineOcclusionState::Partial;
            else if (std::any_of(payload->Sections.begin(), payload->Sections.end(), [](const auto& value) { return !value.Complete; }))
                payload->State = SplineOcclusionState::Partial;

            auto updated = std::make_shared<BuildingRecord>(*owner->second.Record);
            updated->SplineOcclusion = std::move(payload);
            ChangeSet preparedChanges;
            preparedChanges.WorldGeneration = generation;
            preparedChanges.SourceRevision = sourceRevision_;
            preparedChanges.DependencyRevision = dependencyRevision_;
            preparedChanges.InvalidatedSections.push_back(section);
            preparedChanges.InvalidatedCells.assign(owner->second.Cells.begin(), owner->second.Cells.end());
            PrepareLocalChangeLocked(preparedChanges, owner->second.Cells, owner->second.Cells,
                preparedChanges.InvalidatedSections);

            // Insert new reverse edges before dropping old ones. Allocation
            // failure can leave harmless extra edges, never a missing edge.
            for (const auto& dependency : dependencies)
                occluderSections_[dependency.Key].insert(section);
            for (const auto& prior : oldDependencies)
            {
                const bool retained = std::any_of(dependencies.begin(), dependencies.end(),
                    [&](const auto& dependency) { return dependency.Key == prior.Key; });
                if (retained) continue;
                auto reverse = occluderSections_.find(prior.Key);
                if (reverse == occluderSections_.end()) continue;
                reverse->second.erase(section);
                if (reverse->second.empty()) occluderSections_.erase(reverse);
            }
            owner->second.Record = std::move(updated);
            for (const auto& cellKey : owner->second.Cells)
            {
                auto cell = cells_.find(cellKey);
                if (cell != cells_.end()) { Advance(cellRevisionSequence_); cell->second.Revision = cellRevisionSequence_; }
            }
            ApplyLocalChangeLocked(preparedChanges);
            changes = std::move(preparedChanges);
            return true;
        }
        catch (...) { changes = {}; return false; }
    }

    std::vector<std::shared_ptr<const BuildingRecord>> Index::SnapshotUnresolved() const
    {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<const BuildingRecord>> result;
        result.reserve(unresolved_.size());
        for (const auto& key : unresolved_)
        {
            auto it = records_.find(key);
            if (it != records_.end()) result.push_back(it->second.Record);
        }
        return result;
    }

    uint64_t Index::WorldGeneration() const noexcept
    { std::lock_guard lock(mutex_); return worldGeneration_; }

    uint64_t Index::SourceRevision() const noexcept
    { std::lock_guard lock(mutex_); return sourceRevision_; }

    uint64_t Index::DependencyRevision() const noexcept
    { std::lock_guard lock(mutex_); return dependencyRevision_; }
}
