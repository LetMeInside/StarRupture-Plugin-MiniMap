// Declares raw spline fragment/payload copying with no geometry preparation.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingAcquiredFacts.h"
#include <cstdint>

namespace SDK { struct FAuSplineConnectionFragment; struct FAuAPPayloadDataHandle; }

namespace MiniMap::BuildingAcquisition
{
    /** Validated offsets for the consumed StarRupture spline and Mass members. */
    struct RuntimeFieldContract
    {
        int32_t ActorPlacementPayload = -1;
        int32_t SplineFragmentData = -1;
        int32_t SplineFragmentCurves = -1;
        int32_t SplineFragmentCurvesInitialized = -1;
        int32_t BuildingParametersPlacementData = -1;
        int32_t SplineDataPoints = -1;
        int32_t SplineDataLocation = -1;
        int32_t SplineDataRotation = -1;
        int32_t SplineDataRealLength = -1;
    };

    /** Synchronous game-thread copier; no native borrow or allocation escapes in output. */
    class BuildingSplineSource final
    {
    public:
        /** Clears the complete StarRupture field bundle before a new admission attempt. */
        static void ResetRuntimeFieldContract() noexcept;
        /** Publishes all validated StarRupture field offsets as one immutable bundle. */
        static bool PublishRuntimeFieldContract(const RuntimeFieldContract& contract) noexcept;
        /** Copies the published field bundle once during collector initialization. */
        static bool CopyRuntimeFieldContract(RuntimeFieldContract& output) noexcept;
        /** Publishes point field offsets and the reflected runtime element stride. */
        static void SetPointArrayContract(int32_t indexOffset, int32_t positionOffset,
            int32_t tangentOffset, uint32_t elementSize);
        /** Copies a Mass fragment before any payload getter can reenter lifecycle code. */
        static BuildingAcquiredFacts::Spline CopyFragment(
            const SDK::FAuSplineConnectionFragment& fragment,
            const RuntimeFieldContract& contract);
        /** Copies a temporary payload into existing owned facts; caller supplies a scoped handle copy.
         * The handle itself remains borrowed and must not survive the getter call.
         */
        static void CopyFallback(
            const SDK::FAuAPPayloadDataHandle& handle,
            const RuntimeFieldContract& contract,
            BuildingAcquiredFacts::Spline& facts);
        /** Matches existing fallback eligibility; malformed initialized/multi-point source is never replaced. */
        static bool NeedsFallback(const BuildingAcquiredFacts::Spline& facts) noexcept;

    private:
        struct Copier;
    };
}
#endif
