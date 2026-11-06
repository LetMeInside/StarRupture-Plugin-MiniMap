// Declares an independent, caller-driven StarRupture native acquisition collector.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"
#include "Services/Map/Buildings/IBuildingCollector.h"
#include <memory>

namespace SDK { class UWorld; }

namespace MiniMap::BuildingAcquisition
{
    /** Owns one acquisition session. Calls are serialized on the game thread.
     * Caller must Reset before world retirement and ShutdownResources before native
     * services disappear or destruction. This class registers nothing and has no singleton.
     */
    class BuildingCollectorV2 final : public ::Services::Map::Buildings::IBuildingCollector
    {
    public:
        /** Creates empty CPU state only; does not acquire native resources. */
        BuildingCollectorV2();
        /** Requires prior ShutdownResources; performs no implicit native shutdown. */
        ~BuildingCollectorV2() override;
        BuildingCollectorV2(const BuildingCollectorV2&) = delete;
        BuildingCollectorV2& operator=(const BuildingCollectorV2&) = delete;

        /** Roots per-instance acquisition metadata on the game thread; retries missing metadata. */
        bool InitializeResources();
        /** Accepts an exact finite positive request without excluding any entity; false leaves it unchanged. */
        bool SetAcquisitionRegion(const BuildingAcquiredFacts::Region& region) noexcept;
        /** Converts neutral map units directly to StarRupture native centimetres and applies the common validator. */
        bool SetAcquisitionRegion(
            const ::Services::Map::Buildings::BuildingCollectionRegion& region) noexcept override;
        /** Retains the current immutable candidate source, or returns null when unavailable or stale. */
        std::shared_ptr<const ::Services::Map::Buildings::IBuildingCandidatePublication>
            AcquireCandidatePublication() const override;
        /** Advances native acquisition using caller-supplied live-world facts.
         * Caller must keep world/native facilities alive throughout this call and notify
         * reentrant lifecycle changes through Reset or this operation before retirement.
         */
        void AdvanceAcquisition(SDK::UWorld* world, std::uint64_t worldGeneration, bool gameplayReady);
        /** Revokes currentness and clears query/session roots while native services remain live. */
        void Reset();
        /** Clears session then releases metadata/conversion roots; required before destruction. */
        void ShutdownResources();
        /** Returns immutable historical CPU values without advancing acquisition. */
        std::shared_ptr<const BuildingAcquiredFacts> GetFacts() const noexcept;
        /** Tests current eligibility separately from retained CPU lifetime, on the calling thread. */
        bool IsCurrent(const std::shared_ptr<const BuildingAcquiredFacts>& facts) const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
#endif
