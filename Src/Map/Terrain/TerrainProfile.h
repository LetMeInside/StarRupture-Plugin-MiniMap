#pragma once

#include "DiagnosticExports.h"

// All timing storage and implementation are development-only.

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
#include "plugin_helpers.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <utility>

namespace MiniMapTerrainProfile
{
    using Clock = std::chrono::steady_clock;
    static_assert(Clock::is_steady);
    enum class Phase : size_t
    {
        Source, Readiness, PlatformQuery, PlatformValidation, MipQuery, MipValidation,
        Cache, CachePath, CacheHeader, CacheRead, Decode, Upload, BulkCopy,
        CacheWrite, StoreExtraction, RequestedExtraction, SnapshotWait, SnapshotHold,
        PublishWait, PublishHold, Retirement, Free, Publication, ExistingLog, Count
    };
    inline constexpr const char* PhaseNames[] = {
        "source", "ready", "pq", "pv", "mq", "mv",
        "cache", "path", "hdr", "read", "decode", "upload", "bulk",
        "write", "sx", "rx", "sw", "sh",
        "pw", "ph", "retire", "free", "pub", "log"
    };
    static_assert(sizeof(PhaseNames) / sizeof(*PhaseNames) == size_t(Phase::Count));
    enum class Counter : size_t { Attempts, New, Reuse, Hit, Miss, Failure, UploadOK, UploadFailure, Retired, Count };
    struct Metric
    {
        uint64_t Calls = 0;
        double Total = 0, Max = 0, Exclusive = 0;
        void Add(double total, double exclusive) noexcept
        { ++Calls; Total += total; Max = (std::max)(Max, total); Exclusive += exclusive; }
        void Merge(const Metric& m) noexcept
        { Calls += m.Calls; Total += m.Total; Max = (std::max)(Max, m.Max); Exclusive += m.Exclusive; }
    };
    struct Record
    {
        std::array<Metric, size_t(Phase::Count)> Phases{};
        std::array<uint64_t, size_t(Counter::Count)> Counts{};
        Metric Reconcile{}, Residual{};
        uint64_t ResidentOnly = 0;
        uint64_t Unwound = 0;
        void Merge(const Record& r) noexcept
        {
            for (size_t i = 0; i < Phases.size(); ++i) Phases[i].Merge(r.Phases[i]);
            for (size_t i = 0; i < Counts.size(); ++i) Counts[i] += r.Counts[i];
            Reconcile.Merge(r.Reconcile); Residual.Merge(r.Residual); ResidentOnly += r.ResidentOnly;
            Unwound += r.Unwound;
        }
    };
    // All lifecycle/aggregation runs on the existing terrain game thread.
    // Records contain numbers only; no native objects, textures or per-chunk history.
    struct State
    {
        bool Running = false, FirstSeen = false, Meaningful = false;
        uint64_t Epoch = 0;
        Clock::time_point Since{};
        std::array<Record, 4> Groups{}; // first successful / later successful / no attempt / failed batch
        Record Worst{};
        size_t WorstGroup = 0;
    };
    inline State StateData{};
    inline thread_local Record* Current = nullptr;
    class Scope;
    inline thread_local Scope* Top = nullptr;
    inline double Milliseconds(Clock::duration d) noexcept
    { return std::chrono::duration<double, std::milli>(d).count(); }

    class Scope
    {
        Record* Target = nullptr;
        Scope* Parent = nullptr;
        Phase Kind;
        Clock::time_point Start{};
        double Children = 0;
    public:
        explicit Scope(Phase phase) noexcept : Kind(phase)
        {
            if (Current) { Target = Current; Parent = Top; Top = this; Start = Clock::now(); }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        void End() noexcept
        {
            if (!Target) return;
            const double elapsed = Milliseconds(Clock::now() - Start);
            Target->Phases[size_t(Kind)].Add(elapsed, elapsed - Children);
            if (Parent) Parent->Children += elapsed;
            Top = Parent; Target = nullptr;
        }
        ~Scope() noexcept { End(); }
    };
    template<class F> decltype(auto) Measure(Phase phase, F&& function)
    {
        Scope scope(phase);
        return std::forward<F>(function)();
    }
    inline void Count(Counter c) noexcept
    { if (Current) ++Current->Counts[size_t(c)]; }
    struct CacheResult
    {
        Counter Result = Counter::Failure;
        ~CacheResult() noexcept { Count(Result); }
    };
    struct UploadResult
    {
        bool Success = false;
        ~UploadResult() noexcept { Count(Success ? Counter::UploadOK : Counter::UploadFailure); }
    };
    inline void Reset() noexcept
    {
        const auto epoch = StateData.Epoch + 1;
        StateData = {}; StateData.Epoch = epoch;
        StateData.Running = true; StateData.Since = Clock::now();
    }
    inline void Stop() noexcept
    { StateData.Running = false; ++StateData.Epoch; }

    // Formatting uses bounded stack storage, only at the reporting boundary.
    struct Text
    {
        // Raw IPluginLogger::Log avoids the formatted logger's 1024-byte buffer.
        // Stay below its downstream 4096-byte line buffer, including plugin prefix.
        char Buffer[3960]{};
        size_t Used = 0;
        bool Truncated = false;
        template<class... A> void Append(const char* format, A... args) noexcept
        {
            const size_t left = sizeof(Buffer) - Used;
            if (left <= 1) { Truncated = true; return; }
            const int n = std::snprintf(Buffer + Used, left, format, args...);
            if (n < 0 || size_t(n) >= left) Truncated = true;
            if (n > 0) Used += (std::min)(size_t(n), left - 1);
        }
        void AddRecord(const char* label, const Record& r) noexcept
        {
            if (!r.Reconcile.Calls) return;
            Append(" %s{R=%llu/%.3g/%.3g residual=%.3g/%.3g residentOnly=%llu",
                label, r.Reconcile.Calls, r.Reconcile.Total, r.Reconcile.Max,
                r.Residual.Total, r.Residual.Max, r.ResidentOnly);
            Append(" c=");
            for (size_t i = 0; i < r.Counts.size(); ++i)
                Append(i ? "/%llu" : "%llu", r.Counts[i]);
            if (r.Unwound) Append(" unwound=%llu", r.Unwound);
            for (size_t i = 0; i < r.Phases.size(); ++i)
            {
                const auto& m = r.Phases[i];
                if (m.Calls) Append(" %s=%llu/%.3g/%.3g/%.3g", PhaseNames[i], m.Calls, m.Total, m.Max, m.Exclusive);
            }
            Append("}");
        }
        void Emit() noexcept
        {
            if (Truncated)
            {
                constexpr char suffix[] = " TRUNCATED";
                const size_t offset = sizeof(Buffer) - sizeof(suffix);
                for (size_t i = 0; i < sizeof(suffix); ++i) Buffer[offset + i] = suffix[i];
            }
            if (auto* self = GetSelf(); self && self->logger && self->logger->Log)
                self->logger->Log(PluginLogLevel::Info, self, Buffer);
        }
    };
    inline void Report(Clock::time_point now) noexcept
    {
        auto& s = StateData;
        if (now - s.Since < std::chrono::seconds(10)) return;
        if (s.Meaningful)
        {
            const auto start = Clock::now();
            constexpr const char* labels[] = { "first", "later", "noAttempt", "failedBatch" };
            Text text;
            text.Append("MiniMap: TerrainProfile: intervalMs=%.3f closed-batches ms phase=n/inclusiveTotal/inclusiveMax/exclusiveTotal R=n/total/max residual=total/max c=attempt/new/reuse/hit/miss/failure/uploadOK/uploadFail/retired parent-not-additive",
                Milliseconds(now - s.Since));
            for (size_t i = 0; i < s.Groups.size(); ++i) text.AddRecord(labels[i], s.Groups[i]);
            text.Emit();
            // The first summary's logger time is included here; the second logger
            // call and final reset are necessarily outside this overhead sample.
            if (s.Worst.Reconcile.Calls)
            {
                Text worst;
                worst.Append("MiniMap: TerrainProfile: worst-acquisition reportPrepMs=%.3f (excludes-this-log)",
                    Milliseconds(Clock::now() - start));
                worst.AddRecord(labels[s.WorstGroup], s.Worst);
                worst.Emit();
            }
        }
        s.Groups = {}; s.Worst = {}; s.Meaningful = false;
        s.Since = Clock::now(); // cadence excludes report emission; not acquisition time
    }
    class Batch
    {
        Record Data{};
        Record* Previous = nullptr;
        Scope* PreviousTop = nullptr;
        Clock::time_point Start{};
        uint64_t Epoch = 0;
        int Exceptions = 0;
        bool Active = false;
    public:
        Batch() noexcept
        {
            if (StateData.Running)
            {
                Active = true; Epoch = StateData.Epoch;
                Exceptions = std::uncaught_exceptions();
                Previous = Current; PreviousTop = Top; Current = &Data; Top = nullptr;
                Start = Clock::now();
            }
        }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        ~Batch() noexcept
        {
            if (!Active) return;
            const auto end = Clock::now();
            Current = Previous; Top = PreviousTop;
            if (!StateData.Running || Epoch != StateData.Epoch) return;
            const double total = Milliseconds(end - Start);
            double accounted = 0;
            for (const auto& m : Data.Phases) accounted += m.Exclusive;
            Data.Reconcile.Add(total, total);
            // Signed residual deliberately exposes accounting errors; do not clamp.
            Data.Residual.Add(total - accounted, total - accounted);
            const bool acquired = Data.Counts[size_t(Counter::New)] != 0;
            const bool attempted = Data.Counts[size_t(Counter::Attempts)] != 0;
            const bool unwound = std::uncaught_exceptions() > Exceptions;
            Data.Unwound = unwound ? 1 : 0;
            const size_t group = unwound ? 3 : (acquired ? (StateData.FirstSeen ? 1 : 0) : (attempted ? 3 : 2));
            if (acquired && !unwound) StateData.FirstSeen = true;
            if (!attempted && !unwound && Data.Counts[size_t(Counter::Reuse)]) Data.ResidentOnly = 1;
            StateData.Groups[group].Merge(Data);
            if (Data.Counts[size_t(Counter::Retired)]) StateData.Meaningful = true;
            if (attempted)
            {
                StateData.Meaningful = true;
                if (total > StateData.Worst.Reconcile.Total) { StateData.Worst = Data; StateData.WorstGroup = group; }
            }
            // First local in ReconcileViewport, destroyed last: vectors and all
            // lock guards have already unwound. Never reports from a chunk loop.
            if (!Previous) Report(end);
        }
    };
}
#endif
