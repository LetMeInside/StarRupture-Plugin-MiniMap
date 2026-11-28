// Uses genuine engine headers exclusively; the SDK-facing boundary contains only opaque object pointers.
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/PrivateStreamingUEConfiguration.h"
#include "CoreMinimal.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "Misc/Timeout.h"
#include "RenderCommandFence.h"
#include "PrivateWorldNative.h"
#include "PrivateStreamingDefinition.h"
#include "Native/NativeApi.h"
#include <exception>
#include <cstddef>

namespace Services::Map::Terrain::Capture
{

    /** Uses actual constructor/destructor bindings on storage for the genuine FRenderCommandFence type. */
    struct PrivateWorldNative::State final
    {
        alignas(FRenderCommandFence) std::byte Storage[sizeof(FRenderCommandFence)]{};
        bool Active = false;
        static_assert(sizeof(FRenderCommandFence) == 8 && alignof(FRenderCommandFence) == 8);
        static_assert(sizeof(ULevel) == 1008 && offsetof(ULevel, OwningWorld) == 192);
        static_assert(offsetof(ULevel, Actors) == 160 && sizeof(UActorComponent) == 184);
        static_assert(sizeof(TOptional<const UE::FTimeout>) == 24);
        static_assert(UE_WITH_REMOTE_OBJECT_HANDLE == 0 && UE_ENABLE_ARRAY_SLACK_TRACKING == 0);
    };

    /** Does not construct a native fence until submissions actually exist. */
    PrivateWorldNative::PrivateWorldNative() : state_(std::make_unique<State>()) {}

    /** Prevents silent release of an unretired native task/fence reference. */
    PrivateWorldNative::~PrivateWorldNative()
    {
        if (state_->Active)
            std::terminate();
    }

    /** Converts the validated complete subsystem to the receiver expected by the resolved virtual entry. */
    void PrivateWorldNative::AdvancePartition(SDK::UWorldPartitionSubsystem* subsystem)
    {
        auto* nativeSubsystem = reinterpret_cast<::UWorldPartitionSubsystem*>(subsystem);
        auto* streamingInterface = static_cast<::IStreamingWorldSubsystemInterface*>(nativeSubsystem);
        MiniMapNative::Get()->captureAdmission.updatePartition(streamingInterface);
    }

    /** Restores the exact native overload at the isolated ABI boundary. */
    void PrivateWorldNative::AdvanceLevels(SDK::UWorld* world)
    {
        using Update = void(::UWorld*, const TOptional<const UE::FTimeout>&);
        const TOptional<const UE::FTimeout> timeout;
        reinterpret_cast<Update*>(MiniMapNative::Get()->captureAdmission.updateLevels)(
            reinterpret_cast<::UWorld*>(world), timeout);
    }

    /** Requests only native per-world visibility drainage after observed private package requests were waited. */
    void PrivateWorldNative::FlushVisibility(SDK::UWorld* world)
    {
        using Flush = void(::UWorld*, EFlushLevelStreamingType);
        reinterpret_cast<Flush*>(MiniMapNative::Get()->captureAdmission.flushVisibility)(
            reinterpret_cast<::UWorld*>(world), EFlushLevelStreamingType::Visibility);
    }

    /** Visibility is checked together with completed registration, never as a completion-only shortcut. */
    bool PrivateWorldNative::IsLevelReady(const SDK::ULevel* level)
    {
        const auto* nativeLevel = reinterpret_cast<const ULevel*>(level);
        // Native AddToWorld destroys its async registration context before completing visibility.
        return nativeLevel && nativeLevel->bAreComponentsCurrentlyRegistered && nativeLevel->bIsVisible;
    }

    /** Checks the native world state before direct component construction or registration. */
    bool PrivateWorldNative::CanCreateComponentInWorld(const SDK::UWorld* world)
    {
        const auto* nativeWorld = reinterpret_cast<const UWorld*>(world);
        return nativeWorld && !nativeWorld->bIsTearingDown;
    }

    /** Reads the genuine component registration state through the isolated Engine-header boundary. */
    bool PrivateWorldNative::IsComponentRegistered(const SDK::UActorComponent* component)
    {
        const auto* nativeComponent = reinterpret_cast<const UActorComponent*>(component);
        return nativeComponent && nativeComponent->IsRegistered();
    }

    /** Requires both registration and actual render-state creation for the configured scene component. */
    bool PrivateWorldNative::IsComponentReady(const SDK::UActorComponent* component)
    {
        const auto* nativeComponent = reinterpret_cast<const UActorComponent*>(component);
        return nativeComponent && nativeComponent->IsRegistered() && nativeComponent->IsRenderStateCreated();
    }

    /** Allocates native-owned grid/shape arrays with the engine allocator, never an SDK container allocator. */
    bool PrivateWorldNative::ConfigureSource(SDK::UWorldPartitionStreamingSourceComponent* source,
        const PrivateStreamingDefinition& definition, const char** failureReason)
    {
        static_assert(sizeof(UWorldPartitionStreamingSourceComponent) == 280);
        static_assert(offsetof(UWorldPartitionStreamingSourceComponent, TargetGrids) == 200);
        static_assert(offsetof(UWorldPartitionStreamingSourceComponent, Shapes) == 256);
        static_assert(sizeof(FStreamingSourceShape) == 72);
        if (failureReason)
            *failureReason = nullptr;
        auto* nativeSource = reinterpret_cast<UWorldPartitionStreamingSourceComponent*>(source);
        if (!nativeSource)
        {
            if (failureReason)
                *failureReason = "PrivateWorldNative::ConfigureSource received a null source component.";
            return false;
        }
        if (nativeSource->IsRegistered())
        {
            if (failureReason)
                *failureReason = "PrivateWorldNative::ConfigureSource requires the source component to be unregistered.";
            return false;
        }
        if (definition.TargetGrids.empty())
        {
            if (failureReason)
                *failureReason = "PrivateWorldNative::ConfigureSource received no target grids.";
            return false;
        }
        nativeSource->TargetGrids.Reset();
        for (const auto& grid : definition.TargetGrids)
            nativeSource->TargetGrids.Add(FName::CreateFromDisplayId(
                FNameEntryId::FromUnstableInt(grid.ComparisonIndex), grid.Number));
        nativeSource->Shapes.Reset();
        FStreamingSourceShape shape;
        shape.bUseGridLoadingRange = false;
        shape.Radius = definition.Radius;
        nativeSource->Shapes.Add(shape);
        return true;
    }

    /** Reuses the existing native fence bindings and the genuine enum value for RHI submission retirement. */
    void PrivateWorldNative::BeginFence()
    {
        if (state_->Active)
            std::terminate();
        const auto& texture = MiniMapNative::Get()->texture;
        texture.renderFenceConstruct(state_->Storage);
        state_->Active = true;
        texture.renderFenceBegin(state_->Storage, static_cast<int32>(FRenderCommandFence::ESyncDepth::RHIThread));
    }

    /** Destroys the fence only following its real native completion acknowledgement. */
    bool PrivateWorldNative::PollFence()
    {
        if (!state_->Active)
            return true;
        const auto& texture = MiniMapNative::Get()->texture;
        if (!texture.renderFencePoll(state_->Storage))
            return false;
        texture.renderFenceDestroy(state_->Storage);
        state_->Active = false;
        return true;
    }

    /** Uses the engine's actual scoped fence wait rather than a fixed delay or polling loop. */
    bool PrivateWorldNative::WaitFence()
    {
        if (state_->Active)
            MiniMapNative::Get()->captureAdmission.waitRenderFence(state_->Storage, false);
        return PollFence();
    }

    /** Keeps native fence ownership separate from logical request state. */
    bool PrivateWorldNative::HasFence() const noexcept { return state_->Active; }
}
#endif
