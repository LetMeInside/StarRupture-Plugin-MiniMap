// Passes the game-thread-managed target resource to render-thread RHI acquisition.
#if defined(MODLOADER_CLIENT_BUILD)
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
#include <strsafe.h>
#undef Yield
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#define UBT_COMPILED_PLATFORM Windows
#define UBT_COMPILED_TARGET Game
#define PLATFORM_WINDOWS 1
#define PLATFORM_64BITS 1
#define UE_BUILD_SHIPPING 1
#define WITH_EDITOR 0
#define WITH_EDITORONLY_DATA 0
#define WITH_ENGINE 1
#define WITH_COREUOBJECT 1
#define WITH_SERVER_CODE 1
#define WITH_UNREAL_DEVELOPER_TOOLS 0
#define WITH_PLUGIN_SUPPORT 0
#define WITH_PERFCOUNTERS 0
#define IS_MONOLITHIC 1
#define IS_PROGRAM 0
#define STATS 0
#define CORE_API
#define RHI_API
#define RENDERCORE_API
#define TRACELOG_API
#define WITH_MGPU 1
#define WITH_RHI_BREADCRUMBS 0

#include "RHIResources.h"
#include "NativeApi.h"
#include "RHITextureAcquisition.h"
#include "RenderResource.h"

// Returns the borrowed target resource pointer without dereferencing it.
// The renderer must retain and avoid reinitializing the target until the render callback acquires its RHI reference.
void* MiniMapBuildingMapNative::RHITextureAcquisition::GetRenderTargetResourceOnGameThread(
    SDK::UTextureRenderTarget2D* renderTarget)
{
    const auto* api = MiniMapNative::Get();
    if (!renderTarget || !api || !api->player.isGameThread || !api->player.isGameThread() ||
        !api->texture.getRenderTargetResource)
    {
        return nullptr;
    }

    return api->texture.getRenderTargetResource(renderTarget);
}

// Copies the resource's RHI reference into ownedTexture from the render-command callback.
// @param renderTargetResource Borrowed resource returned by the game-thread getter.
// @param ownedTexture Receives an independently retained RHI reference.
// @return True when a non-null texture reference is acquired.
bool MiniMapBuildingMapNative::RHITextureAcquisition::Acquire(
    void* renderTargetResource,
    FTextureRHIRef& ownedTexture)
{
    if (!renderTargetResource)
    {
        return false;
    }

    // Verified PDB base-class chain from the render-target resource to FTexture is all +0.
    auto* texture = reinterpret_cast<FTexture*>(renderTargetResource);
    const FTextureRHIRef& borrowedTexture = texture->GetTextureRHI();
    if (!borrowedTexture)
    {
        return false;
    }

    ownedTexture = borrowedTexture;
    return ownedTexture.GetReference() != nullptr;
}
#endif
