#pragma once

// Native Unreal RHI boundary for passing a render-target resource from the
// game thread to render-thread acquisition of an independently owned RHI ref.
#if defined(MODLOADER_CLIENT_BUILD)
#include "RHIResources.h"

namespace SDK
{
    class UTextureRenderTarget2D;
}

namespace MiniMapBuildingMapNative
{
    // Separates game-thread resource lookup from render-thread RHI acquisition
    // without exposing Unreal layout assumptions to Building Map code.
    class RHITextureAcquisition final
    {
    public:
        // Gets the game-thread-managed resource pointer without dereferencing it.
        // The caller must keep the render target rooted and unreinitialized until
        // the render thread has acquired its RHI reference.
        // Returns null when the target, API, or game-thread contract is unavailable.
        static void* GetRenderTargetResourceOnGameThread(
            SDK::UTextureRenderTarget2D* renderTarget);

        // Dereferences the borrowed resource on the render thread and copies its
        // FTexture RHI reference into ownedTexture.
        // Returns false if the resource or RHI texture is unavailable.
        static bool Acquire(
            void* renderTargetResource,
            FTextureRHIRef& ownedTexture);

    private:
        // This utility type groups native acquisition and has no instances.
        RHITextureAcquisition() = delete;
    };
}
#endif
