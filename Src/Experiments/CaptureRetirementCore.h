#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

// CPU-only policy. Completion means ordered RHI submission, never GPU idle.
namespace MiniMapCaptureRetirement
{
    enum class EnqueueState : uint8_t
    {
        NotAttempted, RejectedBeforeEnqueue, NativeInvoked, CallbackExecuting,
        CallbackDestroyed, ReadbackCompleted, Uncertain
    };
    template<class Invoke> bool SubmitWithEvidence(bool prerequisites,
        std::atomic<EnqueueState>* state, std::atomic<bool>* everInvoked, Invoke&& invoke)
    {
        if (!prerequisites)
        {
            if (state) state->store(EnqueueState::RejectedBeforeEnqueue,std::memory_order_release);
            return false;
        }
        // Publish BEFORE the void native call; a callback may execute before it returns.
        if (everInvoked) everInvoked->store(true,std::memory_order_release);
        if (state) state->store(EnqueueState::NativeInvoked,std::memory_order_release);
        invoke(); return true;
    }
    constexpr EnqueueState ObserveEnqueue(EnqueueState state, bool everInvoked,
        uint32_t callables, bool outstanding, bool cleanupComplete)
    {
        if (everInvoked && !callables && !outstanding && state!=EnqueueState::Uncertain)
            return cleanupComplete ? EnqueueState::ReadbackCompleted : EnqueueState::CallbackDestroyed;
        return state;
    }
    struct Evidence
    {
        bool External = false, CaptureCommitted = false, NativeEverInvoked = false;
        EnqueueState Enqueue = EnqueueState::NotAttempted;
        bool ReadbackAllocated = false, TextureOwned = false, CopyIssued = false;
        bool MappingPossible = false, PendingFenceWritePossible = false;
        uint32_t Callables = 0;
        bool Outstanding = false;
    };
    constexpr bool EmptyRejectedCapture(const Evidence& e)
    {
        return e.External && e.CaptureCommitted && !e.NativeEverInvoked &&
            e.Enqueue == EnqueueState::RejectedBeforeEnqueue && !e.ReadbackAllocated &&
            !e.TextureOwned && !e.CopyIssued && !e.MappingPossible &&
            !e.PendingFenceWritePossible && !e.Callables && !e.Outstanding;
    }
    enum class FenceState : uint8_t { Empty, Constructed, Inserted, Complete, Destroyed, Uncertain };
    constexpr bool CanStart(bool emptyRejected, bool renderer, bool bindings, bool registered, bool currentWorld)
    { return emptyRejected && renderer && bindings && (!registered || currentWorld); }
    constexpr bool CanPoll(bool emptyRejected, bool renderer, bool bindings, bool detached)
    { return emptyRejected && renderer && bindings && detached; }
    class FenceOwner
    {
        alignas(8) unsigned char Storage[8]{};
        FenceState State = FenceState::Empty;
    public:
        constexpr FenceOwner() = default;
        FenceOwner(const FenceOwner&) = delete;
        FenceOwner& operator=(const FenceOwner&) = delete;
        FenceState GetState() const { return State; }
        template<class Ops> bool Prepare(Ops& ops)
        {
            if (State != FenceState::Empty || !ops.Available()) return false;
            // Set uncertainty before crossing a native ownership boundary.
            State = FenceState::Uncertain;
            if (!ops.Construct(Storage)) return false;
            State = FenceState::Constructed; return true;
        }
        template<class Ops> bool Insert(Ops& ops, bool detached)
        {
            if (State != FenceState::Constructed || !detached || !ops.Available()) return false;
            State = FenceState::Uncertain;
            if (!ops.Begin(Storage)) return false;
            State = FenceState::Inserted; return true;
        }
        template<class Ops> bool Poll(Ops& ops)
        {
            if (State == FenceState::Complete) return true;
            if (State != FenceState::Inserted || !ops.Available()) return false;
            if (!ops.Poll(Storage)) return false;
            State = FenceState::Complete; return true;
        }
        template<class Ops> bool Release(Ops& ops)
        {
            if (State == FenceState::Empty || State == FenceState::Destroyed) return true;
            if ((State != FenceState::Constructed && State != FenceState::Complete) || !ops.Available()) return false;
            State = FenceState::Uncertain;
            ops.Destroy(Storage); State = FenceState::Destroyed; return true;
        }
        // No automatic native destruction: pending/uncertain state must retain
        // storage conservatively. The existing game-thread owner calls Release.
    };
    static_assert(alignof(FenceOwner) == 8);
}
