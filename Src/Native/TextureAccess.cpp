#if defined(MODLOADER_CLIENT_BUILD)

#include "TextureAccess.h"
#include "NativeApi.h"

#include <cstddef>
#include <cstdint>

namespace
{
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


    constexpr size_t kUTextureSRGBFlagsOffset =
        0x106;

    constexpr uint8_t kUTextureSRGBFlag =
        0x01;
}


namespace MiniMapNative::TextureAccess
{
    bool QueryPlatformData(
        SDK::UTexture2D* texture,
        PlatformData& outPlatformData)
    {
        outPlatformData = {};

        if (texture == nullptr)
        {
            return false;
        }

        const NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getPlatformData == nullptr)
        {
            return false;
        }

        void* nativePlatformData =
            native->texture.getPlatformData(
                texture);

        if (nativePlatformData == nullptr)
        {
            return false;
        }

        const auto* platformData =
            reinterpret_cast<
            const NativeTexturePlatformDataLayout*>(
                nativePlatformData);

        const uint8_t textureFlags =
            *reinterpret_cast<const uint8_t*>(
                reinterpret_cast<const uint8_t*>(
                    texture) +
                kUTextureSRGBFlagsOffset);

        outPlatformData.SizeX =
            platformData->SizeX;

        outPlatformData.SizeY =
            platformData->SizeY;

        outPlatformData.PackedData =
            platformData->PackedData;

        outPlatformData.PixelFormat =
            platformData->PixelFormat;

        outPlatformData.MipCount =
            platformData->MipCount;

        outPlatformData.MipCapacity =
            platformData->MipCapacity;

        outPlatformData.MipPointers =
            platformData->MipPointers;

        outPlatformData.HasMipPointers =
            platformData->MipPointers != nullptr;

        outPlatformData.VTData =
            platformData->VTData;

        outPlatformData.CPUCopy =
            platformData->CPUCopy;

        outPlatformData.SRGB =
            (textureFlags & kUTextureSRGBFlag) != 0;

        outPlatformData.NativeData =
            nativePlatformData;

        return true;
    }


    bool QueryMipData(
        const PlatformData& platformData,
        int32_t mipIndex,
        MipData& outMipData)
    {
        outMipData = {};

        if (platformData.NativeData == nullptr ||
            !platformData.HasMipPointers ||
            mipIndex < 0 ||
            mipIndex >= platformData.MipCount)
        {
            return false;
        }

        const NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBulkDataSize == nullptr ||
            native->texture.canLoadFromDisk == nullptr)
        {
            return false;
        }

        const auto* nativePlatformData =
            reinterpret_cast<
            const NativeTexturePlatformDataLayout*>(
                platformData.NativeData);

        if (nativePlatformData->MipPointers == nullptr)
        {
            return false;
        }

        const auto* mipData =
            reinterpret_cast<
            const NativeTexture2DMipMapLayout*>(
                nativePlatformData->
                MipPointers[mipIndex]);

        if (mipData == nullptr)
        {
            return false;
        }

        const void* bulkData =
            static_cast<const void*>(
                mipData->BulkData);

        outMipData.SizeX =
            mipData->SizeX;

        outMipData.SizeY =
            mipData->SizeY;

        outMipData.SizeZ =
            mipData->SizeZ;

        outMipData.BulkData =
            bulkData;

        outMipData.BulkSize =
            native->texture.getBulkDataSize(
                bulkData);

        outMipData.CanLoadFromDisk =
            native->texture.canLoadFromDisk(
                bulkData);

        return true;
    }


    bool CopyBulkData(
        const MipData& mipData,
        void** destination,
        bool discardInternalCopy)
    {
        if (destination == nullptr ||
            mipData.BulkData == nullptr)
        {
            return false;
        }

        *destination = nullptr;

        const NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBulkDataCopy == nullptr)
        {
            return false;
        }

        native->texture.getBulkDataCopy(
            const_cast<void*>(
                mipData.BulkData),
            destination,
            discardInternalCopy);

        return *destination != nullptr;
    }


    void FreeBulkDataCopy(
        void* allocation)
    {
        if (allocation == nullptr)
        {
            return;
        }

        const NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.memoryFree == nullptr)
        {
            return;
        }

        native->texture.memoryFree(
            allocation);
    }
}

#endif
