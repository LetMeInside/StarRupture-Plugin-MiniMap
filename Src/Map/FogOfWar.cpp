#if defined(MODLOADER_CLIENT_BUILD)

#include "FogOfWar.h"

#include "Map.h"
#include "MapTransform.h"

#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
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

    constexpr double kMapPivotX =
        -480000.0;

    constexpr double kMapPivotY =
        -380000.0;

    constexpr double kWorldToMaskScale =
        0.00128;

    constexpr double kMaskCellWorldUnits =
        1.0 /
        kWorldToMaskScale;

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

    bool g_renderReported =
        false;

    float g_snapshotAccumulator =
        0.0f;

    Snapshot g_snapshot = {};


    void ClearSnapshot()
    {
        g_snapshot = {};
        g_invalidStateReported =
            false;
        g_renderReported =
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


    bool TryGetMaskByte(
        int32_t maskX,
        int32_t maskY,
        uint8_t& outValue)
    {
        outValue = 0;

        if (!g_snapshot.Valid ||
            maskX < 0 ||
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

        outSegmentIndex =
            segmentVertical *
            kSegmentGridSize +
            segmentHorizontal;

        outMaskX =
            maskX;

        outMaskY =
            maskY;

        return TryGetMaskByte(
            maskX,
            maskY,
            outValue);
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


    uint32_t FogColorForMaskByte(
        uint8_t maskByte)
    {
        const uint32_t alpha =
            255u -
            static_cast<uint32_t>(
                maskByte);

        // ImGui packed color is AABBGGRR. RGB remains black.
        return alpha << 24;
    }


    bool DrawFogRun(
        IModLoaderImGui* ui,
        PluginDrawList drawList,
        const MiniMapMap::Transform& transform,
        int32_t startMaskX,
        int32_t endMaskXExclusive,
        int32_t maskY,
        uint8_t maskByte)
    {
        if (ui == nullptr ||
            startMaskX >=
            endMaskXExclusive ||
            maskByte ==
            255)
        {
            return false;
        }

        const double worldX0 =
            kMapPivotX +
            static_cast<double>(
                startMaskX) *
            kMaskCellWorldUnits;

        const double worldX1 =
            kMapPivotX +
            static_cast<double>(
                endMaskXExclusive) *
            kMaskCellWorldUnits;

        const double worldY0 =
            kMapPivotY +
            static_cast<double>(
                maskY) *
            kMaskCellWorldUnits;

        const double worldY1 =
            worldY0 +
            kMaskCellWorldUnits;

        MiniMapMap::ScreenPoint p1 = {};
        MiniMapMap::ScreenPoint p2 = {};
        MiniMapMap::ScreenPoint p3 = {};
        MiniMapMap::ScreenPoint p4 = {};

        if (!transform.WorldToScreen(
                worldX0,
                worldY0,
                p1) ||
            !transform.WorldToScreen(
                worldX1,
                worldY0,
                p2) ||
            !transform.WorldToScreen(
                worldX1,
                worldY1,
                p3) ||
            !transform.WorldToScreen(
                worldX0,
                worldY1,
                p4))
        {
            return false;
        }

        ui->DL_AddQuadFilled(
            drawList,
            p1.X,
            p1.Y,
            p2.X,
            p2.Y,
            p3.X,
            p3.Y,
            p4.X,
            p4.Y,
            FogColorForMaskByte(
                maskByte));

        return true;
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


    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform)
    {
        if (ui == nullptr ||
            !transform.Valid ||
            !g_snapshot.Valid ||
            transform.PixelsPerWorldUnit <=
                0.0)
        {
            return;
        }

        PluginDrawList drawList =
            ui->GetWindowDrawList();

        if (drawList ==
            nullptr)
        {
            return;
        }

        const double halfWidthWorld =
            static_cast<double>(
                transform.Width) *
            0.5 /
            transform.PixelsPerWorldUnit;

        const double halfHeightWorld =
            static_cast<double>(
                transform.Height) *
            0.5 /
            transform.PixelsPerWorldUnit;

        const double viewportRadiusWorld =
            std::sqrt(
                halfWidthWorld *
                    halfWidthWorld +
                halfHeightWorld *
                    halfHeightWorld);

        if (!std::isfinite(
            viewportRadiusWorld))
        {
            return;
        }

        const double minWorldX =
            transform.PlayerWorldX -
            viewportRadiusWorld;

        const double maxWorldX =
            transform.PlayerWorldX +
            viewportRadiusWorld;

        const double minWorldY =
            transform.PlayerWorldY -
            viewportRadiusWorld;

        const double maxWorldY =
            transform.PlayerWorldY +
            viewportRadiusWorld;

        const int32_t minMaskX =
            std::clamp(
                static_cast<int32_t>(
                    std::floor(
                        (minWorldX -
                            kMapPivotX) *
                        kWorldToMaskScale)),
                0,
                kLogicalMaskSize -
                    1);

        const int32_t maxMaskX =
            std::clamp(
                static_cast<int32_t>(
                    std::floor(
                        (maxWorldX -
                            kMapPivotX) *
                        kWorldToMaskScale)),
                0,
                kLogicalMaskSize -
                    1);

        const int32_t minMaskY =
            std::clamp(
                static_cast<int32_t>(
                    std::floor(
                        (minWorldY -
                            kMapPivotY) *
                        kWorldToMaskScale)),
                0,
                kLogicalMaskSize -
                    1);

        const int32_t maxMaskY =
            std::clamp(
                static_cast<int32_t>(
                    std::floor(
                        (maxWorldY -
                            kMapPivotY) *
                        kWorldToMaskScale)),
                0,
                kLogicalMaskSize -
                    1);

        if (minMaskX >
                maxMaskX ||
            minMaskY >
                maxMaskY)
        {
            return;
        }

        const float viewportX =
            transform.CenterX -
            transform.Width *
            0.5f;

        const float viewportY =
            transform.CenterY -
            transform.Height *
            0.5f;

        ui->DL_PushClipRect(
            drawList,
            viewportX,
            viewportY,
            viewportX +
                transform.Width,
            viewportY +
                transform.Height,
            true);

        std::size_t runCount =
            0;

        for (int32_t maskY = minMaskY;
            maskY <= maxMaskY;
            ++maskY)
        {
            int32_t maskX =
                minMaskX;

            while (maskX <=
                maxMaskX)
            {
                uint8_t maskByte =
                    255;

                if (!TryGetMaskByte(
                    maskX,
                    maskY,
                    maskByte))
                {
                    ++maskX;
                    continue;
                }

                if (maskByte ==
                    255)
                {
                    ++maskX;
                    continue;
                }

                const int32_t runStart =
                    maskX;

                const uint8_t runMaskByte =
                    maskByte;

                ++maskX;

                while (maskX <=
                    maxMaskX)
                {
                    uint8_t nextMaskByte =
                        255;

                    if (!TryGetMaskByte(
                            maskX,
                            maskY,
                            nextMaskByte) ||
                        nextMaskByte !=
                            runMaskByte)
                    {
                        break;
                    }

                    ++maskX;
                }

                if (DrawFogRun(
                    ui,
                    drawList,
                    transform,
                    runStart,
                    maskX,
                    maskY,
                    runMaskByte))
                {
                    ++runCount;
                }
            }
        }

        ui->DL_PopClipRect(
            drawList);

        if (!g_renderReported)
        {
            LOG_INFO(
                "MiniMap: FOW geometry renderer active "
                "visible-mask=(%d..%d, %d..%d) runs=%zu",
                minMaskX,
                maxMaskX,
                minMaskY,
                maxMaskY,
                runCount);

            g_renderReported =
                true;
        }
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