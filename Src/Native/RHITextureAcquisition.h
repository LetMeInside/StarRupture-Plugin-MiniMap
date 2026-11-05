#pragma once

// Native Unreal RHI boundary for resolving a render-target UObject to an
// independently owned FTexture RHI reference using the verified FTexture ABI.
#if defined(MODLOADER_CLIENT_BUILD)
#include "RHIResources.h"

namespace SDK
{
    class UTextureRenderTarget2D;
}

namespace MiniMapBuildingMapNative
{
    // Performs the verified render-target resource conversion without exposing
    // Unreal layout assumptions to Building Map code.
    class RHITextureAcquisition final
    {
    public:
        // Resolves the resource on the rendering thread and copies its borrowed
        // FTexture RHI reference into ownedTexture.
        // Returns false if the resource or RHI texture is unavailable.
        static bool Acquire(
            SDK::UTextureRenderTarget2D* renderTarget,
            FTextureRHIRef& ownedTexture);

    private:
        // This utility type groups native acquisition and has no instances.
        RHITextureAcquisition() = delete;
    };
}
#endif
