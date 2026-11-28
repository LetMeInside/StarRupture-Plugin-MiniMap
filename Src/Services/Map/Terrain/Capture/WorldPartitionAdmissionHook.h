// Owns one admission detour through the existing hook service under the engine's game-thread contract.
// Installation is explicit; the detour grants no exception without WorldPartitionAdmissionScope.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/CaptureAdmissionApi.h"
#include "Services/NativeHooks.h"
#include <atomic>

namespace Services::Map::Terrain::Capture
{
    /** Owns at most one admission detour at a time and preserves its broker continuation.
     * Install and Stop run on the game thread outside subsystem initialization/detour execution.
     * The selected hook backend is the inspected AlienX broker; this is not a generic concurrent-unload contract.
     */
    class WorldPartitionAdmissionHook final
    {
    public:
        /** Creates an inert hook owner without accessing the engine or loader. */
        WorldPartitionAdmissionHook() = default;
        /** Requires successful explicit stop before hook-service/native binding destruction. */
        ~WorldPartitionAdmissionHook();
        /** Keeps the backend handle and publication identity stationary. */
        WorldPartitionAdmissionHook(const WorldPartitionAdmissionHook&) = delete;
        /** Prevents replacing an owner while its detour can execute. */
        WorldPartitionAdmissionHook& operator=(const WorldPartitionAdmissionHook&) = delete;

        /** Installs only without active or uncertain ownership and validates the returned original.
         * No engine admission call may reenter this game-thread installation operation.
         */
        bool Install(MiniMap::Services::INativeHooks& hooks);
        /** Removes on the game thread after all controlled scopes and detour entries have returned.
         * Success covers this engine admission entry only, not other callbacks/resources or DLL unload as a whole.
         */
        bool Stop();
        /** Reports installation/ownership debt that must keep this owner and plugin code alive. */
        bool HasRetirementDebt() const noexcept;

    private:
        /** Requires the verified engine game-thread calling/initialization contract at lifecycle boundaries. */
        static bool CanChangeHook() noexcept;
        /** Receives the broker continuation before Install returns to the game-thread caller. */
        static void PublishOriginal(void* owner, void* original) noexcept;
        /** Forwards through the genuine continuation except for the existing scoped exact-world decision. */
        static bool Dispatch(const SDK::UWorldSubsystem* subsystemDefault, SDK::UObject* outer);

        static std::atomic<MiniMapNative::CaptureAdmissionApi::ShouldCreateSubsystem*> original_;
        static bool installationActiveOrUncertain_;
        static thread_local unsigned entryDepth_;
        MiniMap::Services::INativeHooks* hooks_ = nullptr;
        MiniMap::Services::Hook hook_;
        bool ownershipUncertain_ = false;
        bool originalValidated_ = false;
    };
}
#endif
