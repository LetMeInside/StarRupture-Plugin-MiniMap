// Accesses the genuine inline ULevelStreaming getter with checked delegate and cooked object configuration.
// No generated game SDK implementation header is included here.
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/PackageLoadUEConfiguration.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Engine/LevelStreaming.h"
#include "LevelStreamingRequests.h"
#include "Native/NativeApi.h"
#include <algorithm>
#include <cstdlib>

namespace Services::Map::Terrain::Capture
{
    /** Exposes compile-time layout evidence only; no object of this derived type is constructed. */
    struct LevelStreamingRequests::NativeLayout final : ULevelStreaming
    {
        /** Matches CL-130045 type metadata before permitting genuine member access. */
        static void Verify()
        {
            static_assert(sizeof(ULevelStreaming) == 496 && alignof(ULevelStreaming) == 16);
            static_assert(offsetof(NativeLayout, AsyncRequestIDs) == 264);
            static_assert(offsetof(NativeLayout, LoadedLevel) == 424);
            static_assert(offsetof(NativeLayout, PendingUnloadLevel) == 432);
            static_assert(sizeof(TArray<int32>) == 16 && alignof(TArray<int32>) == 8);
            static_assert(UE_WITH_REMOTE_OBJECT_HANDLE == 0 && WITH_EDITORONLY_DATA == 0);
            static_assert(UE_ENABLE_ARRAY_SLACK_TRACKING == 0);
        }
    };

    /** Reads native IDs directly through the genuine getter; callers serialize against loader advancement. */
    bool LevelStreamingRequests::Observe(const SDK::ULevelStreaming* level,
        std::vector<std::int32_t>& requests)
    {
        NativeLayout::Verify();
        const auto* api = MiniMapNative::Get();
        if (!level || !api || !api->player.isGameThread() || api->world.isGarbageCollectingAndLocking())
            std::abort();
        const auto* nativeLevel = reinterpret_cast<const ULevelStreaming*>(level);
        for (int32 request : nativeLevel->GetAsyncRequestIDs())
        {
            if (request > 0 && std::find(requests.begin(), requests.end(), request) == requests.end())
                requests.push_back(request);
        }
        const auto state = nativeLevel->GetLevelStreamingState();
        return state == ELevelStreamingState::Removed || state == ELevelStreamingState::Unloaded ||
            state == ELevelStreamingState::FailedToLoad;
    }
}
#endif
