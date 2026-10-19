#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingScheduler.h"
#include "BuildingAtlas.h"
#include "BuildingCache.h"
#include "Map/Map.h"
#include "plugin_helpers.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace MiniMapBuildingSchedulerInternal { std::mutex LifecycleMutex; }

namespace
{
    template<class... Args> void SpriteDiagnostic(const char* format, Args... args) noexcept
    {
        try { if (auto* self = GetSelf(); self && self->logger && self->logger->Warn) self->logger->Warn(self, format, args...); }
        catch (...) {} // Diagnostic failures must not change dictionary/FIFO transitions.
    }
    namespace S = MiniMapSpriteStatus;
    namespace A = MiniMapBuildingAtlas;
    namespace C = MiniMapBuildingCache;
    constexpr std::array<std::string_view, 0> BlacklistedBlueprints{};
    // One closed handoff may coalesce several completed passes. Backpressure
    // leaves excess identities undiscovered until a later pass, never orphaned
    // as PendingCache without a work item. These are NOT native capture queues.
    constexpr size_t MaximumBatchNames = 256;
    struct Names
    {
        std::array<std::string, MaximumBatchNames> Paths;
        std::array<std::shared_ptr<const C::Sprite>, MaximumBatchNames> Payloads;
        size_t Count = 0;
    };
    Names g_pass, g_completed;
    struct RetentionRetry
    {
        std::string Path;
        C::Sprite Sprite;
        uint64_t Serial, NextTick;
        unsigned Attempts;
        std::shared_ptr<const C::Sprite> Payload;
    };
    // Bounded decoded work payloads, NOT another per-class status dictionary.
    // Moving into an empty slot needs no allocation, including under memory
    // pressure that caused Add to fail. Session transitions clear these slots.
    using RetentionRetries = std::array<std::optional<RetentionRetry>, MaximumBatchNames>;
    RetentionRetries g_retentionRetries;
    S::Dictionary* g_statuses = nullptr;
    MiniMapSpriteQueues::SpriteFifo* g_fifo = nullptr;
    MiniMapSpriteQueues::SpriteFeedbackQueue g_feedback;
    bool g_sessionActive = false, g_passOpen = false, g_serviceBusy = false, g_backpressureReported = false;
    bool g_discoveryAllocationReported = false, g_invalidFeedbackReported = false, g_invalidHandoffReported = false;
    uint64_t g_serial = 0, g_serviceTick = 0;
    std::thread::id g_drawThread;

    void Clear(Names& names) noexcept
    {
        for (size_t i = 0; i < names.Count; ++i) { names.Paths[i].clear(); names.Payloads[i].reset(); }
        names.Count = 0;
    }
    bool Room() noexcept { return g_pass.Count + g_completed.Count < MaximumBatchNames; }
    bool Admitted(uint64_t serial) noexcept
    { return g_sessionActive && g_serial == serial && g_statuses && g_fifo && MiniMapMap::GameplayReady(); }
    struct ServiceGuard
    {
        ~ServiceGuard()
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            g_serviceBusy = false;
        }
    };
    void RetainDecoded(const A::Batch& batch, std::string& path, C::Sprite& sprite,
        uint64_t serial, uint64_t tick, unsigned attempts, std::shared_ptr<const C::Sprite> payload = {})
    {
        const auto added = A::Add(batch, path, payload ? *payload : sprite); // No lifecycle lock.
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if (!Admitted(serial)) return;
        const auto it = g_statuses->find(path);
        if (it == g_statuses->end() || it->second.Where != S::Location::FileCache)
        {
            SpriteDiagnostic("MiniMap: sprite atlas feedback ignored: canonical=%s reason=expected FileCache status missing/changed; retention attempt=%u", path.c_str(), attempts);
            return;
        }
        if (added == A::AddResult::Invalid)
        {
            it->second.Where = S::Location::Failed;
            SpriteDiagnostic("MiniMap: cached sprite '%s' has unsupported atlas image/bounds; failed for session", path.c_str());
        }
        else if (added == A::AddResult::Unavailable)
        {
            // Three Add attempts total, separated by 60 admitted service ticks.
            // Keep the decoded bytes: retrying retention must not reread/decode.
            if (attempts >= 3)
            {
                SpriteDiagnostic("MiniMap: atlas retention retry limit reached for '%s'; FileCache/fallback preserved for session", path.c_str());
                return;
            }
            for (auto& slot : g_retentionRetries)
                if (!slot)
                {
                    constexpr auto maximum = (std::numeric_limits<uint64_t>::max)();
                    const auto next = tick > maximum - 60 ? maximum : tick + 60;
                    SpriteDiagnostic("MiniMap: atlas retention unavailable for '%s'; decoded retry scheduled (attempt %u/3)",
                        path.c_str(), attempts);
                    slot.emplace(RetentionRetry{std::move(path), std::move(sprite), serial, next, attempts, std::move(payload)});
                    return;
                }
            // Storage pressure is explicit and non-spinning. This valid cache
            // stays FileCache/fallback, never Failed or a capture request.
            SpriteDiagnostic("MiniMap: decoded retention retry storage full (256) for '%s'; FileCache/fallback preserved for session", path.c_str());
        }
        else if (added == A::AddResult::CapacityDeferred)
            SpriteDiagnostic("MiniMap: sprite atlas admission deferred: canonical=%s reason=source capacity; FileCache/fallback preserved, no recapture", path.c_str());
        // Retained: Core owns the crop. CapacityDeferred: legitimate fallback.
        // Neither outcome leaves a decoded retry or schedules another read.
    }
}

namespace MiniMapBuildingSchedulerInternal
{
    uint64_t ActiveSession() noexcept { return g_sessionActive ? g_serial : 0; }
    void StopSession() noexcept
    {
        g_sessionActive = false;
        g_passOpen = false;
        g_discoveryAllocationReported = g_invalidFeedbackReported = g_invalidHandoffReported = false;
        Clear(g_pass); Clear(g_completed);
        for (auto& retry : g_retentionRetries) retry.reset();
    }
}

namespace MiniMapBuildingScheduler
{
    void Initialize(S::Dictionary& statuses, MiniMapSpriteQueues::SpriteFifo& fifo)
    {
        MiniMapBuildingSchedulerInternal::StopSession();
        g_statuses = &statuses; g_fifo = &fifo;
    }
    MiniMapSpriteQueues::SpriteFeedbackQueue& FeedbackQueue() noexcept { return g_feedback; }
    void InitializeSession()
    {
        if (!g_statuses || !g_fifo || g_sessionActive) return;
        if (g_serial == (std::numeric_limits<uint64_t>::max)())
        { LOG_ERROR("MiniMap: building scheduler session serial exhausted; admission closed"); return; }
        std::string path;
        while (g_fifo->Remove(path)) {}
        MiniMapSpriteQueues::Feedback feedback;
        while (g_feedback.Remove(feedback)) {}
        g_statuses->clear();
        Clear(g_pass); Clear(g_completed); g_passOpen = false;
        for (const auto blueprint : BlacklistedBlueprints)
            g_statuses->try_emplace(std::string(blueprint), S::Location::Failed);
        ++g_serial; g_serviceTick = 0; g_backpressureReported = false;
        g_discoveryAllocationReported = g_invalidFeedbackReported = g_invalidHandoffReported = false;
        // Do not reset serviceBusy: a canceled unlocked service may still own
        // CPU work from the prior session. Its guard will release the reservation.
        g_sessionActive = true;
    }
    void StartDraw()
    {
        if (!g_sessionActive || !g_statuses || !g_fifo || g_passOpen) return;
        g_passOpen = true; g_drawThread = std::this_thread::get_id();
        MiniMapSpriteQueues::Feedback feedback;
        // Leave feedback in its original queue if this handoff is full. A success
        // can move its owned string and optional CPU payload into the pass slot.
        while (Room() && g_feedback.Remove(feedback))
        {
            // All feedback originates from an admitted request under the same
            // lifecycle lock. Reject old generations before looking up the path.
            if (feedback.Session != g_serial) continue;
            const auto it = g_statuses->find(feedback.BlueprintPath);
            if (it == g_statuses->end() || it->second.Where != S::Location::Queued)
            {
                if (!g_invalidFeedbackReported)
                {
                    g_invalidFeedbackReported = true;
                    SpriteDiagnostic("MiniMap: sprite capture feedback ignored: canonical=%s reason=expected Queued status missing/changed; further invalid feedback diagnostics suppressed for session", feedback.BlueprintPath.c_str());
                }
                continue;
            }
            if (feedback.Success)
            {
                g_pass.Payloads[g_pass.Count] = std::move(feedback.Payload);
                g_pass.Paths[g_pass.Count++] = std::move(feedback.BlueprintPath);
                it->second.Where = S::Location::PendingCache;
            }
            else
            {
                it->second.Where = S::Location::Failed;
                SpriteDiagnostic("MiniMap: sprite marked Failed: canonical=%s reason=unsuccessful capture/cache feedback; no rediscovery retry this session; see request rejection/persistence diagnostic", feedback.BlueprintPath.c_str());
            }
        }
    }
    void DrawBuilding(const MiniMapBuildingCollector::Record& building)
    {
        if (!g_sessionActive || !g_passOpen || g_drawThread != std::this_thread::get_id() ||
            !building.CanonicalBlueprint || building.CanonicalBlueprint->empty()) return;
        const auto& path = *building.CanonicalBlueprint;
        if (g_statuses->find(path) != g_statuses->end()) return;
        if (!Room())
        {
            if (!g_backpressureReported) SpriteDiagnostic("MiniMap: building cache handoff full (256); remaining discoveries wait for a later draw pass");
            g_backpressureReported = true;
            return;
        }
        try
        {
            // Both allocations precede the nonthrowing transfer into the pass.
            // Failed allocation leaves this class Unknown and eligible next pass.
            std::string copy(path);
            const auto [it, added] = g_statuses->try_emplace(path, S::Location::PendingCache);
            if (added)
            {
                g_pass.Payloads[g_pass.Count].reset(); // Ordinary discovery uses disk.
                g_pass.Paths[g_pass.Count++] = std::move(copy);
            }
        }
        catch (...)
        {
            if (!g_discoveryAllocationReported)
            {
                g_discoveryAllocationReported = true;
                SpriteDiagnostic("MiniMap: building discovery allocation unavailable: canonical=%s; class remains eligible for a later pass; further allocation diagnostics suppressed for session", path.c_str());
            }
        }
    }
    void EndDraw() noexcept
    {
        if (!g_passOpen) return;
        if (g_drawThread != std::this_thread::get_id())
        { LOG_ERROR("MiniMap: building draw pass must end on its starting UI thread"); return; }
        // Caller still holds LifecycleMutex. Slots are bounded before discovery;
        // moving strings/payload owners cannot allocate. No I/O or pixel work;
        // Service cannot observe a partial close.
        for (size_t i = 0; i < g_pass.Count; ++i)
        {
            g_completed.Payloads[g_completed.Count] = std::move(g_pass.Payloads[i]);
            g_completed.Paths[g_completed.Count++] = std::move(g_pass.Paths[i]);
        }
        g_pass.Count = 0; g_passOpen = false;
    }
    void Service()
    {
        Names names;
        RetentionRetries retries;
        uint64_t serial, tick;
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            if (!Admitted(g_serial) || g_serviceBusy) return;
            g_serviceBusy = true; serial = g_serial;
            if (g_serviceTick != (std::numeric_limits<uint64_t>::max)()) ++g_serviceTick;
            tick = g_serviceTick;
            names.Count = g_completed.Count;
            for (size_t i = 0; i < names.Count; ++i)
            {
                names.Paths[i] = std::move(g_completed.Paths[i]);
                names.Payloads[i] = std::move(g_completed.Payloads[i]);
            }
            g_completed.Count = 0;
            for (size_t i = 0; i < g_retentionRetries.size(); ++i)
                if (g_retentionRetries[i] && g_retentionRetries[i]->NextTick <= tick)
                {
                    retries[i] = std::move(g_retentionRetries[i]);
                    g_retentionRetries[i].reset();
                }
        }
        ServiceGuard busy;
        const auto batch = A::BeginBatch(serial);
        struct Finish
        {
            const A::Batch& Batch;
            uint64_t Tick;
            ~Finish() { A::EndBatch(Batch, Tick); }
        } finish{batch, tick};
        for (auto& retry : retries)
        {
            if (!retry) continue;
            {
                std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                if (!Admitted(serial)) break;
                if (retry->Serial != serial) continue;
                const auto it = g_statuses->find(retry->Path);
                if (it == g_statuses->end() || it->second.Where != S::Location::FileCache) continue;
            }
            RetainDecoded(batch, retry->Path, retry->Sprite, serial, tick, retry->Attempts + 1, std::move(retry->Payload));
        }
        size_t servicedNames = 0, loadedSprites = 0, missingSprites = 0, directSprites = 0;
        for (;;)
        {
            servicedNames += names.Count;
            for (size_t i = 0; i < names.Count; ++i)
            {
                auto& path = names.Paths[i];
                {
                    std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                    if (!Admitted(serial)) break;
                    const auto it = g_statuses->find(path);
                    if (it == g_statuses->end() || it->second.Where != S::Location::PendingCache)
                    {
                        if (!g_invalidHandoffReported)
                        {
                            g_invalidHandoffReported = true;
                            SpriteDiagnostic("MiniMap: sprite completed-pass item ignored: canonical=%s reason=expected PendingCache status missing/changed; further invalid handoff diagnostics suppressed for session", path.c_str());
                        }
                        continue;
                    }
                }
                C::Sprite sprite;
                const auto& payload = names.Payloads[i];
                // Successful current-session capture already owns these exact
                // pixels. Warm-cache/fallback remains one unchanged read/decode.
                const auto result = payload ? C::LoadResult{C::LoadStatus::Loaded, "capture payload"}
                    : C::TryLoad(path, sprite); // NO lifecycle lock.
                {
                    std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
                    if (!Admitted(serial)) break;
                    const auto it = g_statuses->find(path);
                    if (it == g_statuses->end() || it->second.Where != S::Location::PendingCache)
                    {
                        if (!g_invalidHandoffReported)
                        {
                            g_invalidHandoffReported = true;
                            SpriteDiagnostic("MiniMap: sprite cache result ignored: canonical=%s reason=expected PendingCache status missing/changed after read; further invalid handoff diagnostics suppressed for session", path.c_str());
                        }
                        continue;
                    }
                    if (result.Status == C::LoadStatus::Missing)
                    {
                        ++missingSprites;
                        try { g_fifo->Add(path); it->second.Where = S::Location::Queued; }
                        catch (...) { it->second.Where = S::Location::Failed; SpriteDiagnostic("MiniMap: capture FIFO allocation failed for '%s'; failed for session", path.c_str()); }
                        continue;
                    }
                    if (result.Status != C::LoadStatus::Loaded)
                    {
                        // Explicit non-spinning policy; I/O errors never enqueue capture.
                        it->second.Where = S::Location::Failed;
                        SpriteDiagnostic("MiniMap: sprite TryLoad '%s' failed (%u, %s, 0x%08X); failed for session",
                            path.c_str(), unsigned(result.Status), result.Detail, unsigned(result.Error));
                        continue;
                    }
                    it->second.Where = S::Location::FileCache;
                }
                ++loadedSprites;
                if (payload) ++directSprites;
                RetainDecoded(batch, path, sprite, serial, tick, 1, payload);
            }
            // EndDraw may have closed more discoveries while TryLoad ran unlocked.
            // Take those already available names into the SAME CPU/GPU batch. The
            // empty handoff observation below is the publication boundary, not a
            // timer or a wait for future identities/captures. Bound total work by
            // the existing handoff capacity; overflow remains owned by g_completed.
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            if (!Admitted(serial) || servicedNames >= MaximumBatchNames || !g_completed.Count) break;
            names.Count = (std::min)(g_completed.Count, MaximumBatchNames - servicedNames);
            for (size_t i = 0; i < names.Count; ++i)
            {
                names.Paths[i] = std::move(g_completed.Paths[i]);
                names.Payloads[i] = std::move(g_completed.Payloads[i]);
            }
            g_completed.Count -= names.Count;
            for (size_t i = 0; i < g_completed.Count; ++i)
            {
                g_completed.Paths[i] = std::move(g_completed.Paths[i + names.Count]);
                g_completed.Payloads[i] = std::move(g_completed.Payloads[i + names.Count]);
            }
        }
        if (servicedNames)
            SpriteDiagnostic("MiniMap: sprite cache batch closed: session=%llu names=%zu loaded=%zu missing=%zu direct=%zu scope=available-handoffs; at most one atlas publication attempt follows",
                static_cast<unsigned long long>(serial), servicedNames, loadedSprites, missingSprites, directSprites);
        // Finish performs one layout/transfer at most for the entire available
        // batch, or services a pending retry without any cache reread. No disk
        // access, layout construction or GPU call occurs in EndDraw/UI rendering.
    }
    void EndSession()
    {
        MiniMapBuildingSchedulerInternal::StopSession();
        if (g_fifo) { std::string path; while (g_fifo->Remove(path)) {} }
        MiniMapSpriteQueues::Feedback feedback;
        while (g_feedback.Remove(feedback)) {}
        if (g_statuses) g_statuses->clear();
    }
    void Shutdown()
    {
        EndSession(); g_statuses = nullptr; g_fifo = nullptr;
    }
}

#endif
