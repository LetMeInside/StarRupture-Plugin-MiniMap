#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

// Retains one immutable StarRupture collector snapshot and adapts its section
// observations to the game-independent publication boundary.

#include "BuildingCollector.h"
#include "Services/Map/Buildings/IBuildingSourcePublication.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace MiniMapBuildingCollector
{
    /** Owns one fixed immutable Snapshot while observing it through the neutral boundary.
     * The nested Snapshot owners remain alive for this object's shared lifetime. Its weak
     * eligibility token is independent of source retention, so revocation never invalidates
     * borrowed source bytes and this object never keeps its collector or native world alive.
     */
    class BuildingSourcePublication final : public Services::Map::Buildings::IBuildingSourcePublication
    {
    public:
        /** Plain serialized-call-thread eligibility evidence owned by the collector State.
         * Publications observe it weakly, so retaining a publication cannot retain State.
         */
        struct EligibilityToken
        {
            bool IsEligible = true;
        };

        /** Retains the selected immutable snapshot and observes collector-owned eligibility.
         * @param snapshot The one immutable source publication described by this object.
         * @param eligibility The collector-local currentness evidence, if available.
         */
        BuildingSourcePublication(
            std::shared_ptr<const Snapshot> snapshot,
            std::weak_ptr<EligibilityToken> eligibility) noexcept;

        /** Reports current eligibility without accessing collector or native state.
         * @return Whether the fixed Snapshot is still eligible as current source.
         */
        bool IsCurrent() const noexcept override;

        /** Returns a conservative incomplete neutral view with validated snapshot coverage.
         * @return Borrowed view with no numerical descriptors until a later adaptation stage.
         */
        Services::Map::Buildings::BuildingSourceView BorrowSourceView() const noexcept override;

        /** Observes this retained snapshot, including after its currentness is revoked.
         * @param grid Requested half-open section grid in service map units.
         * @param observations Caller-owned row-major output slots.
         */
        void ObserveSections(
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            std::span<Services::Map::Buildings::BuildingSectionObservation> observations) const override;

        /** Runs the one shared snapshot accumulation algorithm for collector and publication calls.
         * @param snapshot The exact immutable source to summarize.
         * @param grid Requested row-major section grid.
         * @param observations Caller-owned result slots, completely initialized by the operation.
         */
        static void ObserveSnapshot(
            const Snapshot& snapshot,
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            std::span<Services::Map::Buildings::BuildingSectionObservation> observations);

        /** Validates finite ordered support bounds used by snapshot coverage and record extents.
         * @param extent Candidate support rectangle.
         * @return Whether its flag and finite ordered coordinates are valid.
         */
        static bool HasValidSupportExtent(const Bounds& extent) noexcept;

    private:
        /** Commutative contributor data accumulated for one requested section. */
        struct SectionAccumulator
        {
            uint64_t Sum = 0;
            uint64_t Xor = 0;
            uint64_t Count = 0;
        };

        /** Clipped half-open section-index range touched by one record extent. */
        struct SectionIndexRange
        {
            size_t FirstColumn = 0;
            size_t EndColumn = 0;
            size_t FirstRow = 0;
            size_t EndRow = 0;
        };

        /** Validates grid dimensions and calculates its finite half-open outer bounds.
         * @param grid Requested origin, section size and row/column dimensions.
         * @param observationCount Number of caller output slots.
         * @param sectionCount Receives the validated row-major cell count.
         * @param gridRight Receives the finite exclusive right edge.
         * @param gridBottom Receives the finite exclusive bottom edge.
         * @return Whether grid dimensions, output size and outer bounds are valid.
         */
        static bool GetSectionGridBounds(
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            size_t observationCount,
            size_t& sectionCount,
            long double& gridRight,
            long double& gridBottom);

        /** Finds the clipped section range that may intersect a record's support extent.
         * @param record Snapshot contributor whose support is tested.
         * @param grid Requested grid geometry.
         * @param gridRight Exclusive right edge of the requested grid.
         * @param gridBottom Exclusive bottom edge of the requested grid.
         * @param range Receives the clipped candidate indices when valid.
         * @return Whether the record extent is valid and its candidate range is known.
         */
        static bool GetSectionIndexRange(
            const Record& record,
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            long double gridRight,
            long double gridBottom,
            SectionIndexRange& range);

        /** Accumulates one record fingerprint into each half-open section it intersects.
         * @param record Snapshot contributor and placement fingerprint.
         * @param grid Requested grid geometry.
         * @param gridRight Exclusive right edge of the requested grid.
         * @param gridBottom Exclusive bottom edge of the requested grid.
         * @param accumulators Caller-owned row-major multiset accumulators.
         * @return Whether the record's support extent is valid.
         */
        static bool AccumulateRecordSections(
            const Record& record,
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            long double gridRight,
            long double gridBottom,
            std::span<SectionAccumulator> accumulators);

        /** Applies the established final mixer to an order-independent section multiset.
         * @param accumulator Sum, XOR and count accumulated for one section.
         * @return Existing 64-bit section membership fingerprint.
         */
        static uint64_t FinalizeSectionFingerprint(const SectionAccumulator& accumulator) noexcept;

        /** The sole retained authoritative record owner for this publication. */
        std::shared_ptr<const Snapshot> snapshot_;
        /** Weak currentness evidence cannot extend the collector State lifetime. */
        std::weak_ptr<EligibilityToken> eligibility_;
    };
}

#endif
