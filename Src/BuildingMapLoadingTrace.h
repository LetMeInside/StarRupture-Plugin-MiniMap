// Emits synchronous BM-12.4D breadcrumbs on the game thread for one admitted session update.
#pragma once
#include "DiagnosticExports.h"
#if defined(_DEBUG) && defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "Services/Log.h"
#include <exception>
#include <type_traits>
#include <utility>

namespace MiniMap::Diagnostics
{
    /** Owns only thread-local diagnostic flags; never owns gameplay objects or scheduling state. */
    class BuildingMapLoadingTrace
    {
    public:
        /** Arms one callback and one active update after successful layer admission. */
        static void Arm() noexcept
        {
            pending_ = true;
            callbackPending_ = true;
        }
        /** Claims the first callback without consuming the first visible update. */
        static bool ClaimCallback() noexcept
        {
            const bool claimed = callbackPending_;
            callbackPending_ = false;
            return claimed;
        }
        /** Claims the first update with valid visible bounds; nested operations share its scope. */
        BuildingMapLoadingTrace() noexcept : claimed_(pending_ && !active_)
        {
            if (claimed_)
            {
                pending_ = false;
                active_ = true;
                Write("active update", "enter");
            }
        }
        /** Ends tracing on normal return, early return, or exception without changing gameplay. */
        ~BuildingMapLoadingTrace()
        {
            if (claimed_)
            {
                Write("active update", std::uncaught_exceptions() ? "unwind" : "leave");
                active_ = false;
            }
        }
        /** Writes a fixed coarse milestone only inside the claimed active update. */
        static void Mark(const char* operation) noexcept
        {
            if (active_) Write(operation, "progress");
        }
        /** Invokes an existing operation exactly once and reports its return or unwind immediately. */
        template<class Operation>
        static decltype(auto) Run(const char* name, Operation&& operation, bool force = false)
        {
            const bool tracing = (active_ || force) && !writing_;
            if (tracing) Write(name, "enter");
            try
            {
                if constexpr (std::is_void_v<std::invoke_result_t<Operation>>)
                {
                    std::forward<Operation>(operation)();
                    if (tracing) Write(name, "leave");
                }
                else
                {
                    auto result = std::forward<Operation>(operation)();
                    if (tracing) Write(name, result ? "leave true" : "leave false");
                    return result;
                }
            }
            catch (...)
            {
                if (tracing) Write(name, "unwind");
                throw;
            }
        }
        /** Emits Section coordinates around every attempt, including skipped and failed attempts. */
        template<class Operation>
        static void Section(long long x, long long y, Operation&& operation)
        {
            if (!active_)
            {
                std::forward<Operation>(operation)();
                return;
            }
            char name[96];
            std::snprintf(name, sizeof(name), "Section (%lld,%lld) processing attempt", x, y);
            Run(name, std::forward<Operation>(operation));
        }
    private:
        /** Calls the existing neutral logger immediately; suppresses diagnostic logger recursion. */
        static void Write(const char* name, const char* phase) noexcept
        {
            if (writing_) return;
            writing_ = true;
            if (auto* log = Services::TryLog())
                Services::Logf(*log, Services::Level::Info, "MiniMap: BM124D %s %s", name, phase);
            writing_ = false;
        }
        /** Prevents duplicate ownership of the active diagnostic scope. */
        BuildingMapLoadingTrace(const BuildingMapLoadingTrace&) = delete;
        /** Prevents reassignment of the active diagnostic scope. */
        BuildingMapLoadingTrace& operator=(const BuildingMapLoadingTrace&) = delete;
        bool claimed_ = false;
        inline static thread_local bool pending_ = false;
        inline static thread_local bool callbackPending_ = false;
        inline static thread_local bool active_ = false;
        inline static thread_local bool writing_ = false;
    };
}
#define BM124D_RUN(name, ...) ::MiniMap::Diagnostics::BuildingMapLoadingTrace::Run(name, [&]() { return (__VA_ARGS__); })
#define BM124D_MARK(name) ::MiniMap::Diagnostics::BuildingMapLoadingTrace::Mark(name)
#define BM124D_SECTION(x, y, ...) ::MiniMap::Diagnostics::BuildingMapLoadingTrace::Section(x, y, [&]() { __VA_ARGS__; })
#else
#define BM124D_RUN(name, ...) (__VA_ARGS__)
#define BM124D_MARK(name) ((void)0)
#define BM124D_SECTION(x, y, ...) __VA_ARGS__
#endif
