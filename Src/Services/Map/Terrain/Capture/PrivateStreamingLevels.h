// Tracks the union of private streaming levels; native retention belongs to the session's one inventory.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <vector>
#include <cstdint>
namespace SDK { class UWorld; class UObject; class ULevelStreaming; }

namespace Services::Map::Terrain::Capture
{
    /** Borrows levels retained in the session inventory; removed levels remain observable.
     * The session must Observe before/after native progression and immediately before partition unregistration.
     */
    class PrivateStreamingLevels final
    {
    public:
        /** Creates an empty inventory without engine access. */
        PrivateStreamingLevels() = default;
        /** Prevents copying borrowed tracking into another session. */
        PrivateStreamingLevels(const PrivateStreamingLevels&) = delete;
        /** Prevents overwriting retained objects. */
        PrivateStreamingLevels& operator=(const PrivateStreamingLevels&) = delete;
        /** Adds registered/considered/previously seen levels to the shared session-owned retention inventory. */
        bool Observe(SDK::UWorld* world, std::vector<SDK::UObject*>& nativeOwners);
        /** Copies positive IDs from every retained streaming object, including removed entries. */
        std::vector<std::int32_t> GetRequestIds() const;
        /** Requests private unload/removal without cancellation of any native package request. */
        void RequestUnload();
        /** Flushes only actual private IDs during explicit retirement; refuses suspended loading. */
        bool WaitForRequests();
        /** Requires empty IDs, terminal native states, no loaded/pending level and no streaming transitions. */
        bool IsUnloaded() const;
        /** Clears borrowed tracking only after retirement; the session balances the shared native references. */
        void ClearAfterRetirement();
    private:
        /** Admits one world-owned level and retains it before later callbacks can remove it from the world. */
        bool ObserveLevel(SDK::ULevelStreaming* level, SDK::UWorld* world,
            std::vector<SDK::UObject*>& nativeOwners);
        /** Adds one balanced native reference without duplicating ownership. */
        static void Retain(SDK::UObject* object, std::vector<SDK::UObject*>& nativeOwners);
        std::vector<SDK::ULevelStreaming*> levels_;
    };
}
#endif
