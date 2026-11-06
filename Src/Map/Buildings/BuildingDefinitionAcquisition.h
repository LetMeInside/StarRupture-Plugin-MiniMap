// Declares per-instance authored definition copying and native root ownership.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"
#include <memory>

namespace SDK { class UAuActorPlacementData; }

namespace MiniMap::BuildingAcquisition
{
    /** Copies native definition facts without profiles. Collector owns the sole definition cache. */
    class BuildingDefinitionAcquisition final
    {
    public:
        /** A rooted placement cached by the collector; local strong owners survive reentry.
         * Must be released on the game thread before native API shutdown.
         */
        struct DefinitionOwner
        {
            SDK::UAuActorPlacementData* Root = nullptr;
            std::shared_ptr<const BuildingAcquiredFacts::Definition> Facts;
            DefinitionOwner() = default;
            DefinitionOwner(const DefinitionOwner&) = delete;
            DefinitionOwner& operator=(const DefinitionOwner&) = delete;
            /** Releases only this instance's native placement root. */
            ~DefinitionOwner();
        };

        /** Creates uninitialized metadata state without native access. */
        BuildingDefinitionAcquisition();
        /** Requires explicit Shutdown before destruction. */
        ~BuildingDefinitionAcquisition();
        BuildingDefinitionAcquisition(const BuildingDefinitionAcquisition&) = delete;
        BuildingDefinitionAcquisition& operator=(const BuildingDefinitionAcquisition&) = delete;
        /** Acquires this instance's conversion metadata with partial-root rollback. */
        bool Initialize();
        /** Releases this instance's metadata after collector session users have retired. */
        void Shutdown();
        /** Retains a placement once; caller inserts the returned owner into its one cache. */
        std::shared_ptr<DefinitionOwner> Retain(SDK::UAuActorPlacementData* placement) const;
        /** Copies authored facts; local root/metadata owners survive reflection reentry. */
        std::shared_ptr<const BuildingAcquiredFacts::Definition> Copy(
            const std::shared_ptr<DefinitionOwner>& definition) const;
        /** Reports whether every required native definition value was acquired. */
        static bool IsComplete(const BuildingAcquiredFacts::Definition& facts) noexcept;

    private:
        struct Metadata;
        std::shared_ptr<Metadata> metadata_;
    };
}
#endif
