#if defined(MODLOADER_CLIENT_BUILD)

#include "Terrain.h"
#include "Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "../Native/NativeApi.h"

#include <cmath>

#include "../plugin.h"
#include "../plugin_helpers.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>

namespace
{
    IPluginSelf* g_terrainSelf = nullptr;
    SDK::UCrMapMenuTerrainData* g_terrainData = nullptr;

    PluginTextureHandle g_terrainTexture = nullptr;
    PluginTextureHandle g_referenceTerrainTexture = nullptr;

    std::atomic<bool> g_diagnosticPending = false;

    bool g_diagnosticKeyRegistered = false;
    bool g_tickRegistered = false;

    constexpr const char* kDiagnosticTextureName =
        "MiniMap_Terrain_Current";

    constexpr const char* kDiagnosticChunkTextureName =
        "MiniMap_Terrain_Current_Chunk";

    constexpr const char* kDiagnosticKey = "F8";



    struct NativeTexturePlatformDataLayout
    {
        int32_t SizeX;
        int32_t SizeY;
        uint32_t PackedData;
        uint8_t PixelFormat;
        uint8_t Padding0D[0x0B];
        void** MipPointers;
        int32_t MipCount;
        int32_t MipCapacity;
        void* VTData;
        void* CPUCopy;
    };

    static_assert(
        offsetof(
            NativeTexturePlatformDataLayout,
            MipPointers) == 0x18);

    static_assert(
        offsetof(
            NativeTexturePlatformDataLayout,
            VTData) == 0x28);

    static_assert(
        offsetof(
            NativeTexturePlatformDataLayout,
            CPUCopy) == 0x30);

    static_assert(
        sizeof(
            NativeTexturePlatformDataLayout) == 0x38);


    struct NativeTexture2DMipMapLayout
    {
        uint8_t DerivedData[0x20];
        uint8_t BulkData[0x28];
        uint16_t SizeX;
        uint16_t SizeY;
        uint16_t SizeZ;
        uint16_t Padding4E;
    };

    static_assert(
        offsetof(
            NativeTexture2DMipMapLayout,
            BulkData) == 0x20);

    static_assert(
        offsetof(
            NativeTexture2DMipMapLayout,
            SizeX) == 0x48);

    static_assert(
        sizeof(
            NativeTexture2DMipMapLayout) == 0x50);


    struct RGBA8
    {
        uint8_t R;
        uint8_t G;
        uint8_t B;
        uint8_t A;
    };


    uint8_t Expand5To8(uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 15u) / 31u);
    }


    uint8_t Expand6To8(uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 31u) / 63u);
    }


    RGBA8 DecodeRGB565(uint16_t value)
    {
        RGBA8 color{};

        color.R =
            Expand5To8(
                static_cast<uint16_t>(
                    (value >> 11) & 0x1Fu));

        color.G =
            Expand6To8(
                static_cast<uint16_t>(
                    (value >> 5) & 0x3Fu));

        color.B =
            Expand5To8(
                static_cast<uint16_t>(
                    value & 0x1Fu));

        color.A = 255;

        return color;
    }


    RGBA8 InterpolateColor(
        const RGBA8& a,
        const RGBA8& b,
        uint32_t aWeight,
        uint32_t bWeight,
        uint32_t divisor)
    {
        RGBA8 result{};

        result.R = static_cast<uint8_t>(
            (aWeight * a.R + bWeight * b.R) / divisor);

        result.G = static_cast<uint8_t>(
            (aWeight * a.G + bWeight * b.G) / divisor);

        result.B = static_cast<uint8_t>(
            (aWeight * a.B + bWeight * b.B) / divisor);

        result.A = static_cast<uint8_t>(
            (aWeight * a.A + bWeight * b.A) / divisor);

        return result;
    }


    void DecodeBC1Block(
        const uint8_t* block,
        uint8_t* rgba,
        int rgbaStrideBytes)
    {
        const uint16_t color0 =
            static_cast<uint16_t>(
                block[0] |
                (static_cast<uint16_t>(block[1]) << 8));

        const uint16_t color1 =
            static_cast<uint16_t>(
                block[2] |
                (static_cast<uint16_t>(block[3]) << 8));

        RGBA8 colors[4] = {};

        colors[0] = DecodeRGB565(color0);
        colors[1] = DecodeRGB565(color1);

        if (color0 > color1)
        {
            colors[2] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    2,
                    1,
                    3);

            colors[3] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    1,
                    2,
                    3);
        }
        else
        {
            colors[2] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    1,
                    1,
                    2);

            colors[3] = { 0, 0, 0, 0 };
        }

        const uint32_t indices =
            static_cast<uint32_t>(block[4]) |
            (static_cast<uint32_t>(block[5]) << 8) |
            (static_cast<uint32_t>(block[6]) << 16) |
            (static_cast<uint32_t>(block[7]) << 24);

        for (int y = 0; y < 4; ++y)
        {
            uint8_t* row =
                rgba + y * rgbaStrideBytes;

            for (int x = 0; x < 4; ++x)
            {
                const uint32_t pixelIndex =
                    static_cast<uint32_t>(y * 4 + x);

                const uint32_t colorIndex =
                    (indices >> (pixelIndex * 2u)) & 0x3u;

                const RGBA8& color =
                    colors[colorIndex];

                uint8_t* pixel =
                    row + x * 4;

                pixel[0] = color.R;
                pixel[1] = color.G;
                pixel[2] = color.B;
                pixel[3] = color.A;
            }
        }
    }


    bool ExtractBC1Chunk(
        const uint8_t* source,
        int sourceWidth,
        int sourceHeight,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk)
    {
        constexpr int chunkSizePixels = 256;
        constexpr int blockSizePixels = 4;
        constexpr int bytesPerBlock = 8;

        if (source == nullptr ||
            sourceWidth <= 0 ||
            sourceHeight <= 0 ||
            sourceWidth % blockSizePixels != 0 ||
            sourceHeight % blockSizePixels != 0)
        {
            return false;
        }

        const int sourceBlocksPerRow =
            sourceWidth / blockSizePixels;

        const int sourceBlockRows =
            sourceHeight / blockSizePixels;

        const int chunkBlocksPerAxis =
            chunkSizePixels / blockSizePixels;

        const int startBlockX =
            chunkX * chunkBlocksPerAxis;

        const int startBlockY =
            chunkY * chunkBlocksPerAxis;

        if (startBlockX < 0 ||
            startBlockY < 0 ||
            startBlockX + chunkBlocksPerAxis > sourceBlocksPerRow ||
            startBlockY + chunkBlocksPerAxis > sourceBlockRows)
        {
            return false;
        }

        const size_t chunkRowBytes =
            static_cast<size_t>(chunkBlocksPerAxis) *
            bytesPerBlock;

        const size_t chunkByteCount =
            chunkRowBytes *
            static_cast<size_t>(chunkBlocksPerAxis);

        compressedChunk.resize(
            chunkByteCount);

        for (int blockRow = 0;
            blockRow < chunkBlocksPerAxis;
            ++blockRow)
        {
            const size_t sourceOffset =
                (static_cast<size_t>(
                    startBlockY + blockRow) *
                    static_cast<size_t>(sourceBlocksPerRow) +
                    static_cast<size_t>(startBlockX)) *
                bytesPerBlock;

            const size_t destinationOffset =
                static_cast<size_t>(blockRow) *
                chunkRowBytes;

            std::memcpy(
                compressedChunk.data() + destinationOffset,
                source + sourceOffset,
                chunkRowBytes);
        }

        return true;
    }


    bool DecodeBC1Chunk(
        const std::vector<uint8_t>& compressedChunk,
        std::vector<uint8_t>& rgbaChunk)
    {
        constexpr int chunkSizePixels = 256;
        constexpr int blockSizePixels = 4;
        constexpr int bytesPerBlock = 8;
        constexpr int blocksPerAxis =
            chunkSizePixels / blockSizePixels;

        const size_t expectedCompressedBytes =
            static_cast<size_t>(blocksPerAxis) *
            static_cast<size_t>(blocksPerAxis) *
            bytesPerBlock;

        if (compressedChunk.size() !=
            expectedCompressedBytes)
        {
            return false;
        }

        rgbaChunk.resize(
            static_cast<size_t>(chunkSizePixels) *
            static_cast<size_t>(chunkSizePixels) *
            4u);

        const int rgbaStrideBytes =
            chunkSizePixels * 4;

        for (int blockY = 0;
            blockY < blocksPerAxis;
            ++blockY)
        {
            for (int blockX = 0;
                blockX < blocksPerAxis;
                ++blockX)
            {
                const size_t blockIndex =
                    static_cast<size_t>(blockY) *
                    blocksPerAxis +
                    static_cast<size_t>(blockX);

                const uint8_t* sourceBlock =
                    compressedChunk.data() +
                    blockIndex * bytesPerBlock;

                uint8_t* destinationPixel =
                    rgbaChunk.data() +
                    static_cast<size_t>(blockY * blockSizePixels) *
                    rgbaStrideBytes +
                    static_cast<size_t>(blockX * blockSizePixels) *
                    4u;

                DecodeBC1Block(
                    sourceBlock,
                    destinationPixel,
                    rgbaStrideBytes);
            }
        }

        return true;
    }


    const char* GetPixelFormatName(
        uint8_t pixelFormat)
    {
        switch (pixelFormat)
        {
        case 2:
            return "PF_B8G8R8A8";

        case 5:
            return "PF_DXT1";

        case 7:
            return "PF_DXT5";

        case 23:
            return "PF_BC5";

        case 37:
            return "PF_R8G8B8A8";

        case 56:
            return "PF_BC7";

        default:
            return "<other>";
        }
    }


    SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO()
    {
        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ObjectWalker == nullptr)
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is unavailable");

            return nullptr;
        }

        auto* walker =
            g_terrainSelf->hooks->ObjectWalker;

        if (!walker->IsReady())
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is not ready");

            return nullptr;
        }

        PluginObjectInfo objects[8] = {};

        const int count =
            walker->FindObjectsByClassNameInto(
                "CrMapMenuDevSettings",
                PluginObjectLookup_CDOOnly,
                objects,
                8);

        LOG_INFO(
            "MiniMap: found %d CrMapMenuDevSettings CDO(s)",
            count);

        const int loggedCount =
            count < 8 ? count : 8;

        for (int i = 0; i < loggedCount; ++i)
        {
            LOG_INFO(
                "MiniMap: DevSettings CDO [%d]: "
                "object=%p name=%s class=%s",
                i,
                objects[i].object,
                objects[i].objectName,
                objects[i].className);
        }

        if (count != 1 ||
            objects[0].object == nullptr)
        {
            LOG_WARN(
                "MiniMap: expected exactly one "
                "CrMapMenuDevSettings CDO");

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuDevSettings*>(
            objects[0].object);
    }


    void ProcessTerrainDiagnostic()
    {
        LOG_INFO(
            "MiniMap: processing F8 terrain diagnostic "
            "on game-thread tick");

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "plugin hooks are unavailable");

            return;
        }

        if (!MiniMapMap::HasWorld())
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "active gameplay world is unavailable");

            return;
        }

        if (g_terrainTexture != nullptr)
        {
            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "terrain texture is already loaded: %p",
                g_terrainTexture);

            return;
        }

        if (g_terrainSelf->hooks->ImGuiTextures == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "ImGui texture interface is unavailable");

            return;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "native API is unavailable");

            return;
        }

        const auto& assetApi =
            native->asset;

        const auto& textureApi =
            native->texture;

        if (assetApi.loadSynchronous == nullptr ||
            textureApi.getBrushTexture == nullptr ||
            textureApi.getNumResidentMips == nullptr ||
            textureApi.getNumMipsAllowed == nullptr ||
            textureApi.getNumMips == nullptr ||
            textureApi.streamIn == nullptr ||
            textureApi.waitForPendingInitOrStreaming == nullptr ||
            textureApi.getPlatformData == nullptr ||
            textureApi.getBulkDataSize == nullptr ||
            textureApi.canLoadFromDisk == nullptr ||
            textureApi.getBulkDataCopy == nullptr ||
            textureApi.memoryFree == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "native API is incomplete");

            return;
        }

        // ---------------------------------------------------------------------
        // Obtain the local pawn's physical world position.
        //
        // World ownership and player discovery are map-wide responsibilities.
        // Terrain consumes only the resulting physical world position.
        // ---------------------------------------------------------------------

        SDK::FVector playerLocation = {};

        if (!MiniMapMap::TryGetPlayerWorldPosition(
            playerLocation))
        {
            return;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "player world location=(%.3f, %.3f, %.3f)",
            playerLocation.X,
            playerLocation.Y,
            playerLocation.Z);

        // ---------------------------------------------------------------------
        // Load UCrMapMenuTerrainData.
        // ---------------------------------------------------------------------

        auto* devSettings =
            FindMapMenuDevSettingsCDO();

        if (devSettings == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "CrMapMenuDevSettings CDO was not found");

            return;
        }

        void* terrainDataSoftPtr =
            static_cast<void*>(
                &devSettings->TerrainData);

        SDK::UObject* loadedObject =
            assetApi.loadSynchronous(
                terrainDataSoftPtr);

        if (loadedObject == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "TerrainData could not be loaded");

            return;
        }

        auto* loadedTerrainData =
            static_cast<SDK::UCrMapMenuTerrainData*>(
                loadedObject);

        g_terrainData =
            loadedTerrainData;

        const auto& pivot =
            loadedTerrainData->MapTerrainTopLeftPivotPoint;

        const auto& segmentSize =
            loadedTerrainData->MapTerrainSegmentSize;

        const int segmentCount =
            loadedTerrainData->TerrainSegmentsData.Num();

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "TerrainData origin=(%.3f, %.3f, %.3f) "
            "segment-size=(%.3f, %.3f, %.3f) "
            "segments=%d",
            pivot.X,
            pivot.Y,
            pivot.Z,
            segmentSize.X,
            segmentSize.Y,
            segmentSize.Z,
            segmentCount);

        // ---------------------------------------------------------------------
        // Convert physical player world position to terrain-grid coordinates.
        //
        // Native StarRupture terrain placement establishes a factor of 100
        // between MapTerrainSegmentSize canvas units and world units.
        //
        // World X -> terrain horizontal -> TerrainSegmentGridIndex.Y
        // World Y -> terrain vertical   -> TerrainSegmentGridIndex.X
        // ---------------------------------------------------------------------

        const double worldTileWidth =
            100.0 *
            static_cast<double>(
                segmentSize.X);

        const double worldTileHeight =
            100.0 *
            static_cast<double>(
                segmentSize.Y);

        if (worldTileWidth <= 0.0 ||
            worldTileHeight <= 0.0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "invalid terrain world tile size "
                "(%.3f x %.3f)",
                worldTileWidth,
                worldTileHeight);

            return;
        }

        const double horizontalTile =
            (playerLocation.X -
                static_cast<double>(pivot.X)) /
            worldTileWidth;

        const double verticalTile =
            (playerLocation.Y -
                static_cast<double>(pivot.Y)) /
            worldTileHeight;

        const int gridX =
            static_cast<int>(
                std::floor(
                    verticalTile));

        const int gridY =
            static_cast<int>(
                std::floor(
                    horizontalTile));

        const double localU =
            horizontalTile -
            static_cast<double>(gridY);

        const double localV =
            verticalTile -
            static_cast<double>(gridX);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "player terrain grid=(%d, %d) "
            "local-uv=(%.6f, %.6f)",
            gridX,
            gridY,
            localU,
            localV);

        constexpr int miniMapChunksPerAxis = 8;

        const int chunkX =
            static_cast<int>(
                std::floor(
                    localU *
                    miniMapChunksPerAxis));

        const int chunkY =
            static_cast<int>(
                std::floor(
                    localV *
                    miniMapChunksPerAxis));

        if (chunkX < 0 ||
            chunkX >= miniMapChunksPerAxis ||
            chunkY < 0 ||
            chunkY >= miniMapChunksPerAxis)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "local UV maps outside 8x8 MiniMap chunk grid "
                "chunk=(%d, %d)",
                chunkX,
                chunkY);

            return;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "MiniMap chunk=(%d, %d) of 8x8 "
            "(125m x 125m)",
            chunkX,
            chunkY);

        // ---------------------------------------------------------------------
        // Find the actual terrain record by TerrainSegmentGridIndex.
        // Do not assume array order matches grid order.
        // ---------------------------------------------------------------------

        const SDK::FCrTerrainSegmentData* diagnosticSegment =
            nullptr;

        for (int i = 0; i < segmentCount; ++i)
        {
            const auto& segment =
                loadedTerrainData->
                TerrainSegmentsData[i];

            if (segment.TerrainSegmentGridIndex.X ==
                gridX &&
                segment.TerrainSegmentGridIndex.Y ==
                gridY)
            {
                diagnosticSegment =
                    &segment;

                break;
            }
        }

        if (diagnosticSegment == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "player position maps outside available terrain "
                "or grid=(%d, %d) has no terrain record",
                gridX,
                gridY);

            return;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "selected terrain segment grid=(%d, %d)",
            diagnosticSegment->
            TerrainSegmentGridIndex.X,
            diagnosticSegment->
            TerrainSegmentGridIndex.Y);

        // ---------------------------------------------------------------------
        // Obtain the UTexture2D from the selected terrain brush.
        // ---------------------------------------------------------------------

        SDK::UTexture2D* sourceTexture =
            textureApi.getBrushTexture(
                diagnosticSegment->
                TerrainSegmentTexture);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "GetBrushResourceAsTexture2D returned %p",
            sourceTexture);

        if (sourceTexture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "selected terrain brush did not provide "
                "a UTexture2D");

            return;
        }

        auto* streamableTexture =
            static_cast<SDK::UStreamableRenderAsset*>(
                sourceTexture);

        // ---------------------------------------------------------------------
        // First allow any initialization or streaming work already associated
        // with this texture to finish.
        //
        // StreamIn rejects a new request while pending work exists. This was
        // observed at runtime: the first F8 press returned false while the same
        // request succeeded shortly afterwards.
        // ---------------------------------------------------------------------

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "waiting for existing texture initialization/streaming");

        textureApi.waitForPendingInitOrStreaming(
            streamableTexture,
            true,
            true);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "existing texture initialization/streaming completed");

        // ---------------------------------------------------------------------
        // Inspect the cooked texture metadata without requesting mip 0 or
        // copying bulk bytes yet.
        //
        // These layouts are verified against the matching HF2.5 PDB. The
        // generated SDK intentionally hides FTexturePlatformData, so this
        // diagnostic keeps the native layout knowledge local to Terrain.
        // ---------------------------------------------------------------------

        void* platformDataRaw =
            textureApi.getPlatformData(
                sourceTexture);

        if (platformDataRaw == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "UTexture2D::GetPlatformData returned null");

            return;
        }

        const auto* platformData =
            reinterpret_cast<
            const NativeTexturePlatformDataLayout*>(
                platformDataRaw);

        if (platformData->SizeX <= 0 ||
            platformData->SizeY <= 0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "invalid platform texture dimensions %dx%d",
                platformData->SizeX,
                platformData->SizeY);

            return;
        }

        if (platformData->MipCount <= 0 ||
            platformData->MipCount > 64 ||
            platformData->MipPointers == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "invalid platform mip array "
                "pointer=%p count=%d capacity=%d",
                platformData->MipPointers,
                platformData->MipCount,
                platformData->MipCapacity);

            return;
        }

        const uint8_t textureFlags =
            *reinterpret_cast<const uint8_t*>(
                reinterpret_cast<const uint8_t*>(
                    sourceTexture) +
                0x106);

        const bool isSRGB =
            (textureFlags & 0x01) != 0;

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "terrain platform data: "
            "size=%dx%d packed=0x%08X "
            "pixelFormat=%u (%s) sRGB=%s "
            "mips=%d capacity=%d "
            "VTData=%p CPUCopy=%p",
            platformData->SizeX,
            platformData->SizeY,
            platformData->PackedData,
            static_cast<unsigned int>(
                platformData->PixelFormat),
            GetPixelFormatName(
                platformData->PixelFormat),
            isSRGB ? "true" : "false",
            platformData->MipCount,
            platformData->MipCapacity,
            platformData->VTData,
            platformData->CPUCopy);

        if (platformData->VTData != nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "terrain texture uses virtual-texture data; "
                "this first bulk-data experiment does not support it");

            return;
        }

        auto* mipZero =
            reinterpret_cast<
            const NativeTexture2DMipMapLayout*>(
                platformData->MipPointers[0]);

        if (mipZero == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "platform mip zero pointer is null");

            return;
        }

        const void* bulkData =
            static_cast<const void*>(
                mipZero->BulkData);

        const int64_t mipZeroBulkSize =
            textureApi.getBulkDataSize(
                bulkData);

        const bool mipZeroCanLoadFromDisk =
            textureApi.canLoadFromDisk(
                bulkData);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "mip0: size=%ux%ux%u "
            "bulk=%p bulkSize=%lld "
            "canLoadFromDisk=%s",
            static_cast<unsigned int>(
                mipZero->SizeX),
            static_cast<unsigned int>(
                mipZero->SizeY),
            static_cast<unsigned int>(
                mipZero->SizeZ),
            bulkData,
            static_cast<long long>(
                mipZeroBulkSize),
            mipZeroCanLoadFromDisk
            ? "true"
            : "false");

        if (mipZero->SizeX == 0 ||
            mipZero->SizeY == 0 ||
            mipZero->SizeZ == 0 ||
            mipZeroBulkSize <= 0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "mip0 metadata is invalid");

            return;
        }


        // ---------------------------------------------------------------------
        // Obtain an independent owned copy of cooked mip zero and extract only
        // the current 8x8 MiniMap chunk from its BC1 block stream.
        //
        // PF_DXT1 stores 4x4 texel blocks in eight bytes. A 256x256 chunk is
        // therefore exactly 64x64 blocks = 32768 bytes.
        // ---------------------------------------------------------------------

        if (platformData->PixelFormat != 5)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "BC1 chunk experiment requires PF_DXT1; observed %u (%s)",
                static_cast<unsigned int>(
                    platformData->PixelFormat),
                GetPixelFormatName(
                    platformData->PixelFormat));

            return;
        }

        const int expectedSourceWidth =
            static_cast<int>(mipZero->SizeX);

        const int expectedSourceHeight =
            static_cast<int>(mipZero->SizeY);

        if (expectedSourceWidth != platformData->SizeX ||
            expectedSourceHeight != platformData->SizeY)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "mip0 dimensions do not match platform data "
                "(%dx%d vs %dx%d)",
                expectedSourceWidth,
                expectedSourceHeight,
                platformData->SizeX,
                platformData->SizeY);

            return;
        }

        const int64_t expectedBC1ByteCount =
            static_cast<int64_t>(expectedSourceWidth / 4) *
            static_cast<int64_t>(expectedSourceHeight / 4) *
            8;

        if (mipZeroBulkSize != expectedBC1ByteCount)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "unexpected PF_DXT1 mip0 byte count "
                "(actual=%lld expected=%lld)",
                static_cast<long long>(mipZeroBulkSize),
                static_cast<long long>(expectedBC1ByteCount));

            return;
        }

        void* ownedMipZero = nullptr;

        const auto copyStart =
            std::chrono::steady_clock::now();

        textureApi.getBulkDataCopy(
            const_cast<void*>(bulkData),
            &ownedMipZero,
            false);

        const auto copyEnd =
            std::chrono::steady_clock::now();

        if (ownedMipZero == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "FBulkData::GetCopy returned a null allocation");

            return;
        }

        const double copyMilliseconds =
            std::chrono::duration<double, std::milli>(
                copyEnd - copyStart).count();

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "mip0 GetCopy returned %p bytes=%lld time=%.3f ms",
            ownedMipZero,
            static_cast<long long>(mipZeroBulkSize),
            copyMilliseconds);

        std::vector<uint8_t> compressedChunk;
        std::vector<uint8_t> rgbaChunk;

        const auto extractStart =
            std::chrono::steady_clock::now();

        const bool extracted =
            ExtractBC1Chunk(
                static_cast<const uint8_t*>(ownedMipZero),
                expectedSourceWidth,
                expectedSourceHeight,
                chunkX,
                chunkY,
                compressedChunk);

        textureApi.memoryFree(
            ownedMipZero);

        ownedMipZero = nullptr;

        const auto extractEnd =
            std::chrono::steady_clock::now();

        if (!extracted)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "failed to extract BC1 MiniMap chunk=(%d, %d)",
                chunkX,
                chunkY);

            return;
        }

        const auto decodeStart =
            std::chrono::steady_clock::now();

        const bool decoded =
            DecodeBC1Chunk(
                compressedChunk,
                rgbaChunk);

        const auto decodeEnd =
            std::chrono::steady_clock::now();

        if (!decoded)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "failed to decode BC1 MiniMap chunk=(%d, %d)",
                chunkX,
                chunkY);

            return;
        }

        const double extractMilliseconds =
            std::chrono::duration<double, std::milli>(
                extractEnd - extractStart).count();

        const double decodeMilliseconds =
            std::chrono::duration<double, std::milli>(
                decodeEnd - decodeStart).count();

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "extracted BC1 chunk=(%d, %d) compressed=%zu bytes "
            "RGBA=%zu bytes extract=%.3f ms decode=%.3f ms",
            chunkX,
            chunkY,
            compressedChunk.size(),
            rgbaChunk.size(),
            extractMilliseconds,
            decodeMilliseconds);

        // ---------------------------------------------------------------------
        // Request exactly nine resident mips when possible.
        //
        // For the observed conventional 2048x2048 / 12-mip terrain textures:
        //
        //   9 resident mips -> 256x256 live GPU resource.
        //
        // StreamIn can only increase residency. It cannot reduce a texture that
        // already has more than the requested number of resident mips.
        // ---------------------------------------------------------------------

        constexpr int32_t targetResidentMips =
            9;

        const int32_t totalMipsBefore =
            textureApi.getNumMips(
                sourceTexture);

        const int32_t allowedMipsBefore =
            textureApi.getNumMipsAllowed(
                sourceTexture,
                false);

        const int32_t residentMipsBefore =
            textureApi.getNumResidentMips(
                sourceTexture);

        int32_t residentMips =
            residentMipsBefore;

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "texture mips before StreamIn: "
            "total=%d allowed=%d resident=%d target=%d",
            totalMipsBefore,
            allowedMipsBefore,
            residentMipsBefore,
            targetResidentMips);

        if (allowedMipsBefore < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture permits only %d mip(s); "
                "target is %d",
                allowedMipsBefore,
                targetResidentMips);

            return;
        }

        if (totalMipsBefore < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture contains only %d mip(s); "
                "target is %d",
                totalMipsBefore,
                targetResidentMips);

            return;
        }

        if (residentMips < targetResidentMips)
        {
            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "calling StreamIn(%d, true)",
                targetResidentMips);

            const bool streamAccepted =
                textureApi.streamIn(
                    sourceTexture,
                    targetResidentMips,
                    true);

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "StreamIn returned %s",
                streamAccepted
                ? "true"
                : "false");

            if (!streamAccepted)
            {
                LOG_ERROR(
                    "MiniMap: F8 diagnostic: "
                    "StreamIn request was not accepted");

                return;
            }

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "waiting for requested texture transition");

            textureApi.waitForPendingInitOrStreaming(
                streamableTexture,
                true,
                true);

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "requested texture transition completed");

            residentMips =
                textureApi.getNumResidentMips(
                    sourceTexture);
        }
        else if (residentMips > targetResidentMips)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "texture already has %d resident mips; "
                "StreamIn cannot reduce it to target %d",
                residentMips,
                targetResidentMips);
        }

        const int32_t totalMipsAfter =
            textureApi.getNumMips(
                sourceTexture);

        const int32_t allowedMipsAfter =
            textureApi.getNumMipsAllowed(
                sourceTexture,
                false);

        const int32_t residentMipsAfter =
            textureApi.getNumResidentMips(
                sourceTexture);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "texture mips before AlienX copy: "
            "total=%d allowed=%d resident=%d target=%d",
            totalMipsAfter,
            allowedMipsAfter,
            residentMipsAfter,
            targetResidentMips);

        if (residentMipsAfter < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture did not reach requested residency "
                "(resident=%d target=%d)",
                residentMipsAfter,
                targetResidentMips);

            return;
        }

        if (residentMipsBefore <= targetResidentMips &&
            residentMipsAfter != targetResidentMips)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "exact-nine diagnostic did not finish at target "
                "(resident=%d target=%d)",
                residentMipsAfter,
                targetResidentMips);
        }

        // ---------------------------------------------------------------------
        // Keep the existing whole-tile AlienX copy as a reference, then upload
        // the independently decoded 256x256 MiniMap chunk. The public UI getter
        // returns the chunk handle so the viewport displays the new CPU path.
        // ---------------------------------------------------------------------

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "copying current player terrain grid=(%d, %d) "
            "through AlienX as reference",
            gridX,
            gridY);

        g_referenceTerrainTexture =
            g_terrainSelf->
            hooks->
            ImGuiTextures->
            LoadFromUTexture2D(
                sourceTexture,
                kDiagnosticTextureName);

        if (g_referenceTerrainTexture == nullptr)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "AlienX reference terrain texture load returned null");

            return;
        }

        int referenceWidth = 0;
        int referenceHeight = 0;

        g_terrainSelf->
            hooks->
            ImGuiTextures->
            GetSize(
                g_referenceTerrainTexture,
                &referenceWidth,
                &referenceHeight);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "AlienX reference terrain texture loaded: "
            "grid=(%d, %d) handle=%p size=%dx%d",
            gridX,
            gridY,
            g_referenceTerrainTexture,
            referenceWidth,
            referenceHeight);

        const auto uploadStart =
            std::chrono::steady_clock::now();

        g_terrainTexture =
            g_terrainSelf->
            hooks->
            ImGuiTextures->
            LoadFromRGBA(
                rgbaChunk.data(),
                256,
                256,
                kDiagnosticChunkTextureName);

        const auto uploadEnd =
            std::chrono::steady_clock::now();

        if (g_terrainTexture == nullptr)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "AlienX decoded chunk texture load returned null");

            return;
        }

        int textureWidth = 0;
        int textureHeight = 0;

        g_terrainSelf->
            hooks->
            ImGuiTextures->
            GetSize(
                g_terrainTexture,
                &textureWidth,
                &textureHeight);

        const double uploadMilliseconds =
            std::chrono::duration<double, std::milli>(
                uploadEnd - uploadStart).count();

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "AlienX decoded chunk loaded: "
            "terrain-grid=(%d, %d) chunk=(%d, %d) "
            "handle=%p size=%dx%d upload=%.3f ms",
            gridX,
            gridY,
            chunkX,
            chunkY,
            g_terrainTexture,
            textureWidth,
            textureHeight,
            uploadMilliseconds);
    }


    void OnDiagnosticKeyPressed(
        EModKey key,
        EModKeyEvent event)
    {
        (void)key;
        (void)event;

        g_diagnosticPending.store(
            true,
            std::memory_order_release);

        LOG_INFO(
            "MiniMap: F8 terrain diagnostic requested");
    }


    void OnTick(
        float deltaSeconds)
    {
        (void)deltaSeconds;

        const bool diagnosticRequested =
            g_diagnosticPending.exchange(
                false,
                std::memory_order_acq_rel);

        if (!diagnosticRequested)
        {
            return;
        }

        ProcessTerrainDiagnostic();
    }
}


namespace MiniMapTerrain
{
    void CancelPendingDiagnostic()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);
    }

    bool Initialize(IPluginSelf* self)
    {
        g_terrainSelf = self;
        g_terrainData = nullptr;

        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: hooks are unavailable "
                "during terrain initialization");

            return false;
        }

        return true;
    }


    bool RegisterDiagnostics(IPluginSelf* self)
    {
        if (self == nullptr ||
            self->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostics "
                "because plugin hooks are unavailable");

            return false;
        }

        if (self->hooks->Input == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostic key "
                "because input hooks are unavailable");

            return false;
        }

        if (self->hooks->Engine == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostic tick "
                "because engine hooks are unavailable");

            return false;
        }

        g_terrainSelf = self;

        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        self->hooks->Engine->RegisterOnTick(
            &OnTick);

        g_tickRegistered = true;

        self->hooks->Input->RegisterKeybindByName(
            kDiagnosticKey,
            EModKeyEvent::Pressed,
            &OnDiagnosticKeyPressed);

        g_diagnosticKeyRegistered = true;

        LOG_INFO(
            "MiniMap: registered terrain diagnostic key: %s",
            kDiagnosticKey);

        LOG_INFO(
            "MiniMap: registered terrain diagnostic game-thread tick");

        return true;
    }


    void UnregisterDiagnostics()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_terrainSelf != nullptr &&
            g_terrainSelf->hooks != nullptr)
        {
            if (g_diagnosticKeyRegistered &&
                g_terrainSelf->hooks->Input != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    Input->
                    UnregisterKeybindByName(
                        kDiagnosticKey,
                        EModKeyEvent::Pressed,
                        &OnDiagnosticKeyPressed);

                LOG_INFO(
                    "MiniMap: unregistered terrain diagnostic key: %s",
                    kDiagnosticKey);
            }

            if (g_tickRegistered &&
                g_terrainSelf->hooks->Engine != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    Engine->
                    UnregisterOnTick(
                        &OnTick);

                LOG_INFO(
                    "MiniMap: unregistered terrain diagnostic "
                    "game-thread tick");
            }
        }

        g_diagnosticKeyRegistered = false;
        g_tickRegistered = false;
    }


    void Shutdown()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_referenceTerrainTexture != nullptr)
        {
            if (g_terrainSelf != nullptr &&
                g_terrainSelf->hooks != nullptr &&
                g_terrainSelf->hooks->ImGuiTextures != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    ImGuiTextures->
                    FreeTexture(
                        g_referenceTerrainTexture);

                LOG_INFO(
                    "MiniMap: reference terrain texture released");
            }
            else
            {
                LOG_WARN(
                    "MiniMap: reference terrain texture handle could not "
                    "be released because ImGuiTextures is unavailable");
            }

            g_referenceTerrainTexture = nullptr;
        }

        if (g_terrainTexture != nullptr)
        {
            if (g_terrainSelf != nullptr &&
                g_terrainSelf->hooks != nullptr &&
                g_terrainSelf->hooks->ImGuiTextures != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    ImGuiTextures->
                    FreeTexture(
                        g_terrainTexture);

                LOG_INFO(
                    "MiniMap: terrain texture released");
            }
            else
            {
                LOG_WARN(
                    "MiniMap: terrain texture handle could not "
                    "be released because ImGuiTextures is unavailable");
            }

            g_terrainTexture = nullptr;
        }

        g_terrainData = nullptr;
    }


    PluginTextureHandle GetTerrainTexture()
    {
        return g_terrainTexture;
    }
}

#endif