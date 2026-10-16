#pragma once

#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
#include <cstdint>
#include "CaptureRetirementCore.h"
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
#include "FloatReadbackPixels.h"
#endif
namespace MiniMapReadbackNative
{
    bool ResolvePrerequisites();
    bool Preflight();
    const char* Status();
    void SetStatus(const char* message);

    enum class Phase : uint32_t
    {
        Disabled, PreflightFailed, ReadyToSubmit, CommandQueued, CopySubmitted,
        CopyPending, ReadbackReady, PixelsCopied, Verified, Failed, Cancelled,
        CleanupPending, CleanupComplete
    };
    enum class Outcome : uint32_t { Pending, Verified, Failed, Cancelled };
    struct Job;
    struct Report
    {
        Phase Current = Phase::Disabled;
        Outcome Result = Outcome::Pending;
        uint32_t Visited = 0, Callables = 0;
        bool Outstanding = false, Quarantined = false, HasPixels = false;
        MiniMapCaptureRetirement::EnqueueState Enqueue = MiniMapCaptureRetirement::EnqueueState::NotAttempted;
        bool NativeEnqueueEverInvoked = false;
        const char* Failure = nullptr;
        int Pitch = 0, BufferHeight = 0;
        uint64_t CompletionFrames = 0;
        double CompletionMs = 0;
    };
    // One job; native objects remain private to this TU's render commands.
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    // Resource is GC-protected by the game-thread capture owner through retirement.
    Job* CreateExternalJob(void* resource, uint32_t width, uint32_t height);
    // Owner may call ONLY before builder Execute; no capture may reference it yet.
    bool DiscardExternalBeforeCapture(Job*&);
    bool MarkExternalCaptureCommitted(Job*); // game thread, immediately after Execute
    bool ExternalRejectedWithoutReadback(const Job*); // zero callbacks, no native users
    bool CompleteExternalCaptureRetirement(Job*); // ONLY after ordered RHI fence
    bool CaptureRetirementRendererAvailable();
    const MiniMapFloatPixels::Stats* FloatStats(const Job*);
    // Immutable packed CPU pixels; valid only until DestroyIfRetired. No GPU access.
    const std::vector<uint8_t>* FloatPixels(const Job*);
#endif
    void Service(Job*, uint64_t frame); // game thread; submits at most one command
    void Cancel(Job*);                 // invalidates publication, not resources
    Report Inspect(const Job*);
    bool DestroyIfRetired(Job*&);      // only after callback destruction + cleanup
}
#endif
