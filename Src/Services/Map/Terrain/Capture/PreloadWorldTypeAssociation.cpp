// Mutates the genuine native TMap<FName,EWorldType::Type> using the engine's hashing and allocators.
// No generated game SDK implementation header may enter this translation unit.
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/PackageLoadUEConfiguration.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Containers/Map.h"
#include "PreloadWorldTypeAssociation.h"
#include "PackageLoadOperation.h"
#include "Native/NativeApi.h"
#include <exception>
#include <cstdlib>

using Services::Map::Terrain::Capture::PackageLoadOperation;

/** Supplies the real native name-pool hashing operation required by genuine FName map keys. */
uint32 GetTypeHash(FNameEntryId entry)
{
    return PackageLoadOperation::RequireNativeBindings().hashNameEntry(entry);
}

/** Preserves the engine's invalid-set failure path for genuine preload map allocation. */
void UE::Core::Private::OnInvalidSetNum(unsigned long long count)
{
    PackageLoadOperation::RequireNativeBindings().invalidSetCount(count);
    std::abort();
}

namespace Services::Map::Terrain::Capture
{
    /** Holds an exact native key and the exclusive insertion claim; it owns no world or request. */
    struct PreloadWorldTypeAssociation::State final
    {
        using PreloadMap = TMap<FName, EWorldType::Type>;
        static_assert(sizeof(PreloadMap) == 80 && alignof(PreloadMap) == 8);
        static_assert(sizeof(EWorldType::Type) == 4 && EWorldType::GamePreview == 5);
        static_assert(sizeof(FName) == 8 && WITH_CASE_PRESERVING_NAME == 0);
        static_assert(UE_FNAME_OUTLINE_NUMBER == 0 && UE_ENABLE_ARRAY_SLACK_TRACKING == 0);
        FName Destination;
        bool Owned = false;

        /** Requires native game-thread mutation outside collection; returns no container on rejection. */
        static PreloadMap* GetMap()
        {
            const auto* api = MiniMapNative::Get();
            if (!api || !api->packageLoad.IsComplete() || !api->player.isGameThread() ||
                api->world.isGarbageCollectingAndLocking())
                return nullptr;
            return static_cast<PreloadMap*>(api->packageLoad.worldTypePreloadMap);
        }
    };

    /** Allocates plugin-only ownership metadata, not a second native map. */
    PreloadWorldTypeAssociation::PreloadWorldTypeAssociation() : state_(std::make_unique<State>()) {}

    /** Makes dropping an entry with a potentially live native reader a lifecycle violation. */
    PreloadWorldTypeAssociation::~PreloadWorldTypeAssociation()
    {
        if (state_->Owned)
            std::terminate();
    }

    /** Inserts a GamePreview value only after absence has been checked under native game-thread serialization. */
    bool PreloadWorldTypeAssociation::Acquire(std::int32_t comparisonIndex, std::uint32_t number)
    {
        auto* map = State::GetMap();
        if (!map || state_->Owned || comparisonIndex <= 0)
            return false;
        const FName destination = FName::CreateFromDisplayId(FNameEntryId::FromUnstableInt(comparisonIndex), number);
        if (map->Contains(destination))
            return false;
        map->Add(destination, EWorldType::GamePreview);
        state_->Destination = destination;
        state_->Owned = true;
        return true;
    }

    /** Preserves foreign entries on any ownership inconsistency instead of blindly removing a key. */
    bool PreloadWorldTypeAssociation::Release()
    {
        if (!state_->Owned)
            return true;
        auto* map = State::GetMap();
        if (!map)
            return false;
        const auto* type = map->Find(state_->Destination);
        if (!type || *type != EWorldType::GamePreview || map->Remove(state_->Destination) != 1)
            return false;
        state_->Owned = false;
        return true;
    }
}
#endif
