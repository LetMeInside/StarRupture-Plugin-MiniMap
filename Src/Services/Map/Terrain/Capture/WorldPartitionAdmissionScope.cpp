// Implements only the accepted exact-world admission decision using typed SDK/native interfaces.
// It is dormant until an independently safe streaming owner and installed detour invoke it.
#if defined(MODLOADER_CLIENT_BUILD)
#include "WorldPartitionAdmissionScope.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <exception>

namespace Services::Map::Terrain::Capture
{
    thread_local WorldPartitionAdmissionScope* WorldPartitionAdmissionScope::current_ = nullptr;

    /** Opens admission only after validating and additionally retaining the exact private identity. */
    WorldPartitionAdmissionScope::WorldPartitionAdmissionScope(SDK::UWorld* world,
        SDK::UPackage* package, const std::uint64_t& generation, const SDK::UWorld* sourceWorld)
        : api_(MiniMapNative::Get()), world_(world), package_(package),
          currentGeneration_(&generation), generation_(generation), thread_(std::this_thread::get_id())
    {
        if (current_ || !generation_ || !CanAccessNativeObjects() || !world_ || !package_ ||
            !sourceWorld || sourceWorld == world_ || sourceWorld->Outer == package_ ||
            !api_->captureAdmission.IsComplete() || !HasPrivateWorldAssociation())
            return;
        targets_[0].Class = SDK::UWorldPartitionSubsystem::StaticClass();
        targets_[1].Class = SDK::UWorldPartitionHLODRuntimeSubsystem::StaticClass();
        for (auto& target : targets_)
        {
            if (!target.Class || !target.Class->ClassDefaultObject)
                return;
            target.DefaultObject = static_cast<SDK::UWorldSubsystem*>(target.Class->ClassDefaultObject);
            if (target.DefaultObject->Class != target.Class ||
                !(target.DefaultObject->Flags & SDK::EObjectFlags::ClassDefaultObject))
                return;
        }
        api_->asset.retainObject(world_);
        api_->asset.retainObject(package_);
        for (const auto& target : targets_)
            api_->asset.retainObject(target.DefaultObject);
        current_ = this;
    }

    /** Closes admission before releasing the scope's extra world, package and CDO references. */
    WorldPartitionAdmissionScope::~WorldPartitionAdmissionScope()
    {
        if (thread_ != std::this_thread::get_id())
            std::terminate();
        if (!IsOpen())
            return;
        if (!CanAccessNativeObjects())
            std::terminate();
        current_ = nullptr;
        for (const auto& target : targets_)
            api_->asset.releaseObject(target.DefaultObject);
        api_->asset.releaseObject(package_);
        api_->asset.releaseObject(world_);
    }

    /** Reports successful opening on the constructing thread without consulting native objects. */
    bool WorldPartitionAdmissionScope::IsOpen() const noexcept
    {
        return thread_ == std::this_thread::get_id() && current_ == this;
    }

    /** Prevents removing the admission entry during its owning synchronous InitWorld scope. */
    bool WorldPartitionAdmissionScope::HasOpenScope() noexcept
    {
        return current_ != nullptr;
    }

    /** Requires unchanged bindings, the game thread and no native GC/purge work. */
    bool WorldPartitionAdmissionScope::CanAccessNativeObjects() const noexcept
    {
        return thread_ == std::this_thread::get_id() && api_ && api_ == MiniMapNative::Get() &&
            api_->player.isGameThread && api_->player.isGameThread() &&
            api_->asset.retainObject && api_->asset.releaseObject &&
            api_->world.isGarbageCollectingAndLocking &&
            !api_->world.isGarbageCollectingAndLocking() &&
            api_->world.isIncrementalPurgePending && !api_->world.isIncrementalPurgePending() &&
            api_->world.isIncrementalReachabilityPending && !api_->world.isIncrementalReachabilityPending() &&
            api_->asset.exitPurge && !*api_->asset.exitPurge;
    }

    /** Validates the root graph without transplanting partition objects or attaching gameplay state. */
    bool WorldPartitionAdmissionScope::HasPrivateWorldAssociation() const
    {
        if (!api_->captureAdmission.isGameWorld(world_) ||
            !api_->captureAdmission.isPreviewWorld(world_) || world_->Outer != package_ ||
            api_->captureAdmission.findWorldInPackage(package_) != world_ ||
            world_->NetDriver || world_->DemoNetDriver || world_->OwningGameInstance)
            return false;
        const auto* level = world_->PersistentLevel;
        if (!level || level->Outer != world_ || level->OwningWorld != world_)
            return false;
        const auto* settings = level->WorldSettings;
        return settings && settings->Outer == level && settings->WorldPartition &&
            settings->WorldPartition->Outer == settings;
    }

    /** Admits only an exact target CDO and world while the retained session generation remains current. */
    bool WorldPartitionAdmissionScope::CanAdmit(const SDK::UWorldSubsystem* subsystemDefault,
        SDK::UObject* outer) const
    {
        if (!CanAccessNativeObjects() || *currentGeneration_ != generation_ || outer != world_)
            return false;
        for (const auto& target : targets_)
        {
            if (subsystemDefault == target.DefaultObject && subsystemDefault->Class == target.Class &&
                target.Class->ClassDefaultObject == subsystemDefault &&
                (subsystemDefault->Flags & SDK::EObjectFlags::ClassDefaultObject))
                return HasPrivateWorldAssociation();
        }
        return false;
    }

    /** Preserves unrelated and nested-world behavior through the supplied genuine original. */
    bool WorldPartitionAdmissionScope::Dispatch(const SDK::UWorldSubsystem* subsystemDefault,
        SDK::UObject* outer, MiniMapNative::CaptureAdmissionApi::ShouldCreateSubsystem& original)
    {
        if (current_ && current_->CanAdmit(subsystemDefault, outer))
            return true;
        return original(subsystemDefault, outer);
    }
}
#endif
