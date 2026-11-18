// Declares per-instance authored definition copying and native root ownership.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"
#include <cstdint>
#include <memory>

namespace SDK { class UAuActorPlacementData; }

namespace MiniMap::BuildingAcquisition
{
    /** Validated Client offsets for StarRupture-owned definition properties. */
    struct RuntimeFieldOffsets
    {
        int32_t BuildingId;
        int32_t ActorClass;
        int32_t EntityType;
        int32_t EntityConfig;
        int32_t EntityConfigPtr;
        int32_t Tags;
        int32_t HighResTemplateActor;
        int32_t LowResTemplateActor;
        int32_t WalkwaySplineMesh;
    };

    /** Copies core definition and authored profile facts using the collector's sole definition cache. */
    class BuildingDefinitionAcquisition final
    {
    public:
        /** Clears adapter-published offsets before a new scanner admission attempt. */
        static void ResetRuntimeFieldOffsets() noexcept;
        /** Publishes one complete validated offset set after Client scanner admission. */
        static bool PublishRuntimeFieldOffsets(const RuntimeFieldOffsets& offsets) noexcept;
        /** Copies the current admitted offsets for one acquisition metadata instance. */
        static bool CopyRuntimeFieldOffsets(RuntimeFieldOffsets& output) noexcept;

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
        /** Acquires profile alternatives from already rooted definition references and returns immutable CPU facts.
         * The caller retains the DefinitionOwner across this potentially reentrant asset operation and validates
         * its world/session context before storing the returned facts.
         */
        std::shared_ptr<const BuildingAcquiredFacts::Definition> CopySplineProfiles(
            const std::shared_ptr<DefinitionOwner>& definition) const;
        /** Reports whether the core class, Interior and optional config-reference facts are acquired. */
        static bool IsComplete(const BuildingAcquiredFacts::Definition& facts) noexcept;

    private:
        struct Metadata;
        std::shared_ptr<Metadata> metadata_;
    };
}
#endif
