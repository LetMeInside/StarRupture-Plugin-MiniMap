// Implements the narrow native admission hook lifecycle with the existing synchronous AlienX broker.
// Its game-thread serialization follows subsystem collection creation, not a general hook-drain abstraction.
#if defined(MODLOADER_CLIENT_BUILD)
#include "WorldPartitionAdmissionHook.h"
#include "WorldPartitionAdmissionScope.h"
#include "Native/NativeApi.h"
#include <cstdlib>
#include <exception>

namespace Services::Map::Terrain::Capture
{
    std::atomic<MiniMapNative::CaptureAdmissionApi::ShouldCreateSubsystem*>
        WorldPartitionAdmissionHook::original_ = nullptr;
    bool WorldPartitionAdmissionHook::installationActiveOrUncertain_ = false;
    thread_local unsigned WorldPartitionAdmissionHook::entryDepth_ = 0;

    /** Excludes detour/scope reentrancy and requires the native game-thread boundary. */
    bool WorldPartitionAdmissionHook::CanChangeHook() noexcept
    {
        const auto* api = MiniMapNative::Get();
        return api && api->player.isGameThread && api->player.isGameThread() &&
            !entryDepth_ && !WorldPartitionAdmissionScope::HasOpenScope();
    }

    /** Makes the broker continuation callable before the serialized caller may create any world. */
    void WorldPartitionAdmissionHook::PublishOriginal(void* owner, void* original) noexcept
    {
        auto* self = static_cast<WorldPartitionAdmissionHook*>(owner);
        const auto* api = MiniMapNative::Get();
        if (!self || !api || !original || original == reinterpret_cast<void*>(&Dispatch) ||
            original == reinterpret_cast<void*>(api->captureAdmission.shouldCreateSubsystem))
            return;
        original_.store(reinterpret_cast<MiniMapNative::CaptureAdmissionApi::ShouldCreateSubsystem*>(original),
            std::memory_order_release);
        self->originalValidated_ = true;
    }

    /** Installs only without current/uncertain ownership; uncertainty remains retirement debt. */
    bool WorldPartitionAdmissionHook::Install(MiniMap::Services::INativeHooks& hooks)
    {
        const auto* api = MiniMapNative::Get();
        if (!CanChangeHook() || installationActiveOrUncertain_ || !api->captureAdmission.IsComplete())
            return false;
        installationActiveOrUncertain_ = true;
        originalValidated_ = false;
        hooks_ = &hooks;
        const MiniMap::Services::NativeDetour detour{
            reinterpret_cast<std::uintptr_t>(api->captureAdmission.shouldCreateSubsystem),
            reinterpret_cast<void*>(&Dispatch), &PublishOriginal, this};
        ownershipUncertain_ = true;
        MiniMap::Services::HookInstallation installation{};
        try
        {
            installation = hooks.Install(detour);
        }
        catch (...)
        {
            return false;
        }
        hook_ = installation.handle;
        const bool rejectedWithoutOwnership =
            installation.state == MiniMap::Services::HookInstallState::Rejected &&
            !hook_.id && !originalValidated_;
        ownershipUncertain_ = installation.state == MiniMap::Services::HookInstallState::BackendUnknown ||
            (installation.state == MiniMap::Services::HookInstallState::Active && !hook_.id) ||
            (installation.state == MiniMap::Services::HookInstallState::Rejected && !rejectedWithoutOwnership);
        if (rejectedWithoutOwnership)
            installationActiveOrUncertain_ = false;
        if (installation.state != MiniMap::Services::HookInstallState::Active ||
            installation.status.error != MiniMap::Services::Error::None || !hook_.id)
            return false;
        if (!originalValidated_)
        {
            Stop();
            return false;
        }
        return true;
    }

    /** Stops only at a nonreentrant game-thread boundary; the broker's continuation remains allocated. */
    bool WorldPartitionAdmissionHook::Stop()
    {
        if (!HasRetirementDebt())
            return true;
        if (!CanChangeHook() || !hooks_ || ownershipUncertain_)
            return false;
        const auto removal = hooks_->Remove(hook_);
        if (removal.status.error != MiniMap::Services::Error::None ||
            removal.registry != MiniMap::Services::RegistryRemoval::RequestReturned)
        {
            ownershipUncertain_ = true;
            return false;
        }
        hook_ = {};
        hooks_ = nullptr;
        installationActiveOrUncertain_ = false;
        return true;
    }

    /** Reports backend debt independently of whether exceptional admission is currently open. */
    bool WorldPartitionAdmissionHook::HasRetirementDebt() const noexcept
    {
        return hook_.id || ownershipUncertain_;
    }

    /** Tracks reentrancy on the engine admission thread and preserves original exceptions/return values. */
    bool WorldPartitionAdmissionHook::Dispatch(const SDK::UWorldSubsystem* subsystemDefault,
        SDK::UObject* outer)
    {
        const auto original = original_.load(std::memory_order_acquire);
        if (!original)
            std::abort();
        ++entryDepth_;
        try
        {
            const bool result = WorldPartitionAdmissionScope::Dispatch(subsystemDefault, outer, *original);
            --entryDepth_;
            return result;
        }
        catch (...)
        {
            --entryDepth_;
            throw;
        }
    }

    /** Does not silently discard an installed hook or claim that uncertain backend removal succeeded. */
    WorldPartitionAdmissionHook::~WorldPartitionAdmissionHook()
    {
        if (HasRetirementDebt())
            std::terminate();
    }
}
#endif
