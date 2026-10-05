#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"
#include "Fingerprints.h"
#include "plugin_helpers.h"

namespace
{
    MiniMapNative::NativeApi g_nativeApi = {};
    bool g_initialized = false;
}


namespace MiniMapNative
{
    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses)
    {
        Shutdown();

        if (addresses.softObjectLoadSynchronous == 0 ||
            addresses.getBrushResourceAsTexture2D == 0 ||
            addresses.setForceMipLevelsToBeResident == 0 ||
            addresses.waitForStreaming == 0 ||
            addresses.getNumResidentMips == 0 ||
            addresses.getNumMipsAllowed == 0 ||
            addresses.getNumMips == 0 ||
            addresses.streamIn == 0 ||
            addresses.waitForPendingInitOrStreaming == 0 ||
            addresses.getFirstPlayerController == 0 ||
            addresses.getPlayerPawn == 0 ||
            addresses.getControlRotation == 0 ||
            addresses.isPlayerInForgottenEngine == 0 ||
            addresses.getComponentLocation == 0 ||
            addresses.getPlatformData == 0 ||
            addresses.getBulkDataSize == 0 ||
            addresses.canLoadFromDisk == 0 ||
            addresses.getBulkDataCopy == 0 ||
            addresses.memoryFree == 0)
        {
            return false;
        }

        g_nativeApi.asset.loadSynchronous =
            reinterpret_cast<AssetApi::LoadSynchronousFn>(
                addresses.softObjectLoadSynchronous);

        g_nativeApi.player.getFirstPlayerController =
            reinterpret_cast<PlayerApi::GetFirstPlayerControllerFn>(
                addresses.getFirstPlayerController);

        g_nativeApi.player.getPlayerPawn =
            reinterpret_cast<PlayerApi::GetPlayerPawnFn>(
                addresses.getPlayerPawn);

        g_nativeApi.player.getControlRotation =
            reinterpret_cast<PlayerApi::GetControlRotationFn>(
                addresses.getControlRotation);

        g_nativeApi.player.isPlayerInForgottenEngine =
            reinterpret_cast<PlayerApi::IsPlayerInForgottenEngineFn>(
                addresses.isPlayerInForgottenEngine);

        g_nativeApi.player.getComponentLocation =
            reinterpret_cast<PlayerApi::GetComponentLocationFn>(
                addresses.getComponentLocation);

        g_nativeApi.texture.getBrushTexture =
            reinterpret_cast<TextureApi::GetBrushTextureFn>(
                addresses.getBrushResourceAsTexture2D);

        g_nativeApi.texture.setForceMipLevelsToBeResident =
            reinterpret_cast<
            TextureApi::SetForceMipLevelsToBeResidentFn>(
                addresses.setForceMipLevelsToBeResident);

        g_nativeApi.texture.waitForStreaming =
            reinterpret_cast<TextureApi::WaitForStreamingFn>(
                addresses.waitForStreaming);

        g_nativeApi.texture.getNumResidentMips =
            reinterpret_cast<TextureApi::GetNumResidentMipsFn>(
                addresses.getNumResidentMips);

        g_nativeApi.texture.getNumMipsAllowed =
            reinterpret_cast<TextureApi::GetNumMipsAllowedFn>(
                addresses.getNumMipsAllowed);

        g_nativeApi.texture.getNumMips =
            reinterpret_cast<TextureApi::GetNumMipsFn>(
                addresses.getNumMips);

        g_nativeApi.texture.streamIn =
            reinterpret_cast<TextureApi::StreamInFn>(
                addresses.streamIn);

        g_nativeApi.texture.waitForPendingInitOrStreaming =
            reinterpret_cast<
            TextureApi::WaitForPendingInitOrStreamingFn>(
                addresses.waitForPendingInitOrStreaming);

        g_nativeApi.texture.getPlatformData =
            reinterpret_cast<TextureApi::GetPlatformDataFn>(
                addresses.getPlatformData);

        g_nativeApi.texture.getBulkDataSize =
            reinterpret_cast<TextureApi::GetBulkDataSizeFn>(
                addresses.getBulkDataSize);

        g_nativeApi.texture.canLoadFromDisk =
            reinterpret_cast<TextureApi::CanLoadFromDiskFn>(
                addresses.canLoadFromDisk);

        g_nativeApi.texture.getBulkDataCopy =
            reinterpret_cast<TextureApi::GetBulkDataCopyFn>(
                addresses.getBulkDataCopy);

        g_nativeApi.texture.memoryFree =
            reinterpret_cast<TextureApi::MemoryFreeFn>(
                addresses.memoryFree);

        g_initialized = true;

        LOG_INFO(
            "MiniMap: native API initialized");

        return true;
    }


    void Shutdown()
    {
        g_nativeApi = {};
        g_initialized = false;
    }


    bool IsInitialized()
    {
        return g_initialized;
    }


    const NativeApi* Get()
    {
        if (!g_initialized)
        {
            return nullptr;
        }

        return &g_nativeApi;
    }
}

#endif