#include "BuildingComplexIndex.h"
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

        bool ValidKey(MassKey key) noexcept { return key.Index >= 0 && key.Serial > 0; }
        bool Unbounded(const BuildingRecord& record) noexcept
        { return record.Source->Support == SupportState::Unknown && !record.Source->VisualSupport.Valid; }
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

    bool Index::BeginWorld(uint64_t generation, WorldOrigin origin) noexcept
    {
        if (!generation || !origin.Valid || !std::isfinite(origin.X) || !std::isfinite(origin.Y)) return false;
        try
        {
            std::lock_guard lock(mutex_);
            records_.clear(); cells_.clear(); unresolved_.clear(); sectionCells_.clear(); occluderSections_.clear();
            unboundedUnresolvedCount_ = 0;
            worldGeneration_ = generation; origin_ = origin;
            ++resetRevision_; Advance(sourceRevision_);
            return true;
        }
        catch (...) { return false; }
    }

    void Index::ResetWorld(uint64_t nextGeneration) noexcept
    {
        std::lock_guard lock(mutex_);
        records_.clear(); cells_.clear(); unresolved_.clear(); sectionCells_.clear(); occluderSections_.clear();
        unboundedUnresolvedCount_ = 0;
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
            std::lock_guard lock(mutex_);
            if (input.WorldGeneration != worldGeneration_ || !origin_.Valid) return UpsertResult::InvalidInput;
            if (input.Source->Support == SupportState::Supported && !ValidBounds(input.Source->VisualSupport)) return UpsertResult::InvalidInput;
            if (input.Source->Support == SupportState::ExplicitlyUnsupported && input.Source->VisualSupport.Valid &&
                !ValidBounds(input.Source->VisualSupport)) return UpsertResult::InvalidInput;

            auto old = records_.find(input.Key);
            if (old != records_.end() && old->second.Record->Source->SourceRevision == input.Source->SourceRevision &&
                old->second.Record->Source->SourceGeneration == input.Source->SourceGeneration &&
                old->second.Record->Source->Support == input.Source->Support &&
                old->second.Record->Source->VisualSupport == input.Source->VisualSupport)
                return UpsertResult::Unchanged;

            std::set<CellKey> nextCells;
            const bool boundedUnknown = input.Source->Support == SupportState::Unknown && input.Source->VisualSupport.Valid;
            if (input.Source->Support == SupportState::Supported || boundedUnknown)
                if (!CellsFor(input.Source->VisualSupport, origin_, nextCells)) return UpsertResult::InvalidInput;
            const std::set<CellKey> oldCells = old == records_.end() ? std::set<CellKey>{} : old->second.Cells;

            std::set<CellKey> touched = oldCells;
            touched.insert(nextCells.begin(), nextCells.end());
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
            const bool unresolved = owned->Source->Support == SupportState::Unknown;
            const bool wasUnbounded = old != records_.end() && Unbounded(*old->second.Record);
            const bool isUnbounded = Unbounded(*owned);
            std::vector<CellKey> invalidated(touched.begin(), touched.end());
            // Prepare all allocating map nodes before committing any visible
            // membership changes. merge transfers the prepared nodes.
            std::map<CellKey, Cell> newCellNodes;
            for (const auto& pair : staged)
                if (cells_.find(pair.first) == cells_.end() && !pair.second.empty())
                    newCellNodes.emplace(pair.first, Cell{});
            std::map<MassKey, Stored> newRecordNode;
            if (newRecord) newRecordNode.emplace(owned->Key, Stored{owned, nextCells});
            std::set<MassKey> unresolvedNode;
            if (unresolved && unresolved_.find(owned->Key) == unresolved_.end()) unresolvedNode.insert(owned->Key);

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
            records_.find(owned->Key)->second.Cells.swap(nextCells);
            Advance(sourceRevision_);
            changes.WorldGeneration = worldGeneration_; changes.SourceRevision = sourceRevision_;
            changes.InvalidatedCells = std::move(invalidated);
            changes.GlobalCompletenessInvalidated = wasUnbounded || isUnbounded;
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
            changes.InvalidatedCells.assign(it->second.Cells.begin(), it->second.Cells.end());
            const bool wasUnbounded = Unbounded(*it->second.Record);
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
            records_.erase(it);
            Advance(sourceRevision_);
            changes.WorldGeneration = worldGeneration_; changes.SourceRevision = sourceRevision_;
            changes.GlobalCompletenessInvalidated = wasUnbounded;
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
                        value.Records.push_back(record->second.Record);
                        if (unresolved_.find(id) != unresolved_.end()) value.HasUnresolvedContributor = true;
                    }
                }
            }
            // An unresolved unbounded source may affect every cell.
            if (unboundedUnresolvedCount_) value.HasUnresolvedContributor = true;
            output = std::move(value);
            return true;
        }
        catch (...) { return false; }
    }

    bool Index::TryGetRecord(MassKey key, uint64_t generation,
        std::shared_ptr<const BuildingRecord>& output) const noexcept
    {
        std::lock_guard lock(mutex_);
        if (generation != worldGeneration_) return false;
        auto it = records_.find(key);
        if (it == records_.end()) return false;
        output = it->second.Record;
        return true;
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
}
