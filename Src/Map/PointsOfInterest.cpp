#if defined(MODLOADER_CLIENT_BUILD)

#include "PointsOfInterest.h"

#include "FogOfWar.h"
#include "Map.h"
#include "MapTransform.h"

#include "../Native/NativeApi.h"
#include "../Native/TextureAccess.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr float kSnapshotIntervalSeconds =
        0.5f;

    constexpr float kDefaultIconSizePixels =
        32.0f;

    constexpr float kMinimumIconSizePixels =
        16.0f;

    constexpr float kMaximumIconSizePixels =
        48.0f;

    constexpr double kDefaultPixelsPerWorldUnit =
        0.03;

    constexpr uint8_t kStateHidden =
        0;

    constexpr uint8_t kStateVisibleUnknown =
        1;

    constexpr uint8_t kStateVisibleDiscovered =
        2;

    constexpr uint8_t kStateDisabled =
        3;

    constexpr uint8_t kUnknownFilter =
        24;

    constexpr uint8_t kAbandonedBaseType =
        2;

    constexpr std::ptrdiff_t kActorRootComponentOffset =
        0x1B8;

    constexpr std::ptrdiff_t kActorStateOffset =
        0x2E8;

    constexpr std::ptrdiff_t kActorTypeOffset =
        0x2E9;

    constexpr std::ptrdiff_t kActorGuidOffset =
        0x354;

    constexpr std::ptrdiff_t kCategoryIconOffset =
        0x50;

    constexpr std::ptrdiff_t kCategoryCanHideByFogOffset =
        0x101;

    constexpr std::ptrdiff_t kCategoryLegendFilterOffset =
        0x108;

    constexpr std::ptrdiff_t kCategoryUnknownIconOffset =
        0x110;

    constexpr std::ptrdiff_t kCategoryCompletedBaseIconOffset =
        0x1C0;

    constexpr std::ptrdiff_t kReplicatorDataArrayOffset =
        0x3F8;

    constexpr std::ptrdiff_t kAbandonBaseRecordGuidOffset =
        0x0C;

    constexpr std::size_t kAbandonBaseRecordStride =
        0x78;

    constexpr int32_t kMaximumReasonableAbandonBaseRecords =
        4096;


    struct NativeActorClassStorage
    {
        SDK::UClass* Class =
            nullptr;
    };

    static_assert(
        sizeof(NativeActorClassStorage) ==
        8);


    struct NativeActorArray
    {
        SDK::AActor** Data =
            nullptr;

        int32_t Num =
            0;

        int32_t Max =
            0;
    };

    static_assert(
        sizeof(NativeActorArray) ==
        0x10);


    struct NativeAbandonBaseArray
    {
        uint8_t* Data =
            nullptr;

        int32_t Num =
            0;

        int32_t Max =
            0;
    };

    static_assert(
        sizeof(NativeAbandonBaseArray) ==
        0x10);


    struct POIGuid
    {
        uint32_t A = 0;
        uint32_t B = 0;
        uint32_t C = 0;
        uint32_t D = 0;

        bool operator==(
            const POIGuid& other) const noexcept
        {
            return
                A == other.A &&
                B == other.B &&
                C == other.C &&
                D == other.D;
        }
    };


    struct POIRecord
    {
        POIGuid Guid = {};
        double WorldX = 0.0;
        double WorldY = 0.0;
        PluginTextureHandle Icon = nullptr;
    };


    enum class IconVariant : uint8_t
    {
        Unknown = 1,
        Discovered = 2,
        Completed = 3
    };


    struct POIRGBA8
    {
        uint8_t R;
        uint8_t G;
        uint8_t B;
        uint8_t A;
    };


    IPluginSelf* g_self =
        nullptr;

    bool g_initialized =
        false;

    bool g_tickRegistered =
        false;

    bool g_experienceReady =
        false;

    float g_snapshotAccumulator =
        0.0f;

    SDK::UCrMapMenuDevSettings* g_devSettings =
        nullptr;

    std::mutex g_snapshotMutex;

    std::vector<POIRecord> g_records;

    /*
     * POI icon handles are intentionally retained for plugin/process lifetime.
     *
     * AlienX currently has no fence-aware texture retirement. The landmark
     * icon set is tiny, immutable after upload, and safe to retain.
     */
    std::unordered_map<uint32_t, PluginTextureHandle>
        g_iconCache;


    SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO()
    {
        if (g_self == nullptr ||
            g_self->hooks == nullptr ||
            g_self->hooks->ObjectWalker == nullptr)
        {
            return nullptr;
        }

        auto* walker =
            g_self->hooks->ObjectWalker;

        if (!walker->IsReady())
        {
            return nullptr;
        }

        PluginObjectInfo objects[8] = {};

        const int count =
            walker->FindObjectsByClassNameInto(
                "CrMapMenuDevSettings",
                PluginObjectLookup_CDOOnly,
                objects,
                8);

        if (count !=
                1 ||
            objects[0].object ==
                nullptr)
        {
            return nullptr;
        }

        return static_cast<
            SDK::UCrMapMenuDevSettings*>(
                objects[0].object);
    }


    SDK::UCrPlayerMapMenuDataComponent*
    GetLocalMapDataComponent()
    {
        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        if (native == nullptr ||
            world == nullptr ||
            native->player.getFirstPlayerController == nullptr ||
            native->player.getPlayerPawn == nullptr)
        {
            return nullptr;
        }

        SDK::APlayerController* controller =
            native->player.getFirstPlayerController(
                world);

        if (controller ==
            nullptr)
        {
            return nullptr;
        }

        SDK::ACrCharacterPlayerBase* player =
            native->player.getPlayerPawn(
                static_cast<const SDK::AController*>(
                    controller));

        if (player ==
            nullptr)
        {
            return nullptr;
        }

        return
            player->
                PlayerMapMenuDataComponent;
    }


    uint8_t ReadByte(
        const void* object,
        std::ptrdiff_t offset)
    {
        uint8_t value =
            0;

        if (object !=
            nullptr)
        {
            std::memcpy(
                &value,
                static_cast<const uint8_t*>(
                    object) +
                    offset,
                sizeof(value));
        }

        return value;
    }


    POIGuid ReadGuid(
        const SDK::AActor* actor)
    {
        POIGuid guid = {};

        if (actor !=
            nullptr)
        {
            std::memcpy(
                &guid,
                reinterpret_cast<const uint8_t*>(
                    actor) +
                    kActorGuidOffset,
                sizeof(guid));
        }

        return guid;
    }


    SDK::USceneComponent* ReadRootComponent(
        const SDK::AActor* actor)
    {
        SDK::USceneComponent* component =
            nullptr;

        if (actor !=
            nullptr)
        {
            std::memcpy(
                &component,
                reinterpret_cast<const uint8_t*>(
                    actor) +
                    kActorRootComponentOffset,
                sizeof(component));
        }

        return component;
    }


    const SDK::FSlateBrush* BrushAt(
        const SDK::UCrMapMenuPOIData* category,
        std::ptrdiff_t offset)
    {
        if (category ==
            nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<
            const SDK::FSlateBrush*>(
                reinterpret_cast<const uint8_t*>(
                    category) +
                offset);
    }


    uint8_t Expand5To8(
        uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 15u) /
            31u);
    }


    uint8_t Expand6To8(
        uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 31u) /
            63u);
    }


    POIRGBA8 DecodeRGB565(
        uint16_t value)
    {
        return {
            Expand5To8(
                static_cast<uint16_t>(
                    (value >> 11) &
                    0x1Fu)),
            Expand6To8(
                static_cast<uint16_t>(
                    (value >> 5) &
                    0x3Fu)),
            Expand5To8(
                static_cast<uint16_t>(
                    value &
                    0x1Fu)),
            255
        };
    }


    POIRGBA8 InterpolateRGB(
        const POIRGBA8& a,
        const POIRGBA8& b,
        uint32_t aWeight,
        uint32_t bWeight,
        uint32_t divisor)
    {
        return {
            static_cast<uint8_t>(
                (aWeight * a.R +
                    bWeight * b.R) /
                divisor),
            static_cast<uint8_t>(
                (aWeight * a.G +
                    bWeight * b.G) /
                divisor),
            static_cast<uint8_t>(
                (aWeight * a.B +
                    bWeight * b.B) /
                divisor),
            255
        };
    }


    void DecodeBC3Block(
        const uint8_t* block,
        uint8_t* destination,
        int destinationStrideBytes)
    {
        uint8_t alphaValues[8] = {};

        alphaValues[0] =
            block[0];

        alphaValues[1] =
            block[1];

        if (alphaValues[0] >
            alphaValues[1])
        {
            for (uint32_t i = 1;
                i <= 6;
                ++i)
            {
                alphaValues[i + 1] =
                    static_cast<uint8_t>(
                        ((7u - i) *
                            alphaValues[0] +
                            i *
                            alphaValues[1]) /
                        7u);
            }
        }
        else
        {
            for (uint32_t i = 1;
                i <= 4;
                ++i)
            {
                alphaValues[i + 1] =
                    static_cast<uint8_t>(
                        ((5u - i) *
                            alphaValues[0] +
                            i *
                            alphaValues[1]) /
                        5u);
            }

            alphaValues[6] =
                0;

            alphaValues[7] =
                255;
        }

        uint64_t alphaIndices =
            0;

        for (int i = 0;
            i < 6;
            ++i)
        {
            alphaIndices |=
                static_cast<uint64_t>(
                    block[2 + i]) <<
                (8 * i);
        }

        const uint8_t* colorBlock =
            block +
            8;

        const uint16_t color0 =
            static_cast<uint16_t>(
                colorBlock[0] |
                (static_cast<uint16_t>(
                    colorBlock[1]) <<
                    8));

        const uint16_t color1 =
            static_cast<uint16_t>(
                colorBlock[2] |
                (static_cast<uint16_t>(
                    colorBlock[3]) <<
                    8));

        POIRGBA8 colors[4] = {};

        colors[0] =
            DecodeRGB565(
                color0);

        colors[1] =
            DecodeRGB565(
                color1);

        colors[2] =
            InterpolateRGB(
                colors[0],
                colors[1],
                2,
                1,
                3);

        colors[3] =
            InterpolateRGB(
                colors[0],
                colors[1],
                1,
                2,
                3);

        const uint32_t colorIndices =
            static_cast<uint32_t>(
                colorBlock[4]) |
            (static_cast<uint32_t>(
                colorBlock[5]) <<
                8) |
            (static_cast<uint32_t>(
                colorBlock[6]) <<
                16) |
            (static_cast<uint32_t>(
                colorBlock[7]) <<
                24);

        for (int y = 0;
            y < 4;
            ++y)
        {
            uint8_t* row =
                destination +
                y *
                destinationStrideBytes;

            for (int x = 0;
                x < 4;
                ++x)
            {
                const uint32_t pixelIndex =
                    static_cast<uint32_t>(
                        y * 4 +
                        x);

                const uint32_t colorIndex =
                    (colorIndices >>
                        (pixelIndex *
                            2u)) &
                    0x3u;

                const uint32_t alphaIndex =
                    static_cast<uint32_t>(
                        (alphaIndices >>
                            (pixelIndex *
                                3u)) &
                        0x7u);

                const POIRGBA8& color =
                    colors[colorIndex];

                uint8_t* pixel =
                    row +
                    x *
                    4;

                pixel[0] =
                    color.R;

                pixel[1] =
                    color.G;

                pixel[2] =
                    color.B;

                pixel[3] =
                    alphaValues[
                        alphaIndex];
            }
        }
    }


    bool DecodeBC3Texture(
        const uint8_t* source,
        int width,
        int height,
        size_t sourceBytes,
        std::vector<uint8_t>& rgba)
    {
        if (source == nullptr ||
            width <= 0 ||
            height <= 0 ||
            width % 4 != 0 ||
            height % 4 != 0)
        {
            return false;
        }

        const int blocksX =
            width /
            4;

        const int blocksY =
            height /
            4;

        const size_t expectedBytes =
            static_cast<size_t>(
                blocksX) *
            static_cast<size_t>(
                blocksY) *
            16u;

        if (sourceBytes !=
            expectedBytes)
        {
            return false;
        }

        rgba.resize(
            static_cast<size_t>(
                width) *
            static_cast<size_t>(
                height) *
            4u);

        const int strideBytes =
            width *
            4;

        for (int blockY = 0;
            blockY < blocksY;
            ++blockY)
        {
            for (int blockX = 0;
                blockX < blocksX;
                ++blockX)
            {
                const size_t blockIndex =
                    static_cast<size_t>(
                        blockY) *
                    static_cast<size_t>(
                        blocksX) +
                    static_cast<size_t>(
                        blockX);

                const uint8_t* sourceBlock =
                    source +
                    blockIndex *
                    16u;

                uint8_t* destinationPixel =
                    rgba.data() +
                    static_cast<size_t>(
                        blockY * 4) *
                        static_cast<size_t>(
                            strideBytes) +
                    static_cast<size_t>(
                        blockX * 4) *
                    4u;

                DecodeBC3Block(
                    sourceBlock,
                    destinationPixel,
                    strideBytes);
            }
        }

        return true;
    }


    PluginTextureHandle LoadIconFromBulkData(
        SDK::UTexture2D* texture,
        const char* name)
    {
        if (texture == nullptr ||
            name == nullptr ||
            g_self == nullptr ||
            g_self->hooks == nullptr ||
            g_self->hooks->ImGuiTextures == nullptr)
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::PlatformData
            platformData = {};

        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            texture,
            platformData))
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::MipData
            mipData = {};

        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipData))
        {
            return nullptr;
        }

        void* bulkCopy =
            nullptr;

        if (!MiniMapNative::TextureAccess::CopyBulkData(
            mipData,
            &bulkCopy,
            false))
        {
            return nullptr;
        }

        std::vector<uint8_t> rgba;

        const bool decoded =
            DecodeBC3Texture(
                static_cast<const uint8_t*>(
                    bulkCopy),
                static_cast<int>(
                    mipData.SizeX),
                static_cast<int>(
                    mipData.SizeY),
                static_cast<size_t>(
                    mipData.BulkSize),
                rgba);

        MiniMapNative::TextureAccess::FreeBulkDataCopy(
            bulkCopy);

        if (!decoded)
        {
            return nullptr;
        }

        auto* textures =
            g_self->
                hooks->
                ImGuiTextures;

        if (textures->GetFreeSlotCount() <=
            0)
        {
            return nullptr;
        }

        PluginTextureHandle handle =
            textures->LoadFromRGBA(
                rgba.data(),
                static_cast<int>(
                    mipData.SizeX),
                static_cast<int>(
                    mipData.SizeY),
                name);

        if (handle ==
            nullptr)
        {
            return nullptr;
        }

        LOG_INFO(
            "MiniMap: cached POI icon "
            "name=%s size=%ux%u source=CPU-BC3",
            name,
            static_cast<unsigned int>(
                mipData.SizeX),
            static_cast<unsigned int>(
                mipData.SizeY));

        return handle;
    }



    const SDK::FCrAbandonBaseData* FindReplicatedAbandonBaseData(
        SDK::UWorld* world,
        const POIGuid& guid)
    {
        if (world == nullptr)
        {
            return nullptr;
        }

        /*
         * These two generated fields are verified against the current PDB
         * and native UpdatePOIMarkers path:
         *
         *   UWorld::GameState                         +0x1B0
         *   ACrGameStateBase::AbandonBaseInfoReplicator +0x3A8
         *
         * The tracked MiniMap world is the gameplay world. We do not retain
         * either pointer across polls.
         */
        SDK::AGameStateBase* baseGameState =
            world->GameState;

        if (baseGameState == nullptr)
        {
            return nullptr;
        }

        auto* gameState =
            static_cast<SDK::ACrGameStateBase*>(
                baseGameState);

        SDK::ACrAbandonBaseInfoReplicator* replicator =
            gameState->
                AbandonBaseInfoReplicator;

        if (replicator == nullptr)
        {
            return nullptr;
        }

        NativeAbandonBaseArray array = {};

        std::memcpy(
            &array,
            reinterpret_cast<const uint8_t*>(
                replicator) +
                kReplicatorDataArrayOffset,
            sizeof(array));

        if (array.Num < 0 ||
            array.Max < array.Num ||
            array.Num >
                kMaximumReasonableAbandonBaseRecords ||
            array.Max >
                kMaximumReasonableAbandonBaseRecords)
        {
            return nullptr;
        }

        if (array.Num == 0)
        {
            return nullptr;
        }

        if (array.Data == nullptr)
        {
            return nullptr;
        }

        for (int32_t index = 0;
            index < array.Num;
            ++index)
        {
            const uint8_t* record =
                array.Data +
                static_cast<std::size_t>(
                    index) *
                kAbandonBaseRecordStride;

            POIGuid recordGuid = {};

            std::memcpy(
                &recordGuid,
                record +
                    kAbandonBaseRecordGuidOffset,
                sizeof(recordGuid));

            if (recordGuid ==
                guid)
            {
                return reinterpret_cast<
                    const SDK::FCrAbandonBaseData*>(
                        record);
            }
        }

        /*
         * Do not call native FindAbandonBaseData for an absent record.
         * The native helper enters an Unreal ensure path on lookup miss.
         */
        return nullptr;
    }


    bool IsCompletedAbandonedBase(
        SDK::UWorld* world,
        const POIGuid& guid)
    {
        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->pointsOfInterest.isAbandonBaseCompleted == nullptr)
        {
            return false;
        }

        const SDK::FCrAbandonBaseData* data =
            FindReplicatedAbandonBaseData(
                world,
                guid);

        if (data == nullptr)
        {
            return false;
        }

        return
            native->
                pointsOfInterest.
                isAbandonBaseCompleted(
                    data);
    }

    PluginTextureHandle ResolveIcon(
        const SDK::UCrMapMenuPOIData* category,
        uint8_t poiType,
        IconVariant variant)
    {
        if (category == nullptr)
        {
            return nullptr;
        }

        const uint32_t cacheKey =
            (static_cast<uint32_t>(
                poiType) <<
                8) |
            static_cast<uint32_t>(
                variant);

        const auto existing =
            g_iconCache.find(
                cacheKey);

        if (existing !=
            g_iconCache.end())
        {
            return existing->second;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            return nullptr;
        }

        std::ptrdiff_t brushOffset =
            kCategoryIconOffset;

        if (variant ==
            IconVariant::Unknown)
        {
            brushOffset =
                kCategoryUnknownIconOffset;
        }
        else if (variant ==
            IconVariant::Completed)
        {
            brushOffset =
                kCategoryCompletedBaseIconOffset;
        }

        const SDK::FSlateBrush* brush =
            BrushAt(
                category,
                brushOffset);

        if (brush ==
            nullptr)
        {
            return nullptr;
        }

        SDK::UTexture2D* texture =
            native->
                texture.
                getBrushTexture(
                    *brush);

        if (texture ==
            nullptr)
        {
            return nullptr;
        }

        char name[64] = {};

        const char* variantName =
            "Discovered";

        if (variant ==
            IconVariant::Unknown)
        {
            variantName =
                "Unknown";
        }
        else if (variant ==
            IconVariant::Completed)
        {
            variantName =
                "Completed";
        }

        std::snprintf(
            name,
            sizeof(name),
            "MiniMap_POI_%u_%s",
            static_cast<unsigned int>(
                poiType),
            variantName);

        PluginTextureHandle handle =
            LoadIconFromBulkData(
                texture,
                name);

        if (handle ==
            nullptr)
        {
            return nullptr;
        }

        g_iconCache.emplace(
            cacheKey,
            handle);

        return handle;
    }


    bool TryBuildRecord(
        SDK::AActor* actor,
        SDK::UCrPlayerMapMenuDataComponent* mapData,
        POIRecord& outRecord)
    {
        outRecord = {};

        if (actor == nullptr ||
            mapData == nullptr ||
            g_devSettings == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->pointsOfInterest.findPOIMarkerCategoryData == nullptr ||
            native->pointsOfInterest.getMarkerFilterStatus == nullptr ||
            native->player.getComponentLocation == nullptr)
        {
            return false;
        }

        const uint8_t state =
            ReadByte(
                actor,
                kActorStateOffset);

        if (state ==
                kStateHidden ||
            state ==
                kStateDisabled)
        {
            return false;
        }

        if (state !=
                kStateVisibleUnknown &&
            state !=
                kStateVisibleDiscovered)
        {
            return false;
        }

        const uint8_t poiType =
            ReadByte(
                actor,
                kActorTypeOffset);

        const SDK::UCrMapMenuPOIData* category =
            native->
                pointsOfInterest.
                findPOIMarkerCategoryData(
                    g_devSettings,
                    poiType);

        if (category ==
            nullptr)
        {
            return false;
        }

        const bool unknown =
            state ==
            kStateVisibleUnknown;

        const uint8_t leafFilter =
            unknown
                ? kUnknownFilter
                : ReadByte(
                    category,
                    kCategoryLegendFilterOffset);

        if (!native->
                pointsOfInterest.
                getMarkerFilterStatus(
                    mapData,
                    leafFilter))
        {
            return false;
        }

        const POIGuid guid =
            ReadGuid(
                actor);

        IconVariant iconVariant =
            unknown
                ? IconVariant::Unknown
                : IconVariant::Discovered;

        if (!unknown &&
            poiType ==
                kAbandonedBaseType)
        {
            SDK::UWorld* world =
                MiniMapMap::GetWorld();

            if (IsCompletedAbandonedBase(
                world,
                guid))
            {
                iconVariant =
                    IconVariant::Completed;
            }
        }

        PluginTextureHandle icon =
            ResolveIcon(
                category,
                poiType,
                iconVariant);

        /*
         * If the completed asset is not available yet, retain the ordinary
         * discovered marker for this poll and retry the completed variant on
         * the next poll.
         */
        if (icon == nullptr &&
            iconVariant ==
                IconVariant::Completed)
        {
            icon =
                ResolveIcon(
                    category,
                    poiType,
                    IconVariant::Discovered);
        }

        if (icon ==
            nullptr)
        {
            return false;
        }

        SDK::USceneComponent* root =
            ReadRootComponent(
                actor);

        if (root ==
            nullptr)
        {
            return false;
        }

        SDK::FVector location = {};

        native->
            player.
            getComponentLocation(
                root,
                &location);

        const bool canHideByFog =
            ReadByte(
                category,
                kCategoryCanHideByFogOffset) !=
            0;

        if (canHideByFog &&
            !MiniMapFogOfWar::
                IsNativeWorldPositionRevealed(
                    location.X,
                    location.Y))
        {
            return false;
        }

        outRecord.Guid =
            guid;

        outRecord.WorldX =
            location.X;

        outRecord.WorldY =
            location.Y;

        outRecord.Icon =
            icon;

        return true;
    }


    void PollPointsOfInterest()
    {
        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (world == nullptr ||
            native == nullptr ||
            native->pointsOfInterest.getAllActorsOfClass == nullptr ||
            native->pointsOfInterest.pointOfInterestStaticClass == nullptr ||
            native->texture.memoryFree == nullptr)
        {
            return;
        }

        SDK::UCrPlayerMapMenuDataComponent* mapData =
            GetLocalMapDataComponent();

        if (mapData ==
            nullptr)
        {
            return;
        }

        if (g_devSettings ==
            nullptr)
        {
            g_devSettings =
                FindMapMenuDevSettingsCDO();

            if (g_devSettings ==
                nullptr)
            {
                return;
            }
        }

        SDK::UClass* poiClass =
            native->
                pointsOfInterest.
                pointOfInterestStaticClass();

        if (poiClass ==
            nullptr)
        {
            return;
        }

        NativeActorClassStorage classStorage = {};

        classStorage.Class =
            poiClass;

        NativeActorArray actors = {};

        native->
            pointsOfInterest.
            getAllActorsOfClass(
                static_cast<const SDK::UObject*>(
                    world),
                &classStorage,
                &actors);

        std::vector<POIRecord> nextRecords;

        if (actors.Num >
                0 &&
            actors.Data !=
                nullptr)
        {
            nextRecords.reserve(
                static_cast<std::size_t>(
                    actors.Num));

            for (int32_t index = 0;
                index < actors.Num;
                ++index)
            {
                POIRecord record = {};

                if (TryBuildRecord(
                    actors.Data[index],
                    mapData,
                    record))
                {
                    nextRecords.push_back(
                        record);
                }
            }
        }

        if (actors.Data !=
            nullptr)
        {
            native->
                texture.
                memoryFree(
                    actors.Data);
        }

        {
            std::scoped_lock lock(
                g_snapshotMutex);

            g_records =
                std::move(
                    nextRecords);
        }
    }


    void OnTick(
        float deltaSeconds)
    {
        if (!g_experienceReady ||
            !MiniMapMap::HasWorld())
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

        PollPointsOfInterest();
    }
}


namespace MiniMapPointsOfInterest
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
            return false;
        }

        g_self =
            self;

        Reset();

        self->
            hooks->
            Engine->
            RegisterOnTick(
                &OnTick);

        g_tickRegistered =
            true;

        g_initialized =
            true;

        LOG_INFO(
            "MiniMap: landmark POI subsystem initialized");

        return true;
    }


    void Reset()
    {
        {
            std::scoped_lock lock(
                g_snapshotMutex);

            g_records.clear();
        }

        g_devSettings =
            nullptr;

        g_experienceReady =
            false;

        g_snapshotAccumulator =
            0.0f;
    }


    void OnExperienceLoadComplete()
    {
        if (!g_initialized)
        {
            return;
        }

        /*
         * Let one ordinary snapshot interval elapse before the first scan.
         * Category soft references were observed to become usable shortly
         * after the experience-complete callback rather than synchronously
         * with it.
         */
        g_experienceReady =
            true;

        g_snapshotAccumulator =
            0.0f;

        LOG_INFO(
            "MiniMap: landmark POI acquisition enabled");
    }


    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform)
    {
        if (ui == nullptr ||
            !transform.Valid)
        {
            return;
        }

        std::vector<POIRecord> records;

        {
            std::scoped_lock lock(
                g_snapshotMutex);

            records =
                g_records;
        }

        if (records.empty())
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

        const float viewportX =
            transform.CenterX -
            transform.Width *
            0.5f;

        const float viewportY =
            transform.CenterY -
            transform.Height *
            0.5f;

        const double zoomScale =
            transform.PixelsPerWorldUnit /
            kDefaultPixelsPerWorldUnit;

        const float iconSize =
            std::clamp(
                static_cast<float>(
                    kDefaultIconSizePixels *
                    zoomScale),
                kMinimumIconSizePixels,
                kMaximumIconSizePixels);

        const float halfIcon =
            iconSize *
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

        for (const POIRecord& record :
            records)
        {
            if (record.Icon ==
                nullptr)
            {
                continue;
            }

            MiniMapMap::ScreenPoint point = {};

            if (!transform.WorldToScreen(
                record.WorldX,
                record.WorldY,
                point))
            {
                continue;
            }

            if (point.X <
                    viewportX -
                        halfIcon ||
                point.X >
                    viewportX +
                        transform.Width +
                        halfIcon ||
                point.Y <
                    viewportY -
                        halfIcon ||
                point.Y >
                    viewportY +
                        transform.Height +
                        halfIcon)
            {
                continue;
            }

            ui->DL_AddImage(
                drawList,
                record.Icon,
                point.X -
                    halfIcon,
                point.Y -
                    halfIcon,
                point.X +
                    halfIcon,
                point.Y +
                    halfIcon,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                0xFFFFFFFFu);
        }

        ui->DL_PopClipRect(
            drawList);
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
            g_self->
                hooks->
                Engine->
                UnregisterOnTick(
                    &OnTick);
        }

        g_tickRegistered =
            false;

        Reset();

        /*
         * Deliberately retain cached immutable icon textures. AlienX does not
         * currently provide fence-aware texture retirement.
         */
        g_self =
            nullptr;

        g_initialized =
            false;

        LOG_INFO(
            "MiniMap: landmark POI subsystem shut down");
    }
}

#endif