// Implements fixed-Snapshot source retention, conservative neutral views and
// the shared batch section-observation algorithm for the StarRupture producer.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingSourcePublication.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace MiniMapBuildingCollector
{
    /** Moves in the one fixed Snapshot owner and weakly associates its revocable eligibility.
     * The publication owns immutable source bytes; the weak token does not extend State lifetime.
     * Releasing this object performs only ordinary CPU shared-owner release.
     * @param snapshot Immutable Snapshot described for this publication's lifetime.
     * @param eligibility Collector-owned eligibility for that Snapshot publication.
     */
    BuildingSourcePublication::BuildingSourcePublication(
        std::shared_ptr<const Snapshot> snapshot,
        std::weak_ptr<EligibilityToken> eligibility) noexcept
        : snapshot_(std::move(snapshot)), eligibility_(std::move(eligibility))
    {
    }

    /** Checks independently revocable weak eligibility without consulting collector or native state.
     * Expired and explicitly revoked tokens both report false.
     * @return Whether this fixed Snapshot remains eligible as current source.
     */
    bool BuildingSourcePublication::IsCurrent() const noexcept
    {
        const auto eligibility = eligibility_.lock();
        return eligibility && eligibility->IsEligible;
    }

    /** Borrows frozen sprite requirements and supported spline adaptations without extra allocation.
     * Optional sprite placement appears only when its copied transform was validated; unsupported
     * spline facts remain absent. Readiness stays Incomplete, and empty spans do not certify no content.
     * Only finite positive bounds become coverage.
     * @return Borrowed view whose source storage remains owned by this publication.
     */
    Services::Map::Buildings::BuildingSourceView BuildingSourcePublication::BorrowSourceView() const noexcept
    {
        Services::Map::Buildings::BuildingSourceView view{};
        if (!snapshot_)
        {
            return view;
        }

        view.Readiness = Services::Map::Buildings::BuildingSourceReadiness::Incomplete;
        view.OrdinarySprites = snapshot_->OrdinarySprites;
        view.Splines = snapshot_->Splines;
        view.OverlayLastSprites = snapshot_->OverlayLastSprites;
        const Bounds& region = snapshot_->Region;
        if (!HasValidSupportExtent(region) || region.MaxX <= region.MinX || region.MaxY <= region.MinY)
        {
            return view;
        }

        const double width = region.MaxX - region.MinX;
        const double height = region.MaxY - region.MinY;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        {
            return view;
        }

        view.HasCoverage = true;
        view.Coverage = {region.MinX, region.MinY, width, height};
        return view;
    }

    /** Observes only this object's fixed Snapshot; revocation does not invalidate its bytes.
     * The retained owner keeps records alive, and this method never reacquires collector state
     * or switches to a newer Snapshot.
     * @param grid Requested half-open section grid in map units.
     * @param observations Caller-owned row-major result slots.
     */
    void BuildingSourcePublication::ObserveSections(
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        std::span<Services::Map::Buildings::BuildingSectionObservation> observations) const
    {
        if (!snapshot_)
        {
            std::fill(observations.begin(), observations.end(),
                Services::Map::Buildings::BuildingSectionObservation{});
            return;
        }

        ObserveSnapshot(*snapshot_, grid, observations);
    }

    /** Initializes results as incomplete and certifies them only after valid grid and coverage,
     * complete source membership/acquisition, and successful aggregation of every Snapshot record.
     * The caller retains this immutable Snapshot for the synchronous traversal; no newer source is
     * queried. Results describe membership content, not contributor identity or rendering state.
     * @param snapshot Fixed immutable source borrowed for this call.
     * @param grid Requested row-major half-open grid.
     * @param observations Caller-owned output slots, all reset before validation.
     */
    void BuildingSourcePublication::ObserveSnapshot(
        const Snapshot& snapshot,
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        std::span<Services::Map::Buildings::BuildingSectionObservation> observations)
    {
        std::fill(observations.begin(), observations.end(),
            Services::Map::Buildings::BuildingSectionObservation{});

        size_t sectionCount = 0;
        long double gridRight = 0;
        long double gridBottom = 0;
        if (!GetSectionGridBounds(grid, observations.size(), sectionCount, gridRight, gridBottom))
        {
            return;
        }

        if (!snapshot.Complete || !snapshot.AcquisitionComplete)
        {
            return;
        }

        const Bounds& acquiredRegion = snapshot.Region;
        if (!HasValidSupportExtent(acquiredRegion) ||
            acquiredRegion.MinX > grid.xMapUnits || acquiredRegion.MinY > grid.yMapUnits ||
            acquiredRegion.MaxX < gridRight || acquiredRegion.MaxY < gridBottom)
        {
            return;
        }

        std::vector<SectionAccumulator> accumulators(sectionCount);
        for (const Record& record : snapshot.Records)
        {
            if (!AccumulateRecordSections(record, grid, gridRight, gridBottom, accumulators))
            {
                return;
            }
        }

        for (size_t index = 0; index < sectionCount; ++index)
        {
            observations[index].isComplete = true;
            observations[index].membershipFingerprint = FinalizeSectionFingerprint(accumulators[index]);
        }
    }

    /** Checks the validity flag and finite ordered coordinates of a support rectangle.
     * Equal edges are accepted for generic extent validation; positive coverage is checked
     * separately before BorrowSourceView exposes a coverage rectangle.
     * @param extent Candidate map-space support bounds.
     * @return Whether the extent is marked valid and its edges are finite and ordered.
     */
    bool BuildingSourcePublication::HasValidSupportExtent(const Bounds& extent) noexcept
    {
        return extent.Valid && std::isfinite(extent.MinX) && std::isfinite(extent.MinY) &&
            std::isfinite(extent.MaxX) && std::isfinite(extent.MaxY) &&
            extent.MinX <= extent.MaxX && extent.MinY <= extent.MaxY;
    }

    /** Validates grid dimensions and derives finite exclusive right and bottom edges.
     * Rejects zero dimensions, multiplication overflow, output-size mismatch, non-finite inputs
     * and bounds that do not extend positively from the origin.
     * @param grid Map-space origin, positive square cell size and grid dimensions.
     * @param observationCount Number of caller-owned output slots.
     * @param sectionCount Receives the cell count after dimension validation.
     * @param gridRight Receives the exclusive right edge when validation succeeds.
     * @param gridBottom Receives the exclusive bottom edge when validation succeeds.
     * @return Whether dimensions, output count and half-open outer bounds are valid.
     */
    bool BuildingSourcePublication::GetSectionGridBounds(
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        size_t observationCount,
        size_t& sectionCount,
        long double& gridRight,
        long double& gridBottom)
    {
        if (!std::isfinite(grid.xMapUnits) || !std::isfinite(grid.yMapUnits) ||
            !std::isfinite(grid.sectionSizeMapUnits) || grid.sectionSizeMapUnits <= 0 ||
            grid.columnCount == 0 || grid.rowCount == 0 ||
            grid.rowCount > (std::numeric_limits<size_t>::max)() / grid.columnCount)
        {
            return false;
        }

        sectionCount = grid.columnCount * grid.rowCount;
        if (sectionCount != observationCount)
        {
            return false;
        }

        gridRight = static_cast<long double>(grid.xMapUnits) +
            static_cast<long double>(grid.sectionSizeMapUnits) * grid.columnCount;
        gridBottom = static_cast<long double>(grid.yMapUnits) +
            static_cast<long double>(grid.sectionSizeMapUnits) * grid.rowCount;
        return std::isfinite(gridRight) && std::isfinite(gridBottom) &&
            gridRight > grid.xMapUnits && gridBottom > grid.yMapUnits;
    }

    /** Clips a valid record extent to candidate cells with floor/ceil index bounds.
     * Exact cell-edge checks later preserve half-open intersection semantics. An extent outside
     * the grid succeeds with no candidates; this range is not an entity identity or stable key.
     * @param record Snapshot contributor whose support is mapped to cells.
     * @param grid Requested map-space grid.
     * @param gridRight Exclusive right edge of the grid.
     * @param gridBottom Exclusive bottom edge of the grid.
     * @param range Receives candidates when intersecting; outside extents leave it untouched.
     * @return Whether the record extent is valid.
     */
    bool BuildingSourcePublication::GetSectionIndexRange(
        const Record& record,
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        long double gridRight,
        long double gridBottom,
        SectionIndexRange& range)
    {
        const Bounds& extent = record.Extent;
        if (!HasValidSupportExtent(extent))
        {
            return false;
        }

        if (extent.MaxX <= grid.xMapUnits || extent.MinX >= gridRight ||
            extent.MaxY <= grid.yMapUnits || extent.MinY >= gridBottom)
        {
            return true;
        }

        const long double originX = grid.xMapUnits;
        const long double originY = grid.yMapUnits;
        const long double sectionSize = grid.sectionSizeMapUnits;
        const long double columns = grid.columnCount;
        const long double rows = grid.rowCount;
        const long double firstColumn = std::clamp(
            std::floor((static_cast<long double>(extent.MinX) - originX) / sectionSize), 0.0L, columns);
        const long double endColumn = std::clamp(
            std::ceil((static_cast<long double>(extent.MaxX) - originX) / sectionSize), 0.0L, columns);
        const long double firstRow = std::clamp(
            std::floor((static_cast<long double>(extent.MinY) - originY) / sectionSize), 0.0L, rows);
        const long double endRow = std::clamp(
            std::ceil((static_cast<long double>(extent.MaxY) - originY) / sectionSize), 0.0L, rows);
        range = {
            static_cast<size_t>(firstColumn), static_cast<size_t>(endColumn),
            static_cast<size_t>(firstRow), static_cast<size_t>(endRow)};
        return true;
    }

    /** Adds one record occurrence to every exact half-open cell touched by its support.
     * Candidate clipping is followed by exact support intersection; reference-point position
     * does not control membership. Identical fingerprints remain separate occurrences in Count.
     * Fingerprints are content inputs, never contributor identities.
     * @param record One immutable Snapshot record occurrence.
     * @param grid Requested map-space grid.
     * @param gridRight Exclusive right edge of the grid.
     * @param gridBottom Exclusive bottom edge of the grid.
     * @param accumulators Caller-owned row-major Sum/Xor/Count storage.
     * @return Whether the record extent is valid and can be accumulated.
     */
    bool BuildingSourcePublication::AccumulateRecordSections(
        const Record& record,
        const Services::Map::Buildings::BuildingSectionGrid& grid,
        long double gridRight,
        long double gridBottom,
        std::span<SectionAccumulator> accumulators)
    {
        SectionIndexRange range{};
        if (!GetSectionIndexRange(record, grid, gridRight, gridBottom, range))
        {
            return false;
        }

        const long double originX = grid.xMapUnits;
        const long double originY = grid.yMapUnits;
        const long double sectionSize = grid.sectionSizeMapUnits;
        for (size_t row = range.FirstRow; row < range.EndRow; ++row)
        {
            const long double sectionTop = originY + static_cast<long double>(row) * sectionSize;
            const long double sectionBottom = sectionTop + sectionSize;
            if (record.Extent.MaxY <= sectionTop || record.Extent.MinY >= sectionBottom)
            {
                continue;
            }

            for (size_t column = range.FirstColumn; column < range.EndColumn; ++column)
            {
                const long double sectionLeft = originX + static_cast<long double>(column) * sectionSize;
                const long double sectionRight = sectionLeft + sectionSize;
                if (record.Extent.MaxX <= sectionLeft || record.Extent.MinX >= sectionRight)
                {
                    continue;
                }

                auto& accumulator = accumulators[row * grid.columnCount + column];
                accumulator.Sum += record.SourceFingerprint;
                accumulator.Xor ^= record.SourceFingerprint;
                ++accumulator.Count;
            }
        }

        return true;
    }

    /** Applies the existing multiset finalizer to one section's Sum, Xor and Count.
     * The result is a membership-content signature, not contributor or publication identity,
     * a rendering dependency revision, or sprite-pixel evidence.
     * @param accumulator Order-independent content and multiplicity accumulated for one cell.
     * @return Existing 64-bit section membership fingerprint.
     */
    uint64_t BuildingSourcePublication::FinalizeSectionFingerprint(
        const SectionAccumulator& accumulator) noexcept
    {
        uint64_t value = accumulator.Sum ^ std::rotl(accumulator.Xor, 23);
        value += accumulator.Count * 0x9E3779B97F4A7C15ull;
        value ^= value >> 30;
        value *= 0xBF58476D1CE4E5B9ull;
        value ^= value >> 27;
        value *= 0x94D049BB133111EBull;
        value ^= value >> 31;
        return value;
    }
}
#endif
