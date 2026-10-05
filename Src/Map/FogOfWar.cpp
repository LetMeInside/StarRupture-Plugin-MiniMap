#if defined(MODLOADER_CLIENT_BUILD)

#include "FogOfWar.h"

#include "Map.h"

#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    constexpr float kSnapshotIntervalSeconds =
        0.5f;

    constexpr int32_t kSegmentGridSize =
        6;

    constexpr int32_t kSegmentSize =
        128;

    constexpr int32_t kSegmentCount =
        kSegmentGridSize *
        kSegmentGridSize;

    constexpr int32_t kSegmentByteCount =
        kSegmentSize *
        kSegmentSize;

    constexpr int32_t kLogicalMaskSize =
        kSegmentGridSize *
        kSegmentSize;

    // HF2.5-CL-126119 cooked MapAreaPivotPoint.
    // These are diagnostic mapping constants only. The permanent renderer
    // should obtain the active settings from the loaded map-settings CDO.
    constexpr double kMapPivotX =
        -480000.0;

    constexpr double kMapPivotY =
        -380000.0;

    // 128 mask texels per 100000 world units.
    constexpr double kWorldToMaskScale =
        0.00128;

    // PDB-established native offsets for HF2.5-CL-126119.
    constexpr std::ptrdiff_t kFOWDataOffset =
        0x100;

    constexpr std::size_t kFogSegmentNativeSize =
        0x10;

    struct RawTArray
    {
        const void* Data = nullptr;
        int32_t Num = 0;
        int32_t Max = 0;
    };

    static_assert(
        sizeof(RawTArray) ==
        0x10);

    struct Snapshot
    {
        SDK::UCrPlayerMapMenuDataComponent* Component =
            nullptr;

        std::array<std::vector<uint8_t>, kSegmentCount>
            Segments = {};

        bool Valid = false;
    };

    IPluginSelf* g_self =
        nullptr;

    bool g_initialized =
        false;

    bool g_tickRegistered =
        false;

    bool g_invalidStateReported =
        false;

    float g_snapshotAccumulator =
        0.0f;

    Snapshot g_snapshot = {};


    void ClearSnapshot()
    {
        g_snapshot = {};
        g_invalidStateReported =
            false;
    }


    bool TryGetLocalPlayerAndComponent(
        SDK::ACrCharacterPlayerBase*& outPlayer,
        SDK::UCrPlayerMapMenuDataComponent*& outComponent)
    {
        outPlayer = nullptr;
        outComponent = nullptr;

        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        if (world == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->player.getFirstPlayerController == nullptr ||
            native->player.getPlayerPawn == nullptr)
        {
            return false;
        }

        SDK::APlayerController* controller =
            native->player.getFirstPlayerController(
                world);

        if (controller == nullptr)
        {
            return false;
        }

        SDK::ACrCharacterPlayerBase* player =
            native->player.getPlayerPawn(
                static_cast<const SDK::AController*>(
                    controller));

        if (player == nullptr)
        {
            return false;
        }

        SDK::UCrPlayerMapMenuDataComponent* component =
            player->PlayerMapMenuDataComponent;

        if (component == nullptr)
        {
            return false;
        }

        outPlayer =
            player;

        outComponent =
            component;

        return true;
    }


    bool ValidateRawArray(
        const RawTArray& array,
        int32_t expectedNum)
    {
        if (array.Num !=
            expectedNum ||
            array.Max <
            array.Num ||
            array.Max <
            0)
        {
            return false;
        }

        if (array.Num >
            0 &&
            array.Data ==
            nullptr)
        {
            return false;
        }

        return true;
    }


    bool ReadSnapshot(
        SDK::UCrPlayerMapMenuDataComponent* component,
        Snapshot& outSnapshot)
    {
        outSnapshot = {};

        if (component ==
            nullptr)
        {
            return false;
        }

        const auto* componentBytes =
            reinterpret_cast<const uint8_t*>(
                component);

        RawTArray outer = {};

        std::memcpy(
            &outer,
            componentBytes +
                kFOWDataOffset,
            sizeof(outer));

        if (!ValidateRawArray(
            outer,
            kSegmentCount))
        {
            return false;
        }

        const auto* segmentBytes =
            static_cast<const uint8_t*>(
                outer.Data);

        for (int32_t segmentIndex = 0;
            segmentIndex < kSegmentCount;
            ++segmentIndex)
        {
            RawTArray segment = {};

            std::memcpy(
                &segment,
                segmentBytes +
                    static_cast<std::size_t>(
                        segmentIndex) *
                    kFogSegmentNativeSize,
                sizeof(segment));

            if (!ValidateRawArray(
                segment,
                kSegmentByteCount))
            {
                return false;
            }

            const auto* sourceBytes =
                static_cast<const uint8_t*>(
                    segment.Data);

            outSnapshot.Segments[
                static_cast<std::size_t>(
                    segmentIndex)].
                assign(
                    sourceBytes,
                    sourceBytes +
                        kSegmentByteCount);
        }

        outSnapshot.Component =
            component;

        outSnapshot.Valid =
            true;

        return true;
    }


    bool TrySamplePlayerMask(
        uint8_t& outValue,
        int32_t& outMaskX,
        int32_t& outMaskY,
        int32_t& outSegmentIndex)
    {
        outValue = 0;
        outMaskX = -1;
        outMaskY = -1;
        outSegmentIndex = -1;

        if (!g_snapshot.Valid)
        {
            return false;
        }

        MiniMapMap::PlayerPose pose = {};

        if (!MiniMapMap::TryGetPlayerPose(
            pose))
        {
            return false;
        }

        const double maskXDouble =
            (pose.WorldX -
                kMapPivotX) *
            kWorldToMaskScale;

        const double maskYDouble =
            (pose.WorldY -
                kMapPivotY) *
            kWorldToMaskScale;

        if (!std::isfinite(maskXDouble) ||
            !std::isfinite(maskYDouble))
        {
            return false;
        }

        // Native conversion truncates toward zero.
        const int32_t maskX =
            static_cast<int32_t>(
                maskXDouble);

        const int32_t maskY =
            static_cast<int32_t>(
                maskYDouble);

        if (maskX < 0 ||
            maskY < 0 ||
            maskX >= kLogicalMaskSize ||
            maskY >= kLogicalMaskSize)
        {
            return false;
        }

        const int32_t segmentHorizontal =
            maskX /
            kSegmentSize;

        const int32_t segmentVertical =
            maskY /
            kSegmentSize;

        const int32_t segmentIndex =
            segmentVertical *
            kSegmentGridSize +
            segmentHorizontal;

        const int32_t localX =
            maskX -
            segmentHorizontal *
            kSegmentSize;

        const int32_t localY =
            maskY -
            segmentVertical *
            kSegmentSize;

        const int32_t byteIndex =
            localY *
            kSegmentSize +
            localX;

        const auto& segment =
            g_snapshot.Segments[
                static_cast<std::size_t>(
                    segmentIndex)];

        if (segment.size() !=
            static_cast<std::size_t>(
                kSegmentByteCount))
        {
            return false;
        }

        outValue =
            segment[
                static_cast<std::size_t>(
                    byteIndex)];

        outMaskX =
            maskX;

        outMaskY =
            maskY;

        outSegmentIndex =
            segmentIndex;

        return true;
    }


    void LogPlayerSample(
        const char* prefix)
    {
        uint8_t value = 0;
        int32_t maskX = -1;
        int32_t maskY = -1;
        int32_t segmentIndex = -1;

        if (TrySamplePlayerMask(
            value,
            maskX,
            maskY,
            segmentIndex))
        {
            LOG_INFO(
                "MiniMap: FOW diagnostic: %s "
                "player mask=(%d, %d) segment=%d byte=%u",
                prefix,
                maskX,
                maskY,
                segmentIndex,
                static_cast<unsigned int>(
                    value));
        }
        else
        {
            LOG_INFO(
                "MiniMap: FOW diagnostic: %s "
                "player mask sample unavailable",
                prefix);
        }
    }


    void PollFogOfWar()
    {
        SDK::ACrCharacterPlayerBase* player =
            nullptr;

        SDK::UCrPlayerMapMenuDataComponent* component =
            nullptr;

        if (!TryGetLocalPlayerAndComponent(
            player,
            component))
        {
            return;
        }

        (void)player;

        if (g_snapshot.Component !=
            component)
        {
            ClearSnapshot();
        }

        Snapshot nextSnapshot = {};

        if (!ReadSnapshot(
            component,
            nextSnapshot))
        {
            if (!g_invalidStateReported)
            {
                LOG_WARN(
                    "MiniMap: FOW diagnostic: "
                    "local FOWData is not yet a validated "
                    "36 x 16384-byte mask");

                g_invalidStateReported =
                    true;
            }

            return;
        }

        g_invalidStateReported =
            false;

        if (!g_snapshot.Valid)
        {
            g_snapshot =
                std::move(
                    nextSnapshot);

            LOG_INFO(
                "MiniMap: FOW diagnostic: "
                "snapshot ready component=%p "
                "segments=%d bytes-per-segment=%d total=%d",
                g_snapshot.Component,
                kSegmentCount,
                kSegmentByteCount,
                kSegmentCount *
                    kSegmentByteCount);

            LogPlayerSample(
                "initial");

            return;
        }

        std::size_t changedSegments =
            0;

        std::size_t changedBytes =
            0;

        for (std::size_t segmentIndex = 0;
            segmentIndex <
                g_snapshot.Segments.size();
            ++segmentIndex)
        {
            const auto& previous =
                g_snapshot.Segments[
                    segmentIndex];

            const auto& current =
                nextSnapshot.Segments[
                    segmentIndex];

            std::size_t segmentChangedBytes =
                0;

            for (std::size_t byteIndex = 0;
                byteIndex <
                    current.size();
                ++byteIndex)
            {
                if (previous[
                        byteIndex] !=
                    current[
                        byteIndex])
                {
                    ++segmentChangedBytes;
                }
            }

            if (segmentChangedBytes >
                0)
            {
                ++changedSegments;

                changedBytes +=
                    segmentChangedBytes;
            }
        }

        g_snapshot =
            std::move(
                nextSnapshot);

        if (changedSegments >
            0)
        {
            LOG_INFO(
                "MiniMap: FOW diagnostic: "
                "changed segments=%zu changed bytes=%zu",
                changedSegments,
                changedBytes);

            LogPlayerSample(
                "after update");
        }
    }


    void OnTick(
        float deltaSeconds)
    {
        if (!MiniMapMap::HasWorld())
        {
            return;
        }

        g_snapshotAccumulator +=
            deltaSeconds;

        if (g_snapshotAccumulator <
            kSnapshotIntervalSeconds)
        {
            return;
        }

        g_snapshotAccumulator =
            0.0f;

        PollFogOfWar();
    }
}


namespace MiniMapFogOfWar
{
    bool Initialize(
        IPluginSelf* self)
    {
        if (g_initialized)
        {
            return true;
        }

        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->Engine == nullptr)
        {
            LOG_ERROR(
                "MiniMap: FOW diagnostic initialization failed: "
                "engine hook is unavailable");

            return false;
        }

        g_self =
            self;

        ClearSnapshot();

        g_snapshotAccumulator =
            kSnapshotIntervalSeconds;

        self->hooks->Engine->RegisterOnTick(
            &OnTick);

        g_tickRegistered =
            true;

        g_initialized =
            true;

        LOG_INFO(
            "MiniMap: FOW diagnostic initialized");

        return true;
    }


    void Reset()
    {
        ClearSnapshot();

        g_snapshotAccumulator =
            kSnapshotIntervalSeconds;
    }


    void Shutdown()
    {
        if (!g_initialized)
        {
            return;
        }

        if (g_tickRegistered &&
            g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->Engine != nullptr)
        {
            g_self->hooks->Engine->UnregisterOnTick(
                &OnTick);
        }

        g_tickRegistered =
            false;

        ClearSnapshot();

        g_snapshotAccumulator =
            0.0f;

        g_self =
            nullptr;

        g_initialized =
            false;

        LOG_INFO(
            "MiniMap: FOW diagnostic shut down");
    }
}

#endif