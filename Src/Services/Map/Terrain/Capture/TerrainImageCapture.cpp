// Submits warm-up and final renders of one orthographic view into a borrowed private scene.
// Native references and RHI-thread fences protect capture resources independently of world ownership.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainImageCapture.h"
#include "../Processing/TerrainBc1Codec.h"
#include "Native/NativeApi.h"
#include "Services/Log.h"
#include "SDK/Engine_classes.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace Services::Map::Terrain::Capture
{
    /** Leaves all native work deferred until explicit submission. */
    TerrainImageCapture::TerrainImageCapture() = default;

    /** Never silently discards outstanding scene work or cleanup debt. */
    TerrainImageCapture::~TerrainImageCapture()
    {
        if (status_ != Status::Inactive && status_ != Status::Retired)
            std::terminate();
    }

    /** Uses only typed native predicates; no arbitrary object-memory probing is performed. */
    bool TerrainImageCapture::CanAdvance()
    {
        const auto* api = MiniMapNative::Get();
        return MiniMapNative::IsInitialized() && api && api->captureAdmission.IsComplete() &&
            api->player.isGameThread && api->player.isGameThread() &&
            api->world.isGarbageCollectingAndLocking && !api->world.isGarbageCollectingAndLocking() &&
            api->world.isIncrementalPurgePending && !api->world.isIncrementalPurgePending() &&
            api->world.isIncrementalReachabilityPending && !api->world.isIncrementalReachabilityPending() &&
            api->asset.exitPurge && !*api->asset.exitPurge && api->asset.spawnObject &&
            api->asset.setObjectFlags && api->asset.retainObject && api->asset.releaseObject &&
            api->player.registerComponent && api->player.unregisterComponent &&
            api->player.destroyComponent && api->world.destroyActor &&
            api->texture.initRenderTarget && api->texture.renderFenceConstruct &&
            api->texture.renderFenceBegin && api->texture.renderFencePoll && api->texture.renderFenceDestroy;
    }

    /** Requires exact independent registration of this axis-aligned canvas and finite float projection scales. */
    bool TerrainImageCapture::IsGeometrySupported(const ITerrainDataProvider::CaptureDefinition& definition)
    {
        const auto& bounds = definition.CaptureBounds;
        const auto& registration = definition.ImageRegistration;
        std::uint32_t supersampledWidth = 0;
        std::uint32_t supersampledHeight = 0;
        if (!bounds.IsValid() || !registration.IsValid() ||
            !TerrainImage::IsValidSourceDimensions(definition.WidthPixels, definition.HeightPixels) ||
            !GetSupersampledDimensions(definition, supersampledWidth, supersampledHeight))
            return false;
        const double width = bounds.Maximum.X - bounds.Minimum.X;
        const double height = bounds.Maximum.Y - bounds.Minimum.Y;
        const double depth = bounds.Maximum.Z - bounds.Minimum.Z;
        if (registration.Origin.X != bounds.Minimum.X || registration.Origin.Y != bounds.Minimum.Y ||
            registration.U.X != width || registration.U.Y != 0 ||
            registration.V.X != 0 || registration.V.Y != height)
            return false;
        // Match the ordinary orthographic frustum as well as the custom projection's XY aspect.
        if (width / height != double(definition.WidthPixels) / definition.HeightPixels)
            return false;
        for (double value : {width, height, depth, 2.0 / width, 2.0 / height, 1.0 / depth})
        {
            const float nativeValue = static_cast<float>(value);
            if (!std::isfinite(nativeValue) || nativeValue <= 0)
                return false;
        }
        return true;
    }

    /** Doubles dimensions only when both final and internal extents pass the existing image bounds. */
    bool TerrainImageCapture::GetSupersampledDimensions(
        const ITerrainDataProvider::CaptureDefinition& definition,
        std::uint32_t& widthPixels, std::uint32_t& heightPixels) noexcept
    {
        const auto maximumDimension = (std::numeric_limits<std::uint32_t>::max)() /
            kCaptureSupersamplingFactor;
        if (definition.WidthPixels > maximumDimension || definition.HeightPixels > maximumDimension)
            return false;
        const auto supersampledWidth = definition.WidthPixels * kCaptureSupersamplingFactor;
        const auto supersampledHeight = definition.HeightPixels * kCaptureSupersamplingFactor;
        if (!TerrainImage::IsValidSourceDimensions(supersampledWidth, supersampledHeight))
            return false;
        widthPixels = supersampledWidth;
        heightPixels = supersampledHeight;
        return true;
    }

    /** Rejects unsupported metadata before allocation; setup failure preserves explicit cleanup ownership. */
    bool TerrainImageCapture::Submit(SDK::UWorld* residentWorld,
        const ITerrainDataProvider::CaptureDefinition& definition, bool retainDiagnosticPixels,
        const SDK::AActor* streamingSourceActor)
    {
        if (status_ != Status::Inactive)
            return false;
        if (!definition.Source.IsValid() || !IsGeometrySupported(definition))
        {
            reason_ = "Unsupported terrain capture geometry, registration or dimensions.";
            return false;
        }
        if (!CanAdvance() || !residentWorld ||
            !MiniMapNative::Get()->captureAdmission.isGameWorld(residentWorld) ||
            !MiniMapNative::Get()->captureAdmission.isPreviewWorld(residentWorld) ||
            residentWorld->NetDriver || residentWorld->DemoNetDriver || residentWorld->OwningGameInstance ||
            !residentWorld->PersistentLevel || residentWorld->PersistentLevel->OwningWorld != residentWorld ||
            !residentWorld->PersistentLevel->WorldSettings ||
            !residentWorld->PersistentLevel->WorldSettings->WorldPartition ||
            !MiniMapNative::Get()->captureAdmission.isPartitionInitialized(
                residentWorld->PersistentLevel->WorldSettings->WorldPartition))
        {
            reason_ = "Capture requires an admitted, initialized resident private GamePreview world borrow.";
            return false;
        }
        definition_ = definition;
        world_ = residentWorld;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        retainDiagnosticPixels_ = retainDiagnosticPixels;
#else
        (void)retainDiagnosticPixels;
#endif
        status_ = Status::Preparing;
        reason_.clear();
        advancing_ = true;
        try
        {
            if (CreateResources() && ApplyComponentConfiguration(streamingSourceActor))
            {
                MiniMapNative::Get()->captureAdmission.sendEndOfFrame(world_);
                native_.BeginFence();
                advancing_ = false;
                return true;
            }
        }
        catch (...)
        {
            // Native owners stay attached to this operation even when setup is interrupted.
        }
        reason_ = "Native terrain capture setup failed; retirement is required.";
        RequestRetirement();
        advancing_ = false;
        return false;
    }

    /** Retains the spawned actor and constructs its capture root without actor reflection. */
    bool TerrainImageCapture::CreateResources()
    {
        const auto* api = MiniMapNative::Get();
        SDK::FTransform identity{};
        identity.Rotation.W = 1;
        identity.Scale3D = SDK::FVector(1, 1, 1);
        actor_ = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world_, SDK::AActor::StaticClass(),
            identity, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
            SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        if (!actor_)
            return false;
        api->asset.retainObject(actor_);
        if (SDK::UGameplayStatics::FinishSpawningActor(actor_, identity,
            SDK::ESpawnActorScaleMethod::MultiplyWithRoot) != actor_)
            return false;
        if (actor_->RootComponent || !world_->PersistentLevel || actor_->Outer != world_->PersistentLevel ||
            world_->PersistentLevel->OwningWorld != world_ || !PrivateWorldNative::CanCreateComponentInWorld(world_))
            return false;
        auto* captureClass = SDK::USceneCaptureComponent2D::StaticClass();
        if (!captureClass || captureClass->GetFullName() != "Class Engine.SceneCaptureComponent2D")
            return false;
        SDK::TSubclassOf<SDK::UObject> componentClass(captureClass);
        component_ = static_cast<SDK::USceneCaptureComponent2D*>(api->asset.spawnObject(&componentClass, actor_));
        if (!component_)
            return false;
        api->asset.retainObject(component_);
        if (component_->GetOwner() != actor_)
            return false;
        SDK::TSubclassOf<SDK::UObject> targetClass(SDK::UTextureRenderTarget2D::StaticClass());
        target_ = static_cast<SDK::UTextureRenderTarget2D*>(api->asset.spawnObject(&targetClass, world_));
        if (!target_)
            return false;
        api->asset.retainObject(target_);
        api->asset.setObjectFlags(target_, 0x40);
        ConfigureResources();
        ConfigureProjection();
        ConfigureBackground();
        api->texture.initRenderTarget(target_,
            definition_.WidthPixels * kCaptureSupersamplingFactor,
            definition_.HeightPixels * kCaptureSupersamplingFactor,
            static_cast<std::uint8_t>(SDK::EPixelFormat::PF_B8G8R8A8), !definition_.SRgb);
        component_->TextureTarget = target_;
        actor_->RootComponent = component_;
        api->player.registerComponent(component_, world_, nullptr);
        registered_ = PrivateWorldNative::IsComponentRegistered(component_);
        if (!registered_)
            return false;
        const auto& bounds = definition_.CaptureBounds;
        component_->K2_SetWorldLocationAndRotation(
            SDK::FVector(bounds.Minimum.X + definition_.ImageRegistration.U.X * 0.5,
                bounds.Minimum.Y + definition_.ImageRegistration.V.Y * 0.5, bounds.Maximum.Z),
            SDK::FRotator(-90, -90, 0), false, nullptr, true);
        return actor_->RootComponent == component_ && component_->GetOwner() == actor_ &&
            actor_->Outer == world_->PersistentLevel;
    }

    /** Applies the same mesh selection in every client and optionally reports this inventory. */
    bool TerrainImageCapture::ApplyComponentConfiguration(const SDK::AActor* streamingSourceActor)
    {
#if !MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (definition_.ExcludedStaticMeshAssets.empty())
            return true;
#endif
        const auto components = CollectResidentComponents(streamingSourceActor);
        if (!ApplyMeshExclusions(components))
            return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        ObserveAuthoredLocalLightReadiness(components);
#endif
        return true;
    }

    /** Borrows only owned components of live actors from levels belonging to this capture world. */
    std::vector<SDK::UActorComponent*> TerrainImageCapture::CollectResidentComponents(
        const SDK::AActor* streamingSourceActor) const
    {
        std::vector<SDK::UActorComponent*> components;
        for (const auto* level : world_->Levels)
        {
            if (!level || level->OwningWorld != world_)
                continue;
            for (const auto* actor : level->Actors)
            {
                if (actor && actor != actor_ && actor != streamingSourceActor && actor->Outer == level &&
                    SDK::UKismetSystemLibrary::IsValid(actor))
                    PrivateWorldNative::AppendOwnedComponents(actor, components);
            }
        }
        return components;
    }

    /** Adds registered matches to the capture-local weak list and reports match and insertion totals. */
    bool TerrainImageCapture::ApplyMeshExclusions(std::span<SDK::UActorComponent* const> components)
    {
        if (definition_.ExcludedStaticMeshAssets.empty())
            return true;
        auto* staticMeshComponentClass = SDK::UStaticMeshComponent::StaticClass();
        if (!staticMeshComponentClass)
            return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        std::size_t matched = 0;
        const auto hiddenBefore = component_->HiddenComponents.Num();
#endif
        for (auto* actorComponent : components)
        {
            if (!actorComponent->IsA(staticMeshComponentClass))
                continue;
            auto* staticMeshComponent = static_cast<SDK::UStaticMeshComponent*>(actorComponent);
            if (!MatchesExcludedMesh(staticMeshComponent))
                continue;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            ++matched;
#endif
            if (PrivateWorldNative::IsComponentRegistered(staticMeshComponent))
                component_->HideComponent(staticMeshComponent);
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: terrain capture %s mesh exclusions matched=%zu hidden=%d",
                definition_.Page.CollectionKey.c_str(), matched, component_->HiddenComponents.Num() - hiddenBefore);
#endif
        return true;
    }

    /** Builds the cooked top-level object path from the mesh's owning package and raw object name. */
    bool TerrainImageCapture::MatchesExcludedMesh(const SDK::UStaticMeshComponent* component) const
    {
        const auto* mesh = component->StaticMesh;
        if (!mesh || !mesh->Outer || !mesh->Outer->IsA(SDK::UPackage::StaticClass()))
            return false;
        const auto objectPath = mesh->Outer->Name.GetRawString() + "." + mesh->Name.GetRawString();
        const auto& excludedAssets = definition_.ExcludedStaticMeshAssets;
        return std::find(excludedAssets.begin(), excludedAssets.end(), objectPath) != excludedAssets.end();
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Reports inventory coverage separately from light class matches and native component state. */
    void TerrainImageCapture::ObserveAuthoredLocalLightReadiness(
        std::span<SDK::UActorComponent* const> components) const
    {
        /** Holds the three component counts for one local light type in this inventory. */
        struct LocalLightCounts
        {
            std::size_t Total = 0;
            std::size_t Registered = 0;
            std::size_t Ready = 0;
        };
        LocalLightCounts pointLights;
        LocalLightCounts spotLights;
        auto* pointLightClass = SDK::UPointLightComponent::StaticClass();
        auto* spotLightClass = SDK::USpotLightComponent::StaticClass();
        if (pointLightClass && spotLightClass)
        {
            for (const auto* actorComponent : components)
            {
                const bool isSpot = actorComponent->IsA(spotLightClass);
                if (!isSpot && !actorComponent->IsA(pointLightClass))
                    continue;
                auto& lights = isSpot ? spotLights : pointLights;
                ++lights.Total;
                lights.Registered += PrivateWorldNative::IsComponentRegistered(actorComponent);
                lights.Ready += PrivateWorldNative::IsComponentReady(actorComponent);
            }
        }
        if (auto* log = MiniMap::Services::TryLog())
            MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
                "MiniMap: terrain capture %s enumerated_components=%zu light_classes_resolved=%d "
                "point_total=%zu point_registered=%zu point_ready=%zu spot_total=%zu spot_registered=%zu spot_ready=%zu",
                definition_.Page.CollectionKey.c_str(), components.size(), pointLightClass && spotLightClass,
                pointLights.Total, pointLights.Registered, pointLights.Ready,
                spotLights.Total, spotLights.Registered, spotLights.Ready);
    }
#endif

    /** Disables continuous/main-view capture and initializes an opaque non-mipped display-color target. */
    void TerrainImageCapture::ConfigureResources()
    {
        component_->PrimaryComponentTick.bCanEverTick = false;
        component_->bAutoActivate = false;
        component_->DetailMode = SDK::EDetailMode::DM_Low;
        component_->SetVisibility(true, false);
        component_->SetHiddenInGame(false, false);
        component_->bCaptureEveryFrame = false;
        component_->bCaptureOnMovement = false;
        component_->bAlwaysPersistRenderingState = false;
        component_->bUseRayTracingIfEnabled = false;
        component_->bMainViewFamily = false;
        component_->bMainViewResolution = false;
        component_->bMainViewCamera = false;
        component_->bInheritMainViewCameraPostProcessSettings = false;
        component_->bRenderInMainRenderer = false;
        component_->bEnableOrthographicTiling = false;
        component_->bEnableClipPlane = false;
        component_->PrimitiveRenderMode = SDK::ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        component_->CaptureSource = SDK::ESceneCaptureSource::SCS_FinalColorLDR;
        component_->CompositeMode = SDK::ESceneCaptureCompositeMode::SCCM_Overwrite;
        component_->PostProcessBlendWeight = 0;
        target_->bAutoGenerateMips = false;
        target_->bSupportsUAV = false;
        // Gamma-only resolve writes linear RGB. The target view encodes sRGB when requested.
        target_->TargetGamma = 1.0f;
        float channels[3]{};
        for (std::size_t index = 0; index < 3; ++index)
        {
            const float channel = definition_.OpaqueBackground[index] / 255.0f;
            channels[index] = definition_.SRgb
                ? (channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f))
                : channel;
        }
        target_->ClearColor = SDK::FLinearColor(channels[0], channels[1], channels[2], 1);
    }

    /** Places the camera at maximum Z: depth d=maxZ-worldZ maps to 1-d/(maxZ-minZ).
     * This is projection clipping, not an assertion about renderer culling or Nanite isolation.
     * At yaw -90/pitch -90, screen right is +X and screen down is +Y.
     */
    void TerrainImageCapture::ConfigureProjection()
    {
        component_->ProjectionType = SDK::ECameraProjectionMode::Orthographic;
        component_->OrthoWidth = static_cast<float>(definition_.ImageRegistration.U.X);
        component_->Overscan = 0;
        component_->bAutoCalculateOrthoPlanes = false;
        component_->AutoPlaneShift = 0;
        component_->bUpdateOrthoPlanes = false;
        component_->bUseCameraHeightAsViewTarget = false;
        component_->bUseFauxOrthoViewPos = false;
        component_->bOverride_CustomNearClippingPlane = false;
        SDK::FMatrix projection{};
        projection.XPlane.X = 2.0 / definition_.ImageRegistration.U.X;
        projection.YPlane.Y = 2.0 / definition_.ImageRegistration.V.Y;
        projection.ZPlane.Z = -1.0 / (definition_.CaptureBounds.Maximum.Z - definition_.CaptureBounds.Minimum.Z);
        projection.WPlane.Z = 1;
        projection.WPlane.W = 1;
        component_->CustomProjectionMatrix = projection;
        component_->bUseCustomProjectionMatrix = true;
    }

    /** Selects gamma-only resolve and removes effects that can shade outside the capture geometry. */
    void TerrainImageCapture::ConfigureBackground()
    {
        const wchar_t* names[] = {L"Fog", L"Atmosphere", L"VolumetricFog", L"Cloud", L"MotionBlur",
            L"TemporalAA", L"EyeAdaptation", L"PostProcessing", L"LightShafts", L"DynamicShadows",
            L"DistanceFieldAO"};
        constexpr int flagCount = static_cast<int>(std::size(names));
        SDK::FEngineShowFlagsSetting flags[flagCount]{};
        for (int index = 0; index < flagCount; ++index)
        {
            flags[index].ShowFlagName = SDK::FString(names[index]);
            flags[index].Enabled = false;
        }
        SDK::TArray<SDK::FEngineShowFlagsSetting> settings(flags, flagCount, flagCount);
        component_->SetShowFlagSettings(settings);
    }

    /** Keeps native retirement separate from logical eligibility and from genuine GPU readback completion. */
    void TerrainImageCapture::Advance()
    {
        if (advancing_ || !CanAdvance())
            return;
        advancing_ = true;
        try
        {
            if (status_ == Status::WarmingUp && warmupTicks_ < kCaptureWarmupTicks)
            {
                const auto frame = SDK::UKismetSystemLibrary::GetFrameCount();
                if (frame != lastWarmupFrame_)
                {
                    lastWarmupFrame_ = frame;
                    ++warmupTicks_;
                }
            }
            if (!native_.PollFence())
            {
                advancing_ = false;
                return;
            }
            if (status_ == Status::Retiring)
            {
                AdvanceRetirement();
                advancing_ = false;
                return;
            }
            if (status_ == Status::Preparing || status_ == Status::WarmingUp || status_ == Status::Submitted)
                AdvanceSubmission();
            else if (status_ == Status::ReadingBack)
                AdvanceReadback();
        }
        catch (...)
        {
            reason_ = "Terrain capture/readback/encoding failed; retirement is required.";
            RequestRetirement();
        }
        advancing_ = false;
    }

    /** Reuses the same scene and target for both submissions, then reads only the final image. */
    void TerrainImageCapture::AdvanceSubmission()
    {
        if (status_ == Status::Preparing)
        {
            if (!TerrainImageReadback::InitializeNativeCapture())
            {
                reason_ = "Native capture GPU configuration is unavailable.";
                RequestRetirement();
                return;
            }
            lastWarmupFrame_ = SDK::UKismetSystemLibrary::GetFrameCount();
            status_ = Status::WarmingUp;
            SubmitSceneCapture();
            return;
        }
        if (status_ == Status::WarmingUp)
        {
            if (warmupTicks_ < kCaptureWarmupTicks)
                return;
            status_ = Status::Submitted;
            SubmitSceneCapture();
            return;
        }
        if (!readback_.Start(target_,
            definition_.WidthPixels * kCaptureSupersamplingFactor,
            definition_.HeightPixels * kCaptureSupersamplingFactor))
        {
            reason_ = "Native terrain render-target readback could not start.";
            RequestRetirement();
            return;
        }
        status_ = Status::ReadingBack;
    }

    /** Marks GPU retirement debt before either explicit submission can enqueue native work. */
    void TerrainImageCapture::SubmitSceneCapture()
    {
        sceneCaptureAttempted_ = true;
        component_->CaptureScene();
        native_.BeginFence();
    }

    /** Never consumes RGB until GPU objects and their submitted native callables have retired. */
    void TerrainImageCapture::AdvanceReadback()
    {
        const auto result = readback_.Advance();
        if (result == TerrainImageReadback::Status::Failed)
        {
            reason_ = "Native terrain readback failed; outstanding ownership must still retire.";
            RequestRetirement();
        }
        else if (result == TerrainImageReadback::Status::Complete && readback_.IsRetired())
        {
            if (!CompleteImage())
            {
                reason_ = "Terrain readback RGB or completed BC1 image validation failed.";
                RequestRetirement();
                return;
            }
            readback_.Reset();
            status_ = Status::Ready;
        }
    }

    /** Decodes one sRGB channel to linear light using the standard IEC 61966-2-1 transfer curve. */
    double TerrainImageCapture::DecodeSrgbChannel(std::uint8_t encoded) noexcept
    {
        const double value = static_cast<double>(encoded) / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }

    /** Encodes one linear-light channel with nearest-byte rounding and finite range clamping. */
    std::uint8_t TerrainImageCapture::EncodeSrgbChannel(double linear) noexcept
    {
        const double encoded = linear <= 0.0031308
            ? linear * 12.92
            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        return static_cast<std::uint8_t>(std::lround((std::clamp)(encoded, 0.0, 1.0) * 255.0));
    }

    /** Reduces each exact 2x2 source footprint to one opaque RGB output pixel. */
    bool TerrainImageCapture::DownsampleRgb(std::span<const std::uint8_t> supersampledPixels,
        std::uint32_t supersampledWidth, std::uint32_t supersampledHeight,
        std::uint32_t finalWidth, std::uint32_t finalHeight, bool sRgb,
        std::vector<std::uint8_t>& finalPixels)
    {
        if (finalWidth > (std::numeric_limits<std::uint32_t>::max)() / kCaptureSupersamplingFactor ||
            finalHeight > (std::numeric_limits<std::uint32_t>::max)() / kCaptureSupersamplingFactor ||
            supersampledWidth != finalWidth * kCaptureSupersamplingFactor ||
            supersampledHeight != finalHeight * kCaptureSupersamplingFactor ||
            !TerrainImage::IsValidSourceDimensions(finalWidth, finalHeight) ||
            !TerrainImage::IsValidSourceDimensions(supersampledWidth, supersampledHeight))
            return false;
        const auto sourceByteCount = static_cast<std::size_t>(supersampledWidth) * supersampledHeight * 3u;
        if (supersampledPixels.size() != sourceByteCount)
            return false;
        std::vector<std::uint8_t> result(static_cast<std::size_t>(finalWidth) * finalHeight * 3u);
        for (std::uint32_t y = 0; y < finalHeight; ++y)
        {
            for (std::uint32_t x = 0; x < finalWidth; ++x)
            {
                const auto destination = (static_cast<std::size_t>(y) * finalWidth + x) * 3u;
                for (std::uint32_t channel = 0; channel < 3; ++channel)
                {
                    const auto first = (static_cast<std::size_t>(y * 2u) * supersampledWidth + x * 2u) * 3u + channel;
                    const auto second = first + 3u;
                    const auto third = first + static_cast<std::size_t>(supersampledWidth) * 3u;
                    const auto fourth = third + 3u;
                    if (sRgb)
                    {
                        const double average = (DecodeSrgbChannel(supersampledPixels[first]) +
                            DecodeSrgbChannel(supersampledPixels[second]) +
                            DecodeSrgbChannel(supersampledPixels[third]) +
                            DecodeSrgbChannel(supersampledPixels[fourth])) * 0.25;
                        result[destination + channel] = EncodeSrgbChannel(average);
                    }
                    else
                    {
                        const std::uint32_t sum = supersampledPixels[first] + supersampledPixels[second] +
                            supersampledPixels[third] + supersampledPixels[fourth];
                        result[destination + channel] = static_cast<std::uint8_t>((sum + 2u) / 4u);
                    }
                }
            }
        }
        finalPixels = std::move(result);
        return true;
    }

    /** Encodes final-resolution RGB without transfer conversion or row flipping; opaque BC1 stays canonical. */
    bool TerrainImageCapture::CompleteImage()
    {
        const auto& supersampledPixels = readback_.Pixels();
        const auto supersampledWidth = definition_.WidthPixels * kCaptureSupersamplingFactor;
        const auto supersampledHeight = definition_.HeightPixels * kCaptureSupersamplingFactor;
        std::vector<std::uint8_t> finalPixels;
        if (!DownsampleRgb(supersampledPixels, supersampledWidth, supersampledHeight,
            definition_.WidthPixels, definition_.HeightPixels, definition_.SRgb, finalPixels))
            return false;
        const auto stride = static_cast<std::size_t>(definition_.WidthPixels) * 3u;
        std::vector<std::uint8_t> blocks;
        if (!Processing::TerrainBc1Codec::EncodeRgb(finalPixels, definition_.WidthPixels,
            definition_.HeightPixels, stride, blocks) ||
            !Processing::TerrainBc1Codec::Validate(blocks, definition_.WidthPixels, definition_.HeightPixels))
            return false;
        image_ = std::make_shared<const TerrainImage>(definition_.Source, definition_.WidthPixels,
            definition_.HeightPixels, definition_.ImageRegistration, definition_.SRgb, std::move(blocks));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (retainDiagnosticPixels_)
        {
            diagnosticPixels_.FinalRgbPixels = std::move(finalPixels);
            diagnosticPixels_.SupersampledRgbPixels = readback_.TakePixels();
            diagnosticPixels_.FinalWidthPixels = definition_.WidthPixels;
            diagnosticPixels_.FinalHeightPixels = definition_.HeightPixels;
            diagnosticPixels_.SupersampledWidthPixels = supersampledWidth;
            diagnosticPixels_.SupersampledHeightPixels = supersampledHeight;
        }
#endif
        return true;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Moves the temporary raw capture samples to the service's immediate diagnostic export. */
    TerrainCaptureDiagnosticPixels TerrainImageCapture::TakeDiagnosticPixels() noexcept
    {
        auto pixels = std::move(diagnosticPixels_);
        diagnosticPixels_ = {};
        return pixels;
    }
#endif

    /** Withdraws output eligibility while retaining all scene resources and the borrowed-world obligation. */
    void TerrainImageCapture::RequestRetirement() noexcept
    {
        image_.reset();
        readback_.Cancel();
        if (status_ != Status::Inactive && status_ != Status::Retired)
            status_ = Status::Retiring;
    }

    /** Retires capture work synchronously only from a safe game-thread lifecycle boundary. */
    bool TerrainImageCapture::RetireSynchronously()
    {
        if (status_ == Status::Inactive || status_ == Status::Retired)
            return true;
        if (advancing_ || !CanAdvance())
            return false;
        advancing_ = true;
        RequestRetirement();
        bool retired = false;
        try
        {
            if (!DrainCaptureRenderWork())
            {
                if (reason_.empty())
                    reason_ = "Synchronous terrain render/readback completion was not acknowledged.";
            }
            else if (!FinishSynchronousRetirement())
            {
                if (reason_.empty())
                    reason_ = "Synchronous terrain scene cleanup was not acknowledged.";
            }
            else
            {
                retired = true;
            }
        }
        catch (...)
        {
            reason_ = "Synchronous terrain capture retirement failed; native ownership remains retained.";
        }
        advancing_ = false;
        return retired && status_ == Status::Retired;
    }

    /** Drains the RHI submission and a real GPU readback fence before private scene teardown. */
    bool TerrainImageCapture::DrainCaptureRenderWork()
    {
        if (!native_.WaitFence())
            return false;
        if (sceneCaptureAttempted_ && !readback_.HasGpuCompletionAcknowledgement() && readback_.IsRetired())
        {
            if (!readback_.Reset() || !readback_.Start(target_,
                definition_.WidthPixels * kCaptureSupersamplingFactor,
                definition_.HeightPixels * kCaptureSupersamplingFactor, true))
                return false;
        }
        readback_.RequestSynchronousRetirement();
        // Each pass submits or joins a callback and blocks on its ordered RHI fence; it never spins on readiness.
        while ((sceneCaptureAttempted_ && !readback_.HasGpuCompletionAcknowledgement()) ||
            !readback_.IsRetired())
        {
            const auto status = readback_.Advance();
            if (status == TerrainImageReadback::Status::Failed && !readback_.IsRetired())
                return false;
            native_.BeginFence();
            if (!native_.WaitFence() || readback_.HasPendingCallbackOwner())
                return false;
            if (readback_.GetStatus() == TerrainImageReadback::Status::Failed &&
                readback_.IsRetired() && !readback_.HasGpuCompletionAcknowledgement())
                return false;
            if (readback_.GetStatus() == TerrainImageReadback::Status::Failed && !readback_.IsRetired())
                return false;
        }
        return readback_.Reset();
    }

    /** Uses the existing component cleanup operations and waits for their submitted RHI fence. */
    bool TerrainImageCapture::FinishSynchronousRetirement()
    {
        AdvanceRetirement();
        if (status_ == Status::Retired)
            return true;
        if (!cleanupSubmitted_ || !native_.WaitFence())
            return false;
        ReleaseResources();
        return status_ == Status::Retired;
    }

    /** Destroys capture-owned scene state once and retains its objects behind a second cleanup fence. */
    void TerrainImageCapture::AdvanceRetirement()
    {
        if (sceneCaptureAttempted_ && !readback_.HasGpuCompletionAcknowledgement())
        {
            if (readback_.IsRetired())
            {
                if (!readback_.Reset() || !readback_.Start(target_,
                    definition_.WidthPixels * kCaptureSupersamplingFactor,
                    definition_.HeightPixels * kCaptureSupersamplingFactor, true))
                    return;
            }
            readback_.RequestGpuCompletionRetirement();
            readback_.Advance();
            return;
        }
        readback_.Advance();
        if (!readback_.IsRetired())
            return;
        readback_.Reset();
        const auto* api = MiniMapNative::Get();
        if (cleanupSubmitted_)
        {
            ReleaseResources();
            return;
        }
        if (registered_)
        {
            api->player.unregisterComponent(component_);
            registered_ = false;
        }
        if (component_ && !componentDestroyed_)
        {
            component_->TextureTarget = nullptr;
            api->player.destroyComponent(component_, false);
            componentDestroyed_ = true;
        }
        if (actor_ && !actorDestroyed_)
        {
            if (actorDestroyAttempted_)
                return;
            actorDestroyAttempted_ = true;
            actorDestroyed_ = api->world.destroyActor(reinterpret_cast<std::uintptr_t>(actor_), false, false);
            if (!actorDestroyed_)
            {
                reason_ = "Capture actor destruction failed; native ownership remains retained.";
                return;
            }
        }
        api->captureAdmission.sendEndOfFrame(world_);
        native_.BeginFence();
        cleanupSubmitted_ = true;
    }

    /** Releases only operation-owned roots; world residency and teardown remain with the session. */
    void TerrainImageCapture::ReleaseResources()
    {
        const auto* api = MiniMapNative::Get();
        if (target_)
            api->asset.releaseObject(target_);
        target_ = nullptr;
        if (component_)
            api->asset.releaseObject(component_);
        component_ = nullptr;
        if (actor_)
            api->asset.releaseObject(actor_);
        actor_ = nullptr;
        world_ = nullptr;
        status_ = Status::Retired;
    }

    /** Exposes no target after cancellation or before completed image eligibility. */
    SDK::UTextureRenderTarget2D* TerrainImageCapture::GetRenderTarget() const noexcept
    {
        return status_ == Status::Ready ? target_ : nullptr;
    }

    /** Shares immutable BC1 independently of the native target, readback and borrowed world. */
    std::shared_ptr<const TerrainImage> TerrainImageCapture::GetImage() const noexcept
    {
        return status_ == Status::Ready ? image_ : nullptr;
    }
}
#endif
