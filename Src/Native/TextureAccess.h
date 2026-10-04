#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>

namespace SDK
{
    class UTexture2D;
}

namespace MiniMapNative::TextureAccess
{
    struct PlatformData
    {
        int32_t SizeX = 0;
        int32_t SizeY = 0;
        uint32_t PackedData = 0;
        uint8_t PixelFormat = 0;

        int32_t MipCount = 0;
        int32_t MipCapacity = 0;

        const void* MipPointers = nullptr;
        bool HasMipPointers = false;

        const void* VTData = nullptr;
        const void* CPUCopy = nullptr;

        bool SRGB = false;

        const void* NativeData = nullptr;
    };


    struct MipData
    {
        uint16_t SizeX = 0;
        uint16_t SizeY = 0;
        uint16_t SizeZ = 0;

        const void* BulkData = nullptr;
        int64_t BulkSize = 0;
        bool CanLoadFromDisk = false;
    };


    bool QueryPlatformData(
        SDK::UTexture2D* texture,
        PlatformData& outPlatformData);

    bool QueryMipData(
        const PlatformData& platformData,
        int32_t mipIndex,
        MipData& outMipData);

    bool CopyBulkData(
        const MipData& mipData,
        void** destination,
        bool discardInternalCopy);

    void FreeBulkDataCopy(
        void* allocation);
}

#endif
