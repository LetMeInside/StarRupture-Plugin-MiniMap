#pragma once

#if defined(MINIMAP_ASYNC_READBACK_EXPERIMENT) && MINIMAP_ASYNC_READBACK_EXPERIMENT && defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include "ReadbackPixels.h"
namespace MiniMapReadbackNative
{
    bool ResolvePrerequisites();
    bool Preflight();
    const char* Status();

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
        const char* Failure = nullptr;
        int Pitch = 0, BufferHeight = 0;
        uint64_t CompletionFrames = 0;
        double CompletionMs = 0;
        size_t Mismatches = 0;
        int FirstX = -1, FirstY = -1;
        MiniMapAsyncReadbackExperiment::Pixels::Rgba Expected{}, Actual{};
    };
    // One job; native objects remain private to this TU's render commands.
    Job* CreateJob();
    void Service(Job*, uint64_t frame); // game thread; submits at most one command
    void Cancel(Job*);                 // invalidates publication, not resources
    Report Inspect(const Job*);
    bool DestroyIfRetired(Job*&);      // only after callback destruction + cleanup
}
#endif
