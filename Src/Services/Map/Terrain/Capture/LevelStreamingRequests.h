// Copies real native per-level load request IDs across the genuine UE/generated SDK boundary.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <vector>
namespace SDK { class ULevelStreaming; }

namespace Services::Map::Terrain::Capture
{
    /** Synchronous accessor only; the SDK caller retains the streaming object throughout each call. */
    class LevelStreamingRequests final
    {
    public:
        /** Appends unique positive native IDs; returns true only for an unloaded terminal native state.
         * Does not flush loading, retain a callback, or imply that loaded levels/components have retired.
         */
        static bool Observe(const SDK::ULevelStreaming* level, std::vector<std::int32_t>& requests);
    private:
        /** Checks protected member offsets using a genuine derived type, never an instantiated replacement. */
        struct NativeLayout;
    };
}
#endif
