#pragma once
#include <Engine_classes.hpp>
#include <algorithm>

namespace MiniMapSpriteCapture
{
    // Private-world camera/render configuration.
    inline void ConfigureSceneCapture(SDK::USceneCaptureComponent2D* capture, const SDK::FVector& extent)
    {
        capture->PrimaryComponentTick.bCanEverTick=false; capture->bReplicates=false;
        capture->bAutoActivate=false; capture->bCanEverAffectNavigation=false;
        capture->bCaptureEveryFrame=false;capture->bCaptureOnMovement=false;
        capture->bAlwaysPersistRenderingState=false;capture->bUseRayTracingIfEnabled=false;
        capture->ProjectionType=SDK::ECameraProjectionMode::Orthographic;
        capture->PrimitiveRenderMode=SDK::ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        capture->CaptureSource=SDK::ESceneCaptureSource::SCS_SceneColorHDR;
        capture->CompositeMode=SDK::ESceneCaptureCompositeMode::SCCM_Overwrite;
        capture->OrthoWidth=float(2.0*(std::max)(extent.X,extent.Y)*1.1);
        capture->bAutoCalculateOrthoPlanes=true;capture->bUpdateOrthoPlanes=true;
        capture->bUseCameraHeightAsViewTarget=false; capture->bEnableOrthographicTiling=false;
        capture->bRenderInMainRenderer=false;capture->bMainViewFamily=false;
        capture->bMainViewResolution=false;capture->bMainViewCamera=false;
        capture->bInheritMainViewCameraPostProcessSettings=false;
        capture->PostProcessBlendWeight=0;capture->bEnableClipPlane=false;
        capture->bUseCustomProjectionMatrix=false;
    }
}
