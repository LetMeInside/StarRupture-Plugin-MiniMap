#if defined(MINIMAP_ASYNC_READBACK_EXPERIMENT) && MINIMAP_ASYNC_READBACK_EXPERIMENT && defined(MODLOADER_CLIENT_BUILD)
#include "AsyncReadbackExperiment.h"
#include "ReadbackNativeAdapter.h"
#include "../plugin_helpers.h"
#include <atomic>

namespace MiniMapAsyncReadbackExperiment
{
    namespace Native = MiniMapReadbackNative;
    namespace
    {
        // No global owning UE objects or dynamic initializers. The CPU job is
        // retained after success for diagnostics, then deleted at safe shutdown.
        Native::Job* job = nullptr;
        IPluginEngineEvents* engine = nullptr;
        std::atomic<bool> attempted{false}, stopping{false}, started{false};
        std::atomic_flag scheduling = ATOMIC_FLAG_INIT;
        struct SchedulingScope
        {
            ~SchedulingScope() { scheduling.clear(std::memory_order_release); }
        };
        bool registered = false, finished = false;
        uint32_t reported = 0;
        uint64_t frame = 0;

        constexpr uint32_t Bit(Native::Phase p) { return 1u << uint32_t(p); }

        void OnTick(float)
        {
            if (!started.load(std::memory_order_acquire) || stopping.load() ||
                scheduling.test_and_set(std::memory_order_acquire)) return;
            SchedulingScope scope;
            if (!stopping.load() && !finished && job)
            {
                Native::Service(job, ++frame);
                if (stopping.load())
                {
                    Native::Cancel(job);
                    return;
                }
                const auto report = Native::Inspect(job);
                auto once = [&](Native::Phase phase)
                {
                    const auto bit = Bit(phase);
                    if (!(report.Visited & bit) || (reported & bit)) return false;
                    reported |= bit;
                    return true;
                };
                if (once(Native::Phase::CommandQueued))
                    LOG_INFO("MiniMap: R1 submission started: 37x23 PF_B8G8R8A8 linear, 3404 bytes, splits=13/7");
                if (once(Native::Phase::CopySubmitted))
                    LOG_INFO("MiniMap: R1 GPU copy submitted; polling on later engine frames, one command outstanding maximum");
                if (once(Native::Phase::Cancelled))
                    LOG_WARN("MiniMap: R1 result publication cancelled; queued/GPU resources remain owned until retirement");
                if (once(Native::Phase::Failed))
                    LOG_ERROR("MiniMap: R1 failed: %s; pitchPixels=%d height=%d latency=%.3fms; cleanup=%s; no retry",
                        report.Failure ? report.Failure : "unknown",
                        report.Pitch, report.BufferHeight, report.CompletionMs,
                        report.Quarantined ? "quarantined (retirement unproven)" : "pending/completed");
                if (report.Result == Native::Outcome::Verified && report.HasPixels && once(Native::Phase::Verified))
                    LOG_INFO("MiniMap: R1 VERIFIED: 37x23 BGRA8, pitchPixels=%d bufferHeight=%d latency=%.3fms/%llu frames; all 851 pixels match including RGBA alpha/channel order; mismatches=%zu",
                        report.Pitch, report.BufferHeight, report.CompletionMs,
                        static_cast<unsigned long long>(report.CompletionFrames), report.Mismatches);
                else if (report.HasPixels && report.Mismatches && !(reported & Bit(Native::Phase::PixelsCopied)))
                {
                    reported |= Bit(Native::Phase::PixelsCopied);
                    LOG_ERROR("MiniMap: R1 pixels: pitchPixels=%d height=%d latency=%.3fms/%llu frames mismatches=%zu first=(%d,%d) expectedRGBA=(%u,%u,%u,%u) actualRGBA=(%u,%u,%u,%u)",
                        report.Pitch, report.BufferHeight, report.CompletionMs,
                        static_cast<unsigned long long>(report.CompletionFrames), report.Mismatches,
                        report.FirstX, report.FirstY, unsigned(report.Expected.R), unsigned(report.Expected.G),
                        unsigned(report.Expected.B), unsigned(report.Expected.A), unsigned(report.Actual.R),
                        unsigned(report.Actual.G), unsigned(report.Actual.B), unsigned(report.Actual.A));
                }
                if ((report.Visited & Bit(Native::Phase::CleanupComplete)) &&
                    !report.Callables && !report.Outstanding && once(Native::Phase::CleanupComplete))
                {
                    LOG_INFO("MiniMap: R1 native cleanup complete; outcome=%s; no outstanding callable; one-shot finished",
                        report.Result == Native::Outcome::Verified ? "Verified" :
                        report.Result == Native::Outcome::Cancelled ? "Cancelled" : "Failed");
                    finished = true;
                }
                if (report.Quarantined) finished = true;
            }
        }

        void BeginEngineShutdown()
        {
            // Nonblocking: never queue a new command or wait on this callback.
            if (stopping.exchange(true)) return;
            if (!scheduling.test_and_set(std::memory_order_acquire))
            {
                SchedulingScope scope;
                if (started.load(std::memory_order_acquire)) Native::Cancel(job);
            }
            // If Service was already accepted, its tick observes stopping on
            // return, cancels, and suppresses publication. Never wait here.
            LOG_INFO("MiniMap: R1 shutdown begins: new submissions stopped; result publication cancelled");
        }
    }

    void Initialize()
    {
        if (attempted.exchange(true) || stopping.load()) return;
        LOG_INFO("MiniMap: R1 enabled (one job/process); native ABI assertions passed");
        if (!Native::Preflight())
        {
            LOG_WARN("MiniMap: R1 PreflightFailed: %s; no GPU work submitted", Native::Status());
            return;
        }
        auto* hooks = GetHooks();
        if (!hooks || !hooks->Engine || !hooks->Engine->RegisterOnTick ||
            !hooks->Engine->UnregisterOnTick || !hooks->Engine->RegisterOnShutdown ||
            !hooks->Engine->UnregisterOnShutdown)
        {
            LOG_ERROR("MiniMap: R1 PreflightFailed: engine lifecycle callbacks unavailable");
            return;
        }
        job = Native::CreateJob();
        if (!job) { LOG_ERROR("MiniMap: R1 Failed: CPU job allocation failed; no GPU work"); return; }
        engine = hooks->Engine;
        started.store(true, std::memory_order_release);
        engine->RegisterOnShutdown(&BeginEngineShutdown);
        engine->RegisterOnTick(&OnTick);
        registered = true;
        LOG_INFO("MiniMap: R1 preflight passed; ReadyToSubmit on next post-engine tick");
    }

    void CancelForWorldTransition()
    {
        if (scheduling.test_and_set(std::memory_order_acquire)) return;
        SchedulingScope scope;
        if (started.load(std::memory_order_acquire) && !stopping.load())
        {
            if (job)
            {
                const auto report = Native::Inspect(job);
                // Startup-world teardown must not consume the original CPU-only
                // request. Once any command is queued, normal cancellation applies.
                if (report.Current == Native::Phase::ReadyToSubmit &&
                    report.Result == Native::Outcome::Pending &&
                    !(report.Visited & Bit(Native::Phase::CommandQueued)) &&
                    !report.Callables && !report.Outstanding)
                    return;
            }
            Native::Cancel(job);
        }
    }

    void Shutdown()
    {
        BeginEngineShutdown();
        if (registered)
        {
            engine->UnregisterOnTick(&OnTick);
            engine->UnregisterOnShutdown(&BeginEngineShutdown);
            registered = false;
        }
        if (scheduling.test_and_set(std::memory_order_acquire))
        {
            // A game-thread callback is still executing. No wait and no freeing
            // storage/code beneath it. Hot unload in this condition is unsupported.
            LOG_ERROR("MiniMap: R1 shutdown overlaps scheduling; job retained. DLL unload is NOT certified safe");
            return;
        }
        SchedulingScope scope;
        Native::Cancel(job);
        if (!Native::DestroyIfRetired(job) && job)
        {
            const auto report = Native::Inspect(job);
            LOG_ERROR("MiniMap: R1 shutdown with pending/quarantined work: phase=%u callables=%u outstanding=%u; resources retained, no wait. Pending DLL callbacks are NOT safe to unload; hot reload unsupported",
                unsigned(report.Current), report.Callables, unsigned(report.Outstanding));
        }
    }
}
#endif
