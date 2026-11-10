// Declares an independent, caller-driven StarRupture native acquisition collector.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"
#include "Services/Map/Buildings/BuildingCandidateData.h"
#include "Services/Map/Buildings/IBuildingCollector.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace MiniMap::BuildingAcquisition
{
    /** Owns acquisition for one gameplay session; calls are serialized on the game thread.
     * Update advances acquisition without owning or terminating the session. The owner
     * must call ShutdownResources before native resources are destroyed or this collector
     * is destroyed. Reset and ShutdownResources remain explicit lifecycle operations.
     * This class registers nothing and has no singleton.
     */
    class BuildingCollectorV2 final : public ::Services::Map::Buildings::IBuildingCollector
    {
    public:
        /** Creates empty CPU state and fixes the minimum interval between membership scans.
         * @param membershipInterval Positive delay between authoritative Mass membership scans.
         * @throws std::invalid_argument when the interval is zero or negative.
         */
        explicit BuildingCollectorV2(
            std::chrono::milliseconds membershipInterval = std::chrono::milliseconds{1000});
        /** Requires prior ShutdownResources; performs no implicit native shutdown. */
        ~BuildingCollectorV2() override;
        BuildingCollectorV2(const BuildingCollectorV2&) = delete;
        BuildingCollectorV2& operator=(const BuildingCollectorV2&) = delete;

        /** Roots per-instance acquisition metadata on the game thread; retries missing metadata. */
        bool InitializeResources();
        /** Accepts an exact finite positive request without excluding any entity; false leaves it unchanged. */
        bool SetAcquisitionRegion(const BuildingAcquiredFacts::Region& region) noexcept;
        /** Sets the policy applied when requested Section publications are constructed.
         * The default is enabled; a changed value does not alter the acquired dataset or its hash.
         * @param enabled Whether known drone rails remain eligible for candidate publication.
         */
        void SetRenderDroneRails(bool enabled) noexcept;
        /** Converts neutral map units directly to StarRupture native centimetres and applies the common validator. */
        bool SetAcquisitionRegion(
            const ::Services::Map::Buildings::BuildingCollectionRegion& region) noexcept override;
        /** Reads the hash of the committed regional dataset without advancing acquisition. */
        std::uint64_t AcquisitionHash() const noexcept override;
        /** Builds ordered Section publications from the last successful committed regional scan.
         * Throws when a non-empty request cannot be validated or the committed dataset is unavailable.
         */
        std::vector<std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>>
            AcquireSectionPublications(
                std::span<const ::Services::Map::Buildings::BuildingCollectionRegion> sectionRegions)
                const override;
        /** Advances pending acquisition on the permitted game thread using MiniMapMap::GetWorld().
         * An unavailable world or uninitialized metadata leaves acquisition state untouched.
         * This does not establish, replace or terminate session ownership; native resources
         * remain the owner's responsibility.
         */
        void Update() override;
        /** Clears the committed regional result and query/session roots while native services remain live. */
        void Reset();
        /** Clears session then releases metadata/conversion roots; required before destruction. */
        void ShutdownResources();

    private:
        /** Creates the unchanged placement-content signature from one exact acquired occurrence. */
        static std::uint64_t ComputeOccurrenceSignature(const BuildingAcquiredFacts::Building& building) noexcept;

        /** Appends the canonical quaternion bytes using the established sign and component order. */
        static void AppendCanonicalQuaternion(
            std::uint64_t& hash,
            const BuildingAcquiredFacts::Quaternion& rotation) noexcept;

        /** Applies one FNV-1a byte step to the placement-content hash. */
        static void AppendHashByte(std::uint64_t& hash, std::uint8_t value) noexcept;

        /** Serializes an unsigned integer in little-endian order into the placement hash. */
        static void AppendHashInteger(std::uint64_t& hash, std::uint64_t value) noexcept;

        /** Serializes a floating component while canonicalizing signed zero. */
        static void AppendHashDouble(std::uint64_t& hash, double value) noexcept;

        /** Requires ready transform evidence and finite native map X/Y coordinates. */
        static bool HasValidMapPosition(const BuildingAcquiredFacts::Building& building) noexcept;

        struct State;
        std::unique_ptr<State> state_;
    };
}
#endif
