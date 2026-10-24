#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

namespace SDK { struct FAuAPPayloadDataHandle; }
namespace MiniMapBuildingCollector { struct SplineGeometry; }

namespace MiniMapBuildingSplinePayload
{
    // Return values of one copy operation, not entity acquisition/lifecycle states.
    enum class CopyResult { Complete, NotReady, UnsupportedPointCount, InvalidData, Unavailable, WrongThread };

    // Borrow an engine-owned handle only for this synchronous game-thread call.
    // Output is replaced (and contains no segments on failure). No native pointer
    // or allocation escapes. STL allocation failures propagate after native cleanup.
    CopyResult Copy(const SDK::FAuAPPayloadDataHandle& handle,
        MiniMapBuildingCollector::SplineGeometry& output);
}
#endif
