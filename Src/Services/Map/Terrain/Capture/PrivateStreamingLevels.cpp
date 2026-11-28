// Uses typed SDK links and the isolated genuine getter; the session retains every observed native owner.
#if defined(MODLOADER_CLIENT_BUILD)
#include "PrivateStreamingLevels.h"
#include "LevelStreamingRequests.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <algorithm>
#include <exception>

namespace Services::Map::Terrain::Capture
{
    /** Retains before native progression; vector allocation failure cannot lose an acquired native reference. */
    void PrivateStreamingLevels::Retain(SDK::UObject* object, std::vector<SDK::UObject*>& nativeOwners)
    {
        if (object && std::find(nativeOwners.begin(), nativeOwners.end(), object) == nativeOwners.end())
        {
            nativeOwners.push_back(object);
            MiniMapNative::Get()->asset.retainObject(object);
        }
    }

    /** Captures both level references before the engine can move LoadedLevel to PendingUnloadLevel. */
    bool PrivateStreamingLevels::ObserveLevel(SDK::ULevelStreaming* level, SDK::UWorld* world,
        std::vector<SDK::UObject*>& nativeOwners)
    {
        if (!level)
            return true;
        if (level->Outer != world)
            return false;
        Retain(level, nativeOwners);
        if (std::find(levels_.begin(), levels_.end(), level) == levels_.end())
            levels_.push_back(level);
        Retain(level->LoadedLevel, nativeOwners);
        Retain(level->PendingUnloadLevel, nativeOwners);
        return (!level->LoadedLevel || level->LoadedLevel->OwningWorld == world) &&
            (!level->PendingUnloadLevel || level->PendingUnloadLevel->OwningWorld == world);
    }

    /** Uses the actual SDK-exposed consideration array, never its padding or an assumed native offset. */
    bool PrivateStreamingLevels::Observe(SDK::UWorld* world, std::vector<SDK::UObject*>& nativeOwners)
    {
        const auto* api = MiniMapNative::Get();
        if (!world || !api || !api->player.isGameThread() || api->world.isGarbageCollectingAndLocking())
            return false;
        for (auto* level : world->StreamingLevels)
        {
            if (!ObserveLevel(level, world, nativeOwners))
                return false;
        }
        for (auto* level : world->StreamingLevelsToConsider.StreamingLevels)
        {
            if (!ObserveLevel(level, world, nativeOwners))
                return false;
        }
        for (auto* level : levels_)
        {
            if (!ObserveLevel(level, world, nativeOwners))
                return false;
        }
        return true;
    }

    /** Copies from the full retained union so unregistration cannot erase the only observation path. */
    std::vector<std::int32_t> PrivateStreamingLevels::GetRequestIds() const
    {
        std::vector<std::int32_t> requests;
        for (auto* level : levels_)
            LevelStreamingRequests::Observe(level, requests);
        return requests;
    }

    /** Withdraws private level demand using normal native setters; the root owner unregisters its source first. */
    void PrivateStreamingLevels::RequestUnload()
    {
        for (auto* level : levels_)
        {
            level->SetShouldBeVisible(false);
            level->SetShouldBeLoaded(false);
            level->SetIsRequestingUnloadAndRemoval(true);
        }
    }

    /** A retirement-only wait services genuine request IDs and then re-reads the retained native containers. */
    bool PrivateStreamingLevels::WaitForRequests()
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->player.isGameThread() || !api->packageLoad.IsComplete())
            return false;
        const auto requests = GetRequestIds();
        if (requests.empty())
            return true;
        const auto suspended = *api->packageLoad.loadingSuspended;
        if (!suspended || suspended())
            return false;
        for (auto request : requests)
            api->packageLoad.flushRequest(request);
        return GetRequestIds().empty();
    }

    /** Checks transitions separately from package completion; this is still not a render-resource acknowledgement. */
    bool PrivateStreamingLevels::IsUnloaded() const
    {
        std::vector<std::int32_t> requests;
        for (auto* level : levels_)
        {
            if (!LevelStreamingRequests::Observe(level, requests) || level->LoadedLevel ||
                level->PendingUnloadLevel || level->IsStreamingStatePending())
                return false;
        }
        return requests.empty();
    }

    /** Clears only borrowed observations; the session releases all native owners from its single inventory. */
    void PrivateStreamingLevels::ClearAfterRetirement()
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->player.isGameThread() || !IsUnloaded())
            std::terminate();
        levels_.clear();
    }
}
#endif
