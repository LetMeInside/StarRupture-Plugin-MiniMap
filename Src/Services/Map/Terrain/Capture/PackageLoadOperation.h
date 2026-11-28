// Owns a genuine engine package-load completion and its request-specific destruction acknowledgement.
// This low-level bridge creates no world context, partition, streaming source or gameplay state.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <memory>
#include <vector>

namespace SDK { class UPackage; class UObject; class UWorld; }
namespace MiniMapNative { struct PackageLoadApi; }

namespace Services::Map::Terrain::Capture
{
    /** Owns one asynchronous root request, GamePreview preload association and callback completion graph.
     * Callers supply an absent unique destination and keep native bindings/plugin code available
     * through native retirement, ownership transfer and destruction. No request is submitted automatically.
     */
    class PackageLoadOperation final
    {
    public:
        /** Describes the logical result separately from native callback retirement. */
        enum class Result { Pending, Succeeded, Failed, Cancelled };

        /** Creates an inert operation without engine allocation or loading. */
        PackageLoadOperation() = default;
        /** Requires native retirement, then removes preload identity and releases any untransferred completion owners. */
        ~PackageLoadOperation();
        /** Keeps native callback state and request identity with one stable owner. */
        PackageLoadOperation(const PackageLoadOperation&) = delete;
        /** Prevents replacing an operation with outstanding native ownership. */
        PackageLoadOperation& operator=(const PackageLoadOperation&) = delete;

        /** Loads source into a distinct externally chosen instance name using the native custom-name overload.
         * Each name is supplied as its comparison index and number, not native object storage.
         * Names must be valid package identities; this operation establishes/owns destination preload identity.
         * False may still leave retirement debt; consult IsNativeRetired before destroying the operation.
         */
        bool Submit(std::int32_t sourceComparisonIndex, std::uint32_t sourceNumber,
            std::int32_t instanceComparisonIndex, std::uint32_t instanceNumber);
        /** Invalidates logical delivery only; it does not cancel native async loading. */
        void Cancel();
        /** Waits for this positive native request ID only, outside reentrant engine loading callbacks.
         * Refuses suspended loading; confirms completion and destruction of all delegate captures after return.
         * May service required engine dependencies and block; never passes a global/empty request selection.
         */
        bool WaitForNativeRetirement();
        /** Observes callback destruction without flushing ordinary asynchronous acquisition. */
        bool PollNativeRetirement() noexcept;
        /** Reports explicit native-return/destruction acknowledgement, independently of logical result. */
        bool IsNativeRetired() const noexcept;
        /** Borrows the callback-owned package after retirement, including cancellation; null after transfer. */
        SDK::UPackage* GetRetainedPackage() const noexcept;
        /** Borrows the world discovered/retained during completion; null after transfer or for a non-world package. */
        SDK::UWorld* GetRetainedWorld() const noexcept;
        /** Transfers callback-acquired package/world references to the session after native retirement.
         * Capacity allocation precedes transfer; failure leaves callback ownership intact. No SDK layout is accessed.
         */
        SDK::UPackage* TransferRetainedPackage(std::vector<SDK::UObject*>& nativeOwners);
        /** Reports the logical outcome without implying initialization, readiness or native retirement. */
        Result GetResult() const noexcept;

        /** Supplies bound genuine-header ABI entry points; missing bindings are a lifecycle violation. */
        static const MiniMapNative::PackageLoadApi& RequireNativeBindings();

    private:
        /** State is also retained by every genuine delegate copy until its native destruction. */
        struct State;
        /** Rejects non-game-thread or reentrant ownership transitions before calling the loader. */
        bool CanEnterNative() const noexcept;

        std::shared_ptr<State> state_;
        std::int32_t requestId_ = 0;
        bool nativeRetired_ = true;
        bool insideNativeCall_ = false;
    };
}
#endif
