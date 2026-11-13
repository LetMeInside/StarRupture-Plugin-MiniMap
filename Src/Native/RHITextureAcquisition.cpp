// Resolves a retained Unreal render target to an owning FTexture RHI reference.
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

// Resolves the resource in the rendering callback and retains its RHI texture
// before later asynchronous GPU work can depend on it.
bool MiniMapBuildingMapNative::RHITextureAcquisition::Acquire(
    SDK::UTextureRenderTarget2D* renderTarget,
    FTextureRHIRef& ownedTexture)
{
    const auto* api = MiniMapNative::Get();
    if (!renderTarget || !api || !api->texture.getRenderTargetResource)
    {
        return false;
    }

    void* renderTargetResource = api->texture.getRenderTargetResource(renderTarget);
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
