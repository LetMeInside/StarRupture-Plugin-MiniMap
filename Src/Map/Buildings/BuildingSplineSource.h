// Declares raw spline fragment/payload copying with no geometry preparation.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"

namespace SDK { struct FAuSplineConnectionFragment; struct FAuAPPayloadDataHandle; }

namespace MiniMap::BuildingAcquisition
{
    /** Synchronous game-thread copier; no native borrow or allocation escapes in output. */
    class BuildingSplineSource final
    {
    public:
        /** Copies a Mass fragment before any payload getter can reenter lifecycle code. */
        static BuildingAcquiredFacts::Spline CopyFragment(const SDK::FAuSplineConnectionFragment& fragment);
        /** Copies a temporary payload into existing owned facts; caller supplies a scoped handle copy.
         * The handle itself remains borrowed and must not survive the getter call.
         */
        static void CopyFallback(const SDK::FAuAPPayloadDataHandle& handle, BuildingAcquiredFacts::Spline& facts);
        /** Matches existing fallback eligibility; malformed initialized/multi-point source is never replaced. */
        static bool NeedsFallback(const BuildingAcquiredFacts::Spline& facts) noexcept;

    private:
        struct Copier;
    };
}
#endif
