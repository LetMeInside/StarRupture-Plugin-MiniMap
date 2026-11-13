#pragma once
#include <cstdint>
#include <cstddef>

// CPU-only policy. Completion means ordered RHI submission, never GPU idle.
namespace MiniMapCaptureRetirement
{
    enum class FenceState : uint8_t { Empty, Constructed, Inserted, Complete, Destroyed, Uncertain };
    class FenceOwner
    {
        alignas(8) unsigned char Storage[8]{};
        FenceState State = FenceState::Empty;
    public:
        constexpr FenceOwner() = default;
        FenceOwner(const FenceOwner&) = delete;
        FenceOwner& operator=(const FenceOwner&) = delete;
        FenceState GetState() const { return State; }
        // Reuse storage only AFTER the verified destructor. No task handle copy,
        // no reset of an inserted/uncertain object and no new native boundary.
        bool Rearm()
        {if(State!=FenceState::Destroyed)return State==FenceState::Empty;State=FenceState::Empty;return true;}
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
