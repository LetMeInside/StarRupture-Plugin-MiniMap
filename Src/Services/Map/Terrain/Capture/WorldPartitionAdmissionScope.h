// Restricts one synchronous InitWorld admission scope to a retained private GamePreview world.
// This guard installs no hook, creates no world and owns no streaming operation.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/CaptureAdmissionApi.h"
#include <cstdint>
#include <thread>

namespace SDK { class UClass; }
namespace MiniMapNative { struct NativeApi; }

namespace Services::Map::Terrain::Capture
{
    /** Adds temporary retention during controlled native initialization only.
     * The future session owner must already retain the world/package and outlive this scope,
     * the supplied generation reference and every native operation started by InitWorld.
     * Construction/destruction must occur synchronously on the game thread.
     */
    class WorldPartitionAdmissionScope final
    {
    public:
        /** Validates an independently owned graph and opens the thread-local exception.
         * package is the expected private instance; sourceWorld must be a distinct live world.
         * generation is borrowed only until destruction; zero or an existing scope rejects admission.
         */
        WorldPartitionAdmissionScope(SDK::UWorld* world, SDK::UPackage* package,
            const std::uint64_t& generation, const SDK::UWorld* sourceWorld);

        /** Closes admission before releasing only the scope's additional native retentions. */
        ~WorldPartitionAdmissionScope();

        /** Prevents moving or duplicating thread-local scope and native retention ownership. */
        WorldPartitionAdmissionScope(const WorldPartitionAdmissionScope&) = delete;
        /** Prevents replacing an active admission identity. */
        WorldPartitionAdmissionScope& operator=(const WorldPartitionAdmissionScope&) = delete;

        /** Reports whether this scope was admitted; callers must not initialize on rejection. */
        bool IsOpen() const noexcept;
        /** Reports an enclosing controlled initialization on this thread, for hook stop exclusion. */
        static bool HasOpenScope() noexcept;

        /** Implements the detour decision, forwarding every other invocation exactly once.
         * original must be the validated callable trampoline, never the intercepted entry.
         * Hook installation/publication and callback drain remain separate responsibilities.
         */
        static bool Dispatch(const SDK::UWorldSubsystem* subsystemDefault,
            SDK::UObject* outer, MiniMapNative::CaptureAdmissionApi::ShouldCreateSubsystem& original);

    private:
        /** Checks game-thread/native GC conditions without raw-memory validation. */
        bool CanAccessNativeObjects() const noexcept;
        /** Checks exact SDK ownership links, absent live game attachment and native package lookup. */
        bool HasPrivateWorldAssociation() const;
        /** Checks exact CDO/world identity, generation and actual GamePreview predicates. */
        bool CanAdmit(const SDK::UWorldSubsystem* subsystemDefault, SDK::UObject* outer) const;

        static thread_local WorldPartitionAdmissionScope* current_;
        const MiniMapNative::NativeApi* api_ = nullptr;
        SDK::UWorld* world_ = nullptr;
        SDK::UPackage* package_ = nullptr;
        /** Exact class and temporarily retained CDO for one required subsystem. */
        struct AdmissionTarget
        {
            SDK::UClass* Class = nullptr;
            SDK::UWorldSubsystem* DefaultObject = nullptr;
        };
        AdmissionTarget targets_[2];
        const std::uint64_t* currentGeneration_ = nullptr;
        std::uint64_t generation_ = 0;
        std::thread::id thread_;
    };
}
#endif
